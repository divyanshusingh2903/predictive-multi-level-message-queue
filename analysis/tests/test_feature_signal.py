import csv
import json
import random
import tempfile
import unittest
from pathlib import Path

from analysis.feature_signal import decide, evaluate_run, load

CONFIG = json.loads((Path(__file__).resolve().parents[2] / "benchmarks/configs/feature-signal-v1.json").read_text())
FIELDS = ["run", "seq", "submit_unix", "service", "job_type", "payload_bytes", "width", "height", "pixels", "rows",
          "duration_ms"]


def write_run(path, size_matters, seed=1, n=3000):
    rng = random.Random(seed)
    with path.open("w", newline="") as stream:
        w = csv.DictWriter(stream, fieldnames=FIELDS, extrasaction="ignore")
        w.writeheader()
        for i in range(n):
            kind = rng.choice(["resize", "call"])
            size = int(2 ** rng.uniform(10, 20))
            if kind == "resize":
                duration = (size / 1000 if size_matters else 50) * rng.uniform(0.9, 1.1)
            else:
                duration = 30 * rng.uniform(0.7, 1.3)
            w.writerow({"run": "r", "seq": i, "submit_unix": i, "service": "svc", "job_type": kind,
                        "payload_bytes": size, "duration_ms": duration})


class FeatureSignal(unittest.TestCase):
    def evaluate(self, size_matters):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "run.csv"
            write_run(path, size_matters)
            return evaluate_run(load(path), 0.6, CONFIG["gradient_boosting"])

    def test_size_bins_help_only_when_cost_follows_size(self):
        informative = self.evaluate(True)
        resize = informative["per_key"]["svc/resize"]
        self.assertLess(resize["per_key_size_bin_median"] + 1.0, resize["per_key_median"])
        self.assertGreater(informative["models"]["per_key_size_bin_median"]["tier_accuracy"],
                           informative["models"]["per_key_median"]["tier_accuracy"] + 0.05)
        flat = self.evaluate(False)["per_key"]["svc/resize"]
        self.assertAlmostEqual(flat["per_key_size_bin_median"], flat["per_key_median"], delta=0.1)

    def test_decision_needs_five_of_six_and_at_least_one_run(self):
        win = {"models": {"per_key_median": {"median_abs_log2_error": 1.0, "tier_accuracy": 0.6},
                          "per_key_size_bin_median": {"median_abs_log2_error": 0.5, "tier_accuracy": 0.8},
                          "gradient_boosting": None}}
        lose = {"models": {"per_key_median": {"median_abs_log2_error": 1.0, "tier_accuracy": 0.6},
                           "per_key_size_bin_median": {"median_abs_log2_error": 0.95, "tier_accuracy": 0.61},
                           "gradient_boosting": None}}
        self.assertEqual(decide(CONFIG, {"a": lose})["verdict"], "per-key statistics sufficient")
        self.assertEqual(decide(CONFIG, {"a": win})["verdict"], "binned size feature worthwhile")
        runs = {str(i): win for i in range(5)} | {"x": lose}
        self.assertEqual(decide(CONFIG, runs)["verdict"], "binned size feature worthwhile")
        runs = {str(i): win for i in range(4)} | {"x": lose, "y": lose}
        self.assertEqual(decide(CONFIG, runs)["verdict"], "per-key statistics sufficient")


if __name__ == "__main__":
    unittest.main()
