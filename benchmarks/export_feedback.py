"""Export message-grouped static feedback using exact benchmark timing sidecars."""
from __future__ import annotations

import argparse
import hashlib
from collections import Counter
import json
from pathlib import Path
import tempfile

from analysis.features import FeatureSchema
from analysis.feedback import FeedbackIndex, dimensions, strict_json
from .evaluate import dump, read_tsv
from .workloads import SCHEMA


def partition(arrival: int | None, label_times: list[int], cutoff: int) -> str:
    if arrival is None: return "unresolved"
    if arrival >= cutoff: return "evaluation"
    if not label_times: return "unresolved"
    if arrival < cutoff:
        return "train" if max(label_times) < cutoff else "embargo"
    return "evaluation"


def export_run(run: Path, output: Path, train_fraction: float = .6) -> dict:
    if not 0 < train_fraction < 1: raise ValueError("train fraction must be between zero and one")
    if output.exists(): raise ValueError("export output must be fresh")
    output.mkdir(parents=True)
    experiments = sorted(run.glob("*/run.json")) if (run / "manifest.json").exists() else [run / "run.json"]
    if not experiments: raise ValueError("no run artifacts")
    stats = Counter()
    provenance = []
    schema = FeatureSchema.from_dict(SCHEMA)
    with tempfile.TemporaryDirectory(prefix="harbinger-export-") as scratch:
        streams = {name: (output / f"{name}.jsonl").open("w") for name in
                   ("events", "attempts", "messages", "train", "evaluation_ingress", "evaluation_labels")}
        try:
            for index, run_file in enumerate(experiments):
                metadata = json.loads(run_file.read_text())
                if metadata["policy"] != "disabled": continue  # static production collection, no scheduling-selected dataset.
                if type(metadata["parameters"]["max_retries"]) is not int or not 1 <= metadata["parameters"]["max_retries"] <= 100:
                    raise ValueError("invalid benchmark attempt budget")
                directory = run_file.parent
                if hashlib.sha256((directory / "trace.tsv").read_bytes()).hexdigest() != metadata["trace_sha256"]:
                    raise ValueError("trace hash mismatch")
                store = FeedbackIndex(Path(scratch) / f"{index}.sqlite")
                try:
                    ingestion = store.ingest(list((directory / "feedback").glob("*.jsonl")),
                                             {schema.version: schema}, metadata["parameters"]["lease_ms"])
                    provenance.append({"run": directory.name, **ingestion, "trace_sha256": metadata["trace_sha256"],
                        "sidecar_sha256": hashlib.sha256((directory / "observations.tsv").read_bytes()).hexdigest(),
                        "run_metadata_sha256": hashlib.sha256(run_file.read_bytes()).hexdigest()})
                    stats["invalid_runs"] += not metadata["metrics"]["valid"]
                    stats["missing_or_extra_feedback_records"] += abs(ingestion["records"] - int(metadata["summary"]["feedback_written"]))
                    db = store.db
                    db.executescript("CREATE TABLE timing(message TEXT, attempt TEXT, kind TEXT, time INTEGER, arrival INTEGER, published INTEGER);"
                                     "CREATE INDEX timing_join ON timing(message,attempt,kind);")
                    for row in read_tsv(directory / "observations.tsv"):
                        db.execute("INSERT INTO timing VALUES(?,?,?,?,?,?)", (row["message_id"], row["attempt"], row["kind"],
                                   int(row["time_ns"]), int(row["arrival_ns"]), int(row["published_ns"])))
                    # Temporal split is frozen against the arrival trace, not eventual success times.
                    trace = (directory / "trace.tsv").read_text().splitlines()[1:]
                    cutoff = int(trace[min(len(trace) - 1, int(len(trace) * train_fraction))].split("\t")[1]) * 1000
                    stats["telemetry_loss"] += metadata["metrics"]["telemetry_loss"]
                    stats["observation_drops"] += metadata["metrics"]["observation_drops"]
                    groups = db.execute("SELECT DISTINCT instance,message FROM events ORDER BY instance,message")
                    for instance, message in groups:
                        if instance != metadata["summary"]["feedback_instance"]:
                            raise ValueError("feedback belongs to a different broker run")
                        count = db.execute("SELECT COUNT(*) FROM events WHERE instance=? AND message=?", (instance, message)).fetchone()[0]
                        if count > metadata["parameters"]["max_retries"] + 1:
                            raise ValueError("too many outcomes per message")
                        rows = [json.loads(item[0]) for item in db.execute("SELECT body FROM events WHERE instance=? AND message=?",
                                                                         (instance, message))]
                        # Memory is bounded per message by the finite declared retry budget; corrupt streams are rejected.
                        if len(rows) > metadata["parameters"]["max_retries"] + 1:
                            raise ValueError("too many outcomes per message")
                        times = []
                        for record in rows:
                            kind = "ingress" if record["event_type"] == "ingress" else (
                                "ttl" if record["dlq_reason"] == "TTL_EXPIRED" else "max_retries" if
                                record["dlq_reason"] else record["outcome"])
                            # Queued expiry has null attempt although delivery_count can be nonzero after retry.
                            if record["attempt_id"] is None:
                                matches = db.execute("SELECT time,arrival,published FROM timing WHERE message=? AND kind=?", (message, kind)).fetchall()
                            else:
                                matches = db.execute("SELECT time,arrival,published FROM timing WHERE message=? AND kind=? AND attempt=?",
                                                     (message, kind, record["attempt_id"])).fetchall()
                            record["benchmark_transition_ns"] = matches[0][0] if len(matches) == 1 else None
                            record["benchmark_publication_ns"] = matches[0][2] if len(matches) == 1 else None
                            record["benchmark_time_ns"] = (matches[0][1] if record["event_type"] == "ingress" else matches[0][2]) if len(matches) == 1 else None
                            if record["benchmark_time_ns"] is not None and record["benchmark_time_ns"] > int(metadata["summary"]["cutoff_ns"]):
                                record["benchmark_time_ns"] = None
                            record["benchmark_arrival_ns"] = matches[0][1] if len(matches) == 1 else None
                            record["dimensions"] = dimensions(record)
                            if record["event_type"] == "outcome" and record["benchmark_time_ns"] is not None:
                                times.append(record["benchmark_time_ns"])
                        arrival = next((row["benchmark_arrival_ns"] for row in rows if row["benchmark_arrival_ns"] is not None), None)
                        split = partition(arrival, times, cutoff)
                        unknown_times = any(row["benchmark_time_ns"] is None for row in rows)
                        if unknown_times:
                            stats["unresolved_event_times"] += 1
                            if split != "evaluation": split = "unresolved"
                        outcomes = [row for row in rows if row["event_type"] == "outcome"]
                        if sum(row["event_type"] == "ingress" for row in rows) > 1: raise ValueError("duplicate message ingress")
                        attempts = [row["attempt_id"] for row in outcomes if row["attempt_id"] is not None]
                        if len(attempts) != len(set(attempts)): raise ValueError("duplicate attempt outcome")
                        terminal = [row for row in outcomes if row["outcome"] in ("ack", "dlq")]
                        if len(terminal) > 1: raise ValueError("multiple terminal outcomes")
                        stats[split] += 1
                        stats["missing_ingress"] += not any(row["event_type"] == "ingress" for row in rows)
                        stats["missing_terminal"] += not bool(terminal)
                        stats["attempt_ordinal_gaps"] += any(value != position for position, value in enumerate(sorted(map(int, attempts)), 1))
                        # Ingress before feedback at equal time: prediction cannot consume equal-time labels.
                        rows.sort(key=lambda row: (row["benchmark_time_ns"] if row["benchmark_time_ns"] is not None else 2**63,
                                                   row["event_type"] != "ingress", row["event_id"]))
                        key = {"run": directory.name, "broker_instance_id": instance, "message_id": message, "split": split}
                        streams["messages"].write(json.dumps({**key, "arrival_ns": arrival, "outcomes": len(outcomes),
                            "terminal": terminal[0]["outcome"] if terminal else None}) + "\n")
                        if split == "evaluation":
                            streams["evaluation_ingress"].write(json.dumps({**key, "arrival_ns": arrival,
                                "features": rows[0]["routing"]["features"], "feature_schema_version": schema.version}) + "\n")
                        for row in rows:
                            streams["events"].write(json.dumps({**key, **row}) + "\n")
                            if row["event_type"] == "outcome":
                                streams["attempts"].write(json.dumps({**key, **row}) + "\n")
                                for name, value in row["dimensions"].items():
                                    if value is True: stats[name] += 1
                            if row["label_status"] == "eligible" and split in ("train", "evaluation") and row["benchmark_time_ns"] is not None:
                                label = {**key, "attempt_id": row["attempt_id"], "label_available_ns": row["benchmark_time_ns"],
                                    "arrival_ns": arrival, "feature_schema_version": schema.version,
                                    "routing_policy_version": row["routing"]["routing_policy_version"],
                                    "model_version": row["routing"]["model_version"],
                                    "stored_prediction_ms": row["routing"]["predicted_processing_time_ms"],
                                    "stored_predicted_bucket": row["routing"]["predicted_bucket"],
                                    "features": row["routing"]["features"], "processing_time_ms": row["processing_time_ms"]}
                                streams["train" if split == "train" else "evaluation_labels"].write(json.dumps(label) + "\n")
                                stats[f"{split}_labels"] += 1
                finally: store.close()
            # Order within each independent run; do not invent cross-broker clock alignment.
            for name in ("events", "attempts", "train", "evaluation_ingress", "evaluation_labels"):
                streams[name].close()
                import sqlite3
                with sqlite3.connect(Path(scratch) / f"sort-{name}.sqlite") as db:
                    db.execute("CREATE TABLE ordered (run TEXT,time INTEGER,tie INTEGER,body TEXT)")
                    with (output / f"{name}.jsonl").open() as stream:
                        for line in stream:
                            row = json.loads(line)
                            time = row.get("benchmark_time_ns", row.get("label_available_ns", row.get("arrival_ns")))
                            db.execute("INSERT INTO ordered VALUES(?,?,?,?)", (row["run"], time, row.get("event_type") != "ingress", line))
                    with (output / f"{name}.jsonl").open("w") as stream:
                        for (body,) in db.execute("SELECT body FROM ordered ORDER BY run,time IS NULL,time,tie,body"):
                            stream.write(body)
        finally:
            for stream in streams.values(): stream.close()
    result = {"artifact_version": 1, "train_fraction": train_fraction,
              "tie_rule": "ingress before equal-time feedback; learn only strictly earlier labels",
              "availability": "broker publication after writer admission attempt; durable filesystem/learner consumption latency is not modeled",
              "ordering_scope": "independent per-run monotonic clock origins",
              "stats": dict(stats), "sources": provenance,
              "valid_for_learning": bool(provenance) and not (stats["telemetry_loss"] or stats["observation_drops"] or
                  stats["unresolved"] or stats["attempt_ordinal_gaps"] or stats["invalid_runs"] or
                  stats["missing_or_extra_feedback_records"] or stats["unresolved_event_times"]),
              "selection_bias": "Only eligible successful Ack observations are training labels; failures/TTL/missing remain in audit streams."}
    dump(output / "manifest.json", result)
    return result


def export_legacy(feedback: Path, output: Path, schemas: dict, lease_ms: int) -> dict:
    """Audit legacy logs without inventing exact event times or temporal training splits."""
    if output.exists(): raise ValueError("export output must be fresh")
    output.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix="harbinger-audit-") as scratch:
        store = FeedbackIndex(Path(scratch) / "audit.sqlite")
        try:
            stats = store.ingest(list(feedback.glob("*.jsonl")), schemas, lease_ms)
            with (output / "events.jsonl").open("w") as events, (output / "attempts.jsonl").open("w") as attempts:
                for (body,) in store.db.execute("SELECT body FROM events ORDER BY source,offset"):
                    record = json.loads(body)
                    row = {**record, "dimensions": dimensions(record), "split": "unresolved"}
                    data = json.dumps(row) + "\n"
                    events.write(data)
                    if record["event_type"] == "outcome": attempts.write(data)
            with (output / "messages.jsonl").open("w") as messages:
                for instance, message, count in store.db.execute("SELECT instance,message,COUNT(*) FROM events GROUP BY instance,message"):
                    messages.write(json.dumps({"broker_instance_id": instance, "message_id": message,
                                               "events": count, "split": "unresolved"}) + "\n")
        finally: store.close()
    manifest = {"artifact_version": 1, "stats": stats, "valid_for_learning": False,
                "ordering": "segment generation/source offset only; asynchronous publication is not transition chronology",
                "chronology": "Feedback v1 alone cannot establish exact delayed-label availability or safe temporal splits."}
    dump(output / "manifest.json", manifest)
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--run", type=Path)
    inputs.add_argument("--feedback", type=Path)
    parser.add_argument("--schema", type=Path, action="append", default=[])
    parser.add_argument("--lease-ms", type=int)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--train-fraction", type=float, default=.6)
    args = parser.parse_args()
    if args.run:
        if args.schema or args.lease_ms: parser.error("benchmark schemas/lease come from run metadata")
        export_run(args.run, args.output, args.train_fraction)
    else:
        if not args.schema or not args.lease_ms: parser.error("legacy feedback requires --schema and --lease-ms")
        registry = {}
        for path in args.schema:
            schema = FeatureSchema.from_dict(strict_json(path.read_bytes()))
            if schema.version in registry: parser.error("duplicate schema registration")
            registry[schema.version] = schema
        export_legacy(args.feedback, args.output, registry, args.lease_ms)


if __name__ == "__main__": main()
