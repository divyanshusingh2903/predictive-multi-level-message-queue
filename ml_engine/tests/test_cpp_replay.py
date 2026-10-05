import json
import os
from pathlib import Path
import tempfile
import unittest

from ml_engine.cpp_replay import run_replay, score

BINARY = os.environ.get("HARBINGER_PREDICTOR_REPLAY")
MS = 1_000_000


def events(run="r-s1-r1-disabled", per_class=60):
    rows, clock, index = [], 0, 0
    for round_ in range(per_class):
        for name, duration in (("class0", 2), ("class1", 20), ("class2", 200)):
            index += 1
            base = {"run": run, "broker_instance_id": "i", "message_id": f"m{index}",
                    "split": "train" if round_ < per_class // 2 else "evaluation",
                    "routing": {"features": {"payload_size_bytes": 1, "headers": {"job": name}}}}
            rows.append({**base, "event_type": "ingress", "event_id": f"i:{2 * index - 1}", "benchmark_time_ns": clock,
                         "label_status": None, "processing_time_ms": None})
            rows.append({**base, "event_type": "outcome", "event_id": f"i:{2 * index}", "benchmark_time_ns": clock + 5 * MS,
                         "label_status": "eligible", "processing_time_ms": duration})
            clock += 10 * MS
    return rows


@unittest.skipUnless(BINARY, "HARBINGER_PREDICTOR_REPLAY not set (build the harbinger_predictor_replay target)")
class CppReplay(unittest.TestCase):
    def export(self, rows, root):
        path = Path(root) / "events.jsonl"
        path.write_text("".join(json.dumps(r) + "\n" for r in rows))
        return Path(root)

    def test_cold_then_learned_tiers_and_scores(self):
        with tempfile.TemporaryDirectory() as root:
            rows = events()
            predictions, summaries = run_replay(BINARY, self.export(rows, root),
                                                ["--min-samples", "5", "--global-min-samples", "30", "--decay", "1"])
        first = predictions[("r-s1-r1-disabled", "i", "m1")]
        self.assertEqual((first["duration_ms"], first["bucket"], first["fallback"]), (None, 1, "unready"))
        last = {k: v for k, v in predictions.items() if k[2] in ("m178", "m179", "m180")}
        self.assertEqual(sorted(v["bucket"] for v in last.values()), [0, 1, 2])
        self.assertEqual(len(summaries["r-s1-r1-disabled"]["boundaries_ms"]), 2)
        report = score(rows, predictions, summaries, 3)["evaluation"]
        self.assertEqual(report["eligible_labels"], 90)
        self.assertEqual(report["bucket_accuracy"], 1.0)
        self.assertEqual(report["prediction_coverage"], 1.0)

    def test_runs_are_independent_and_deterministic(self):
        with tempfile.TemporaryDirectory() as root:
            rows = events("a-s1-r1-disabled") + events("b-s2-r1-disabled")
            export = self.export(rows, root)
            args = ["--min-samples", "5", "--global-min-samples", "30"]
            one = run_replay(BINARY, export, args)
            two = run_replay(BINARY, export, args)
        self.assertEqual(one, two)
        self.assertEqual(set(one[1]), {"a-s1-r1-disabled", "b-s2-r1-disabled"})
        # Second run restarts cold rather than inheriting the first run's keys.
        self.assertEqual(one[0][("b-s2-r1-disabled", "i", "m1")]["fallback"], "unready")

    def test_ineligible_and_malformed_input(self):
        with tempfile.TemporaryDirectory() as root:
            rows = events(per_class=3)
            for row in rows:
                if row["event_type"] == "outcome":
                    row["label_status"] = "failure"
            _, summaries = run_replay(BINARY, self.export(rows, root))
            self.assertEqual(summaries["r-s1-r1-disabled"]["learned"], 0)
            (Path(root) / "events.jsonl").write_text("{not json}\n")
            with self.assertRaises(Exception):
                run_replay(BINARY, Path(root))
