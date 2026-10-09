import copy
import json
from pathlib import Path
import tempfile
import unittest

from analysis.features import FeatureSchema
from analysis.feedback import FeedbackIndex, dimensions, strict_json, validate_record

FIXTURE = Path(__file__).resolve().parents[2] / "tests/fixtures/ml/feedback_v1.json"


class Feedback(unittest.TestCase):
    def setUp(self):
        self.record = json.loads(FIXTURE.read_text())
        self.schemas = {"features-v1": FeatureSchema("features-v1", ())}

    def test_valid_zero_and_dimensions(self):
        validate_record(self.record, self.schemas, 100)
        self.assertTrue(dimensions(self.record)["eligible"])
        row = copy.deepcopy(self.record)
        row.update(trigger="ttl_sweep", attempt_id=None, settlement_operation=None,
                   processing_time_ms=None, outcome="dlq", dlq_reason="TTL_EXPIRED", label_status="missing")
        validate_record(row, self.schemas, 100)
        self.assertTrue(dimensions(row)["censored"])
        self.assertTrue(dimensions(row)["missing_measurement"])

    def test_shadow_predictor_fallbacks_are_valid(self):
        shadow = json.loads(FIXTURE.with_name("feedback_v1_shadow.json").read_text())
        validate_record(shadow, self.schemas, 100)
        for reason in ("cold_key", "high_spread", "censored", "stale_key", "invalid_key", "unready"):
            row = copy.deepcopy(shadow)
            row["routing"]["fallback_reason"] = reason
            validate_record(row, self.schemas, 100)
        row = copy.deepcopy(shadow)
        row["routing"]["fallback_reason"] = "made_up"
        with self.assertRaises(ValueError):
            validate_record(row, self.schemas, 100)

    def test_precedence_and_corrupt_records(self):
        for change in ({"record_version": True}, {"attempt_id": "0"}, {"processing_time_ms": -1},
                       {"outcome": "dlq"}, {"event_id": "wrong:1"}, {"retry_count": True},
                       {"extra_payload": "secret"}):
            with self.assertRaises(ValueError): validate_record({**self.record, **change}, self.schemas, 100)
        for data in ('{"a":1,"a":2}', '{"a":NaN}', '{"a":Infinity}'):
            with self.assertRaises(ValueError): strict_json(data)
        row = copy.deepcopy(self.record)
        row.update(processing_time_ms=-1, label_status="negative", settlement_operation="nack",
                   outcome="dlq", dlq_reason="TTL_EXPIRED")
        validate_record(row, self.schemas, 100)
        self.assertTrue(dimensions(row)["failed_attempt"])
        self.assertTrue(dimensions(row)["censored"])

    def test_dedup_conflict_tail_and_routing_consistency(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / ("feedback-" + "a" * 32 + "-00000000000000000001.jsonl")
            path.write_text((json.dumps(self.record) + "\n") * 2)
            store = FeedbackIndex(Path(root) / "scratch.db")
            try:
                self.assertEqual(store.ingest([path], self.schemas, 100)["duplicates"], 1)
                changed = {**self.record, "elapsed_since_arrival_ms": 8}
                path.write_text(json.dumps(changed) + "\n")
                with self.assertRaisesRegex(ValueError, "conflicting"): store.ingest([path], self.schemas, 100)
                path.write_text(json.dumps(self.record))
                with self.assertRaisesRegex(ValueError, "incomplete"): store.ingest([path], self.schemas, 100)
            finally: store.close()


if __name__ == "__main__": unittest.main()
