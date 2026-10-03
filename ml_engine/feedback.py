"""Strict bounded v1 feedback ingestion into a disk-backed audit index."""
from __future__ import annotations

from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import sqlite3

from .features import FeatureSchema, validate_snapshot

MAX_EVENT_BYTES = 16384
FIELDS = {"record_version", "event_id", "broker_instance_id", "message_id", "event_type", "attempt_id",
          "collected_at_utc", "routing", "settlement_operation", "trigger", "outcome", "dlq_reason",
          "retry_count", "processing_time_ms", "label_status", "elapsed_since_arrival_ms"}
ROUTING_FIELDS = {"feature_schema_version", "routing_policy_version", "model_version", "mode", "features",
                  "feature_validity", "predicted_processing_time_ms", "predicted_bucket", "ingress_priority",
                  "fallback_reason", "inference_elapsed_ms"}
SEGMENT = re.compile(r"feedback-[0-9a-f]{32}-[0-9]{20}\.jsonl", re.ASCII)


def _pairs(items: list[tuple]) -> dict:
    output = {}
    for key, value in items:
        if key in output: raise ValueError(f"duplicate JSON key: {key}")
        output[key] = value
    return output


def strict_json(data: bytes | str) -> dict:
    def invalid(value: str) -> None: raise ValueError(f"nonfinite JSON: {value}")
    return json.loads(data, object_pairs_hook=_pairs, parse_constant=invalid)


def _integer(value: object, low: int, high: int) -> bool:
    return type(value) is int and low <= value <= high


def _identifier(value: object) -> bool:
    return isinstance(value, str) and 0 < len(value.encode("utf8")) <= 128 and not any(ord(c) < 32 for c in value)


def validate_record(record: dict, schemas: dict[str, FeatureSchema], lease_ms: int) -> dict:
    if not isinstance(record, dict) or set(record) != FIELDS or type(record["record_version"]) is not int or record["record_version"] != 1:
        raise ValueError("unsupported feedback record/fields/version")
    for key in ("message_id", "broker_instance_id"):
        if not _identifier(record[key]): raise ValueError(f"invalid {key}")
    if not isinstance(record["event_id"], str): raise ValueError("invalid event identity")
    match = re.fullmatch(re.escape(record["broker_instance_id"]) + r":([1-9][0-9]{0,19})", record["event_id"])
    if not match or int(match[1]) > 2**64 - 1: raise ValueError("invalid event identity")
    attempt = record["attempt_id"]
    if attempt is not None and (not isinstance(attempt, str) or not re.fullmatch(r"[1-9][0-9]{0,19}", attempt, re.ASCII)
                                or int(attempt) > 2**64 - 1):
        raise ValueError("invalid attempt identity")
    if not isinstance(record["collected_at_utc"], str) or not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", record["collected_at_utc"], re.ASCII):
        raise ValueError("invalid timestamp")
    datetime.strptime(record["collected_at_utc"], "%Y-%m-%dT%H:%M:%SZ")
    if not _integer(record["retry_count"], 0, 2**32 - 1) or not _integer(record["elapsed_since_arrival_ms"], 0, 2**63 - 1):
        raise ValueError("invalid feedback counters")
    measurement = record["processing_time_ms"]
    if measurement is not None and not _integer(measurement, -2**63, 2**63 - 1):
        raise ValueError("invalid measurement")
    routing = record["routing"]
    if not isinstance(routing, dict) or set(routing) != ROUTING_FIELDS:
        raise ValueError("invalid routing fields")
    for key in ("feature_schema_version", "routing_policy_version"):
        if not _identifier(routing[key]): raise ValueError("invalid routing version")
    if routing["model_version"] is not None and not _identifier(routing["model_version"]):
        raise ValueError("invalid model version")
    if routing["mode"] not in ("disabled", "shadow", "predictive") or routing["feature_validity"] not in ("valid", "feature_limit"):
        raise ValueError("invalid routing mode/validity")
    if not _integer(routing["ingress_priority"], 0, 254): raise ValueError("invalid ingress priority")
    if routing["predicted_bucket"] is not None and not _integer(routing["predicted_bucket"], 0, 254):
        raise ValueError("invalid predicted bucket")
    for key in ("predicted_processing_time_ms", "inference_elapsed_ms"):
        value = routing[key]
        if value is not None and (type(value) not in (int, float) or not __import__("math").isfinite(value) or value < 0):
            raise ValueError("invalid routing measurement")
    if routing["fallback_reason"] not in (None, "timeout", "unavailable", "overloaded", "unready", "incompatible_version",
                                           "invalid_prediction", "feature_limit", "client_error"):
        raise ValueError("invalid fallback reason")
    schema = schemas.get(routing["feature_schema_version"])
    if schema is None: raise ValueError("unregistered feature schema")
    if routing["feature_validity"] == "valid":
        validate_snapshot(schema, routing["feature_schema_version"], routing["features"])
    elif routing["features"] is not None:
        raise ValueError("feature-limit snapshot must be null")
    trigger, operation, outcome, reason = (record[key] for key in ("trigger", "settlement_operation", "outcome", "dlq_reason"))
    if record["event_type"] == "ingress":
        if trigger != "submit" or any(value is not None for value in (attempt, operation, outcome, reason, measurement)) or record["retry_count"]:
            raise ValueError("invalid ingress event")
    elif record["event_type"] == "outcome":
        if trigger not in ("settlement", "lease_expiry", "pull_expiry", "ttl_sweep") or outcome not in ("ack", "retry", "dlq"):
            raise ValueError("invalid outcome event")
        if (outcome == "dlq") != (reason is not None) or reason not in (None, "TTL_EXPIRED", "MAX_RETRIES_EXCEEDED", "PROCESSING_ERROR"):
            raise ValueError("invalid DLQ disposition")
        if trigger == "settlement":
            if attempt is None or operation not in ("ack", "nack") or measurement is None:
                raise ValueError("invalid settlement observation")
            if outcome == "ack" and operation != "ack" or outcome == "retry" and operation != "nack":
                raise ValueError("operation/outcome conflict")
        else:
            if operation is not None or measurement is not None or outcome == "ack":
                raise ValueError("invalid expiry observation")
            if trigger == "lease_expiry" and attempt is None:
                raise ValueError("lease expiry needs attempt")
            if trigger in ("pull_expiry", "ttl_sweep") and (attempt is not None or reason != "TTL_EXPIRED"):
                raise ValueError("invalid queued expiry")
    else: raise ValueError("invalid event type")
    expected = ("missing" if measurement is None else "negative" if measurement < 0 else
                "out_of_range" if measurement > lease_ms else "invalid_features" if routing["feature_validity"] != "valid" else
                "censored" if reason == "TTL_EXPIRED" else "failure" if operation == "nack" else
                "eligible" if outcome == "ack" else "missing")
    if record["label_status"] != expected: raise ValueError("label precedence mismatch")
    return record


def dimensions(record: dict) -> dict:
    measured = record["processing_time_ms"]
    return {"eligible": record["label_status"] == "eligible", "actual_outcome": record["outcome"],
            "failed_attempt": record["settlement_operation"] == "nack" or record["trigger"] == "lease_expiry",
            "censored": record["dlq_reason"] == "TTL_EXPIRED", "missing_measurement": measured is None,
            "measurement_status": record["label_status"] if record["label_status"] in
            ("missing", "negative", "out_of_range") else "valid",
            "feature_validity": record["routing"]["feature_validity"]}


class FeedbackIndex:
    """Disk-backed validated identities/groups; caller owns the scratch database lifetime."""
    def __init__(self, path: Path):
        self.db = sqlite3.connect(path)
        self.db.executescript("""
          PRAGMA cache_size=-4096;
          CREATE TABLE events (event_id TEXT PRIMARY KEY, instance TEXT, message TEXT, attempt TEXT,
            event_type TEXT, source TEXT, offset INTEGER, body TEXT, routing TEXT);
          CREATE INDEX messages ON events(instance,message);
        """)

    def close(self) -> None: self.db.close()

    def ingest(self, paths: list[Path], schemas: dict[str, FeatureSchema], lease_ms: int) -> dict:
        if not _integer(lease_ms, 1, 2**63 - 1): raise ValueError("lease must be a positive integer")
        stats = {"records": 0, "duplicates": 0, "sources": []}
        for path in sorted(paths):
            if not SEGMENT.fullmatch(path.name) or path.is_symlink() or not path.is_file():
                raise ValueError("only regular sealed feedback segments are accepted")
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                offset = 0
                while data := stream.readline(MAX_EVENT_BYTES + 2):
                    digest.update(data)
                    if not data.endswith(b"\n") or len(data) > MAX_EVENT_BYTES + 1:
                        raise ValueError(f"incomplete/oversized record: {path}:{offset}")
                    record = validate_record(strict_json(data), schemas, lease_ms)
                    body = json.dumps(record, sort_keys=True, separators=(",", ":"), allow_nan=False)
                    previous = self.db.execute("SELECT body FROM events WHERE event_id=?", (record["event_id"],)).fetchone()
                    if previous:
                        if previous[0] != body: raise ValueError("conflicting duplicate event identity")
                        stats["duplicates"] += 1
                    else:
                        routing = json.dumps(record["routing"], sort_keys=True, separators=(",", ":"))
                        other = self.db.execute("SELECT routing FROM events WHERE instance=? AND message=? LIMIT 1",
                                                (record["broker_instance_id"], record["message_id"])).fetchone()
                        if other and other[0] != routing: raise ValueError("message routing context changed")
                        self.db.execute("INSERT INTO events VALUES (?,?,?,?,?,?,?,?,?)", (record["event_id"],
                            record["broker_instance_id"], record["message_id"], record["attempt_id"], record["event_type"],
                            path.name, offset, body, routing))
                        stats["records"] += 1
                    offset += len(data)
            stats["sources"].append({"file": path.name, "sha256": digest.hexdigest(), "bytes": offset})
            self.db.commit()
        previous_instance, previous_sequence = None, 0
        gaps = 0
        for instance, event_id in self.db.execute("SELECT instance,event_id FROM events ORDER BY instance,length(event_id),event_id"):
            if instance != previous_instance: previous_sequence = 0
            sequence = int(event_id.rsplit(":", 1)[1])
            gaps += sequence > previous_sequence + 1
            previous_instance, previous_sequence = instance, sequence
        stats["sequence_gap_ranges"] = gaps  # Possible incompleteness; not a count of lost message labels.
        return stats
