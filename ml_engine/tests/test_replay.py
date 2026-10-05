import copy
import unittest

from ml_engine.models import Candidate
from ml_engine.replay import replay
from ml_engine.tests.online_helpers import config, events


class Replay(unittest.TestCase):
    def test_original_predictions_out_of_order_zero_label_and_equal_time(self):
        evidence = []
        report = replay(events(), Candidate("global_mean", config()), config(), emit=evidence.append)
        predictions = [r["duration_ms"] for r in evidence if r["kind"] == "prediction" and r["split"] == "evaluation"]
        self.assertEqual(predictions, [2, 2, 2])
        self.assertEqual(report["cohorts"]["evaluation"]["mae_ms"], 4)
        self.assertEqual(report["cohorts"]["evaluation"]["positive_scored_labels"], 2)
        self.assertEqual(report["updates"], 4)
        self.assertEqual(report["peak_pending"], 3)
        self.assertEqual(report, replay(events(), Candidate("global_mean", config()), config()))

    def test_duplicate_feedback_learns_once_and_failures_do_not_learn(self):
        source = events()
        duplicate = copy.deepcopy(next(r for r in source if r["message_id"] == "b" and r["event_type"] == "outcome"))
        source.insert(source.index(next(r for r in source if r["message_id"] == "c" and r["event_type"] == "outcome")), duplicate)
        self.assertEqual(replay(source, Candidate("global_mean", config()), config())["updates"], 4)
        for trigger in ("lease_expiry", "ttl_sweep"):
            source = events()
            terminal = next(r for r in source if r["message_id"] == "b" and r["event_type"] == "outcome")
            terminal.update(label_status="missing", outcome="dlq", trigger=trigger, processing_time_ms=None)
            self.assertEqual(replay(source, Candidate("global_mean", config()), config())["updates"], 3)
        source = events()
        terminal = next(r for r in source if r["message_id"] == "b" and r["event_type"] == "outcome")
        terminal.update(label_status="failure", outcome="dlq", settlement_operation="nack")
        self.assertEqual(replay(source, Candidate("global_mean", config()), config())["updates"], 3)

    def test_embargo_never_learns_and_limits_fail_closed(self):
        source = events()
        for row in source:
            if row["message_id"] == "warm":
                row["split"] = "embargo"
        self.assertEqual(replay(source, Candidate("global_mean", config()), config())["updates"], 3)
        small = config(); small["pending_limit"] = 1
        with self.assertRaisesRegex(ValueError, "pending"):
            replay(events(), Candidate("global_mean", small), small)
        with self.assertRaisesRegex(ValueError, "missing terminal"):
            replay(events()[:-1], Candidate("global_mean", config()), config())
        source = events()
        a, b = next(i for i,r in enumerate(source) if r["message_id"] == "c" and r["event_type"] == "ingress"), next(i for i,r in enumerate(source) if r["message_id"] == "b" and r["event_type"] == "outcome")
        source[a], source[b] = source[b], source[a]
        with self.assertRaisesRegex(ValueError, "equal-time"):
            replay(source, Candidate("global_mean", config()), config())

    def test_retry_audit_published_after_ack_is_not_a_second_label(self):
        source = events()
        ack = next(r for r in source if r["message_id"] == "b" and r["event_type"] == "outcome")
        retry = copy.deepcopy(ack)
        ack.update(attempt_id="2", retry_count=1)
        retry.update(event_id="fixture-instance:9", benchmark_time_ns=6, label_status="failure", outcome="retry",
                     settlement_operation="nack", processing_time_ms=9, retry_count=1)
        # The earlier state transition can be published after its terminal Ack.
        retry["benchmark_transition_ns"] = 3
        source.append(retry)
        source.sort(key=lambda r: (r["benchmark_time_ns"], r["event_type"] != "ingress"))
        report = replay(source, Candidate("global_mean", config()), config())
        self.assertEqual(report["updates"], 4)
        self.assertEqual(report["cohorts"]["evaluation"]["mae_ms"], 4)
