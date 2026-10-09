"""Validate and index exact-time feedback exports without pooling broker clocks."""
from __future__ import annotations

from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sqlite3
import tempfile

from .features import FeatureSchema
from .feedback import FIELDS, dimensions, strict_json, validate_record

FILES = ("events.jsonl", "attempts.jsonl", "messages.jsonl", "train.jsonl",
         "evaluation_ingress.jsonl", "evaluation_labels.jsonl", "manifest.json")
ENVELOPE = {"run", "split", "benchmark_transition_ns", "benchmark_publication_ns",
            "benchmark_time_ns", "benchmark_arrival_ns", "dimensions"}


def canonical(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def fingerprint(value: object) -> str:
    return hashlib.sha256(canonical(value).encode()).hexdigest()


def rows(path: Path):
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"expected regular dataset file: {path}")
    with path.open("rb") as stream:
        while line := stream.readline(65538):
            if len(line) > 65537 or not line.endswith(b"\n"):
                raise ValueError(f"incomplete/oversized dataset row: {path}")
            row = strict_json(line)
            if not isinstance(row, dict):
                raise ValueError("expected dataset object")
            yield row


def identity(row: dict) -> tuple:
    return row["run"], row["broker_instance_id"], row["message_id"]


class TemporalDataset:
    """Owns a temporary SQLite index; close or use as a context manager."""
    def __init__(self, root: Path, config: dict):
        self.scratch = tempfile.TemporaryDirectory(prefix="harbinger-model-data-")
        self.db = sqlite3.connect(Path(self.scratch.name) / "index.sqlite")
        try:
            self._load(root, config)
        except Exception:
            self.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        self.db.close()
        self.scratch.cleanup()

    def _load(self, root: Path, config: dict):
        self.hashes = {}
        for name in FILES:
            path = root / name
            if path.is_symlink() or not path.is_file():
                raise ValueError(f"missing/irregular export: {name}")
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(65536), b""):
                    digest.update(chunk)
            self.hashes[name] = digest.hexdigest()
        self.fingerprint = fingerprint(self.hashes)
        self.manifest = strict_json((root / "manifest.json").read_bytes())
        manifest = self.manifest
        if (type(manifest.get("artifact_version")) is not int or manifest.get("artifact_version") != 1 or
                manifest.get("valid_for_learning") is not True or
                manifest.get("ordering_scope") != "independent per-run monotonic clock origins" or
                manifest.get("tie_rule") != "ingress before equal-time feedback; learn only strictly earlier labels" or
                manifest.get("availability") != "broker publication after writer admission attempt; durable filesystem/learner consumption latency is not modeled" or
                manifest.get("train_fraction") != config["train_fraction"]):
            raise ValueError("invalid/incompatible learning manifest or chronology")
        if (not isinstance(manifest.get("stats"), dict) or
                any(type(value) is not int or value < 0 for value in manifest["stats"].values())):
            raise ValueError("invalid dataset coverage counters")
        forbidden = ("telemetry_loss", "observation_drops", "unresolved", "attempt_ordinal_gaps",
                     "invalid_runs", "missing_or_extra_feedback_records", "unresolved_event_times",
                     "missing_ingress", "missing_terminal")
        if any(manifest["stats"].get(key, 0) for key in forbidden):
            raise ValueError("incomplete dataset coverage")
        self.sources = {}
        for source in manifest["sources"]:
            name = source["run"]
            if name in self.sources or source.get("sequence_gap_ranges", 0):
                raise ValueError("duplicate run or feedback sequence gaps")
            if not re.fullmatch(r"[a-z0-9-]+-s[0-9]+-r[0-9]+-disabled", name):
                raise ValueError("unsupported run identity")
            for key in ("trace_sha256", "sidecar_sha256", "run_metadata_sha256"):
                if not re.fullmatch(r"[0-9a-f]{64}", source[key]):
                    raise ValueError("invalid provenance fingerprint")
            self.sources[name] = source
        if not self.sources:
            raise ValueError("empty dataset")
        schema = FeatureSchema.from_dict(config["schema"])
        self.db.executescript("""
          PRAGMA cache_size=-4096;
          CREATE TABLE events(run TEXT, instance TEXT, message TEXT, event TEXT, kind TEXT,
            time INTEGER, split TEXT, body TEXT, PRIMARY KEY(run,event));
          CREATE INDEX groups ON events(run,instance,message);
          CREATE INDEX chronology ON events(run,time,kind,event);
          CREATE TABLE expected(view TEXT, key TEXT, body TEXT, PRIMARY KEY(view,key));
          CREATE TABLE seen(view TEXT, key TEXT, PRIMARY KEY(view,key));
        """)
        self.audit = Counter()
        for row in rows(root / "events.jsonl"):
            if set(row) != FIELDS | ENVELOPE:
                raise ValueError("unsupported export event fields")
            run, instance, message = identity(row)
            if run not in self.sources or row["split"] not in ("train", "evaluation", "embargo"):
                raise ValueError("unregistered run or unresolved partition")
            validate_record({k: row[k] for k in FIELDS}, {schema.version: schema}, config["lease_ms"])
            if row["dimensions"] != dimensions(row):
                raise ValueError("audit dimension mismatch")
            time, arrival, transition, publication = (row[k] for k in
                ("benchmark_time_ns", "benchmark_arrival_ns", "benchmark_transition_ns", "benchmark_publication_ns"))
            if (any(type(x) is not int or not 0 <= x < 2**63 for x in (time, arrival, transition, publication)) or
                    not arrival <= transition <= publication or
                    time != (arrival if row["event_type"] == "ingress" else publication)):
                raise ValueError("invalid exact event chronology")
            body = canonical(row)
            prior = self.db.execute("SELECT body FROM events WHERE run=? AND event=?", (run, row["event_id"])).fetchone()
            if prior:
                if prior[0] != body:
                    raise ValueError("conflicting duplicate event")
                self.audit["identical_duplicates"] += 1
                continue
            self.db.execute("INSERT INTO events VALUES(?,?,?,?,?,?,?,?)", (run, instance, message,
                row["event_id"], row["event_type"], time, row["split"], body))
            if row["event_type"] == "outcome":
                for key, value in row["dimensions"].items():
                    if value is True:
                        self.audit[key] += 1
                self._expect("attempts", (run, row["event_id"]), row)
                if row["label_status"] == "eligible" and row["split"] in ("train", "evaluation"):
                    label = {k: row[k] for k in ("run", "broker_instance_id", "message_id", "split", "attempt_id")}
                    label.update(label_available_ns=time, arrival_ns=arrival, processing_time_ms=row["processing_time_ms"],
                        feature_schema_version=schema.version, routing_policy_version=row["routing"]["routing_policy_version"],
                        model_version=row["routing"]["model_version"], features=row["routing"]["features"],
                        stored_prediction_ms=row["routing"]["predicted_processing_time_ms"],
                        stored_predicted_bucket=row["routing"]["predicted_bucket"])
                    self._expect("train" if row["split"] == "train" else "evaluation_labels", identity(label), label)
        self._validate_groups(schema)
        for view in ("attempts", "messages", "train", "evaluation_ingress", "evaluation_labels"):
            for row in rows(root / f"{view}.jsonl"):
                key = (row["run"], row["event_id"]) if view == "attempts" else identity(row)
                key = canonical(key)
                expected = self.db.execute("SELECT body FROM expected WHERE view=? AND key=?", (view, key)).fetchone()
                if not expected or expected[0] != canonical(row):
                    raise ValueError(f"{view} differs from validated audit events")
                self.db.execute("INSERT OR IGNORE INTO seen VALUES(?,?)", (view, key))
            count = self.db.execute("SELECT COUNT(*) FROM expected WHERE view=?", (view,)).fetchone()[0]
            actual = self.db.execute("SELECT COUNT(*) FROM seen WHERE view=?", (view,)).fetchone()[0]
            if count != actual:
                raise ValueError(f"missing {view} associations")
        for run, source in self.sources.items():
            count = self.db.execute("SELECT COUNT(*) FROM events WHERE run=?", (run,)).fetchone()[0]
            if count != source["records"]:
                raise ValueError("source event count mismatch")
            instances = self.db.execute("SELECT COUNT(DISTINCT instance) FROM events WHERE run=?", (run,)).fetchone()[0]
            if instances != 1:
                raise ValueError("an independent run must contain exactly one broker clock/instance")
            minimum, maximum = self.db.execute("""SELECT
                MIN(CAST(substr(event,length(instance)+2) AS INTEGER)),
                MAX(CAST(substr(event,length(instance)+2) AS INTEGER)) FROM events WHERE run=?""", (run,)).fetchone()
            if minimum != 1 or maximum != count:
                raise ValueError("feedback event sequence gaps")
            train_max = self.db.execute("SELECT MAX(time) FROM events WHERE run=? AND split='train'", (run,)).fetchone()[0]
            eval_min = self.db.execute("SELECT MIN(time) FROM events WHERE run=? AND split='evaluation' AND kind='ingress'", (run,)).fetchone()[0]
            if train_max is not None and eval_min is not None and train_max >= eval_min:
                raise ValueError("training feedback crosses evaluation boundary")
        self.db.commit()

    def _expect(self, view: str, key: tuple, row: dict):
        try:
            self.db.execute("INSERT INTO expected VALUES(?,?,?)", (view, canonical(key), canonical(row)))
        except sqlite3.IntegrityError as exc:
            raise ValueError("duplicate message ingress/eligible label") from exc

    def _validate_groups(self, schema):
        for run, instance, message in self.db.execute("SELECT DISTINCT run,instance,message FROM events"):
            group = [strict_json(body) for (body,) in self.db.execute(
                "SELECT body FROM events WHERE run=? AND instance=? AND message=? LIMIT 103", (run, instance, message))]
            if len(group) > 102:
                raise ValueError("unbounded attempts")
            ingress = [r for r in group if r["event_type"] == "ingress"]
            terminal = [r for r in group if r["outcome"] in ("ack", "dlq")]
            if len(ingress) != 1 or len(terminal) != 1:
                raise ValueError("missing/multiple ingress or terminal")
            first = ingress[0]
            if any(r["routing"] != first["routing"] or r["split"] != first["split"] or
                   r["benchmark_arrival_ns"] != first["benchmark_arrival_ns"] for r in group):
                raise ValueError("immutable ingress context or partition changed")
            attempts = sorted(int(r["attempt_id"]) for r in group if r["attempt_id"] is not None)
            if attempts != list(range(1, len(attempts) + 1)):
                raise ValueError("duplicate/gapped attempt outcomes")
            if any(r["benchmark_transition_ns"] > terminal[0]["benchmark_transition_ns"] for r in group):
                raise ValueError("outcome after terminal")
            key = {"run": run, "broker_instance_id": instance, "message_id": message, "split": first["split"]}
            self._expect("messages", (run, instance, message), {**key, "arrival_ns": first["benchmark_arrival_ns"],
                "outcomes": len(group) - 1, "terminal": terminal[0]["outcome"]})
            if first["split"] == "evaluation":
                self._expect("evaluation_ingress", (run, instance, message), {**key,
                    "arrival_ns": first["benchmark_arrival_ns"], "features": first["routing"]["features"],
                    "feature_schema_version": schema.version})
            self.audit["messages"] += 1
            self.audit[first["split"]] += 1

    def events(self, run: str, delay_ns: int = 0):
        if run not in self.sources or type(delay_ns) is not int or delay_ns < 0:
            raise ValueError("invalid run/delay")
        maximum = self.db.execute("SELECT MAX(time) FROM events WHERE run=?", (run,)).fetchone()[0]
        if maximum + delay_ns >= 2**63:
            raise ValueError("unrepresentable delayed event time")
        # Labels are withheld beyond the measured publication time in sensitivity runs.
        query = """SELECT body FROM events WHERE run=? ORDER BY
                   time + CASE WHEN kind='outcome' THEN ? ELSE 0 END,
                   CASE WHEN kind='ingress' THEN 0 ELSE 1 END, length(event),event"""
        for (body,) in self.db.execute(query, (run, delay_ns)):
            row = strict_json(body)
            if row["event_type"] == "outcome":
                row["benchmark_time_ns"] += delay_ns
            yield row
