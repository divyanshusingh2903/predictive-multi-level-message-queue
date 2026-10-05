"""Hand-checkable feedback fixtures shared by contract and optional model tests."""
import copy
import json
from pathlib import Path

from ml_engine.dataset import canonical
from ml_engine.feedback import dimensions

ROOT = Path(__file__).resolve().parents[2]
RUN = "fixture-s101-r0-disabled"


def config():
    result = json.loads((ROOT / "ml_engine/configs/online-comparison-v1.json").read_text())
    result["policy"]["ready_labels"] = 1
    result["group_min_labels"] = 1
    result["candidates"] = ["global_mean", "job_mean"]
    result["required_cases"] = ["fixture"]
    result["seeds"] = [101]
    result["repeats"] = 1
    result["minimum_evaluation_labels"] = 1
    result["profiling"]["repeats"] = 1
    result["availability_delay_sensitivity_ms"] = []
    return result


def snapshot(job="class0"):
    return {"payload_size_bytes": "16", "headers": {"job": job},
            "missing_reasons": {"job": "absent"} if job is None else {}}


def events():
    fixture = json.loads((ROOT / "tests/fixtures/ml/online_replay_v1.json").read_text())
    template = json.loads((ROOT / "tests/fixtures/ml/feedback_v1.json").read_text())
    output = []
    for i, spec in enumerate(fixture["messages"]):
        row = copy.deepcopy(template)
        row.update(run=RUN, split=spec["split"], message_id=spec["id"], event_id=f"fixture-instance:{2*i+1}",
                   event_type="ingress", attempt_id=None, settlement_operation=None, trigger="submit",
                   outcome=None, dlq_reason=None, processing_time_ms=None, label_status="missing",
                   benchmark_arrival_ns=spec["arrival"], benchmark_time_ns=spec["arrival"],
                   benchmark_transition_ns=spec["arrival"], benchmark_publication_ns=spec["arrival"])
        row["routing"].update(feature_schema_version="synthetic-job-v1", features=snapshot())
        row["dimensions"] = dimensions(row)
        output.append(row)
        label = copy.deepcopy(row)
        label.update(event_id=f"fixture-instance:{2*i+2}", event_type="outcome", attempt_id="1",
                     settlement_operation="ack", trigger="settlement", outcome="ack", label_status="eligible",
                     processing_time_ms=spec["duration"], benchmark_time_ns=spec["available"],
                     benchmark_transition_ns=spec["available"], benchmark_publication_ns=spec["available"])
        label["dimensions"] = dimensions(label)
        output.append(label)
    return sorted(output, key=lambda r: (r["benchmark_time_ns"], r["event_type"] != "ingress", r["event_id"]))


def write_export(root, records=None):
    records = events() if records is None else records
    streams = {name: [] for name in ("events", "attempts", "messages", "train", "evaluation_ingress", "evaluation_labels")}
    streams["events"] = records
    for ingress in records:
        if ingress["event_type"] != "ingress":
            continue
        group = [r for r in records if r["message_id"] == ingress["message_id"]]
        terminal = next(r for r in group if r["outcome"] in ("ack", "dlq"))
        key = {k: ingress[k] for k in ("run", "broker_instance_id", "message_id", "split")}
        streams["messages"].append({**key, "arrival_ns": ingress["benchmark_arrival_ns"],
                                    "outcomes": len(group)-1, "terminal": terminal["outcome"]})
        if ingress["split"] == "evaluation":
            streams["evaluation_ingress"].append({**key, "arrival_ns": ingress["benchmark_arrival_ns"],
                "features": ingress["routing"]["features"], "feature_schema_version": "synthetic-job-v1"})
    for row in records:
        if row["event_type"] != "outcome":
            continue
        streams["attempts"].append(row)
        if row["label_status"] == "eligible" and row["split"] in ("train", "evaluation"):
            streams["train" if row["split"] == "train" else "evaluation_labels"].append({
                **{k: row[k] for k in ("run", "broker_instance_id", "message_id", "split", "attempt_id")},
                "label_available_ns": row["benchmark_time_ns"], "arrival_ns": row["benchmark_arrival_ns"],
                "feature_schema_version": "synthetic-job-v1", "routing_policy_version": row["routing"]["routing_policy_version"],
                "model_version": None, "stored_prediction_ms": None, "stored_predicted_bucket": None,
                "features": row["routing"]["features"], "processing_time_ms": row["processing_time_ms"]})
    for name, values in streams.items():
        (root / f"{name}.jsonl").write_text("".join(canonical(r)+"\n" for r in values))
    manifest = {"artifact_version": 1, "valid_for_learning": True, "train_fraction": .5,
        "ordering_scope": "independent per-run monotonic clock origins",
        "tie_rule": "ingress before equal-time feedback; learn only strictly earlier labels",
        "availability": "broker publication after writer admission attempt; durable filesystem/learner consumption latency is not modeled",
        "stats": {}, "sources": [{"run": RUN, "records": len(records), "sequence_gap_ranges": 0,
        "trace_sha256": "a"*64, "sidecar_sha256": "b"*64, "run_metadata_sha256": "c"*64}]}
    (root / "manifest.json").write_text(canonical(manifest)+"\n")
