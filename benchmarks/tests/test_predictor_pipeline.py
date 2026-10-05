import json
import os
from pathlib import Path
import tempfile
import unittest

from benchmarks.evaluate import run_experiment
from benchmarks.export_feedback import export_run
from ml_engine.dataset import TemporalDataset
from ml_engine.models import Candidate
from ml_engine.replay import replay
from ml_engine.tests.online_helpers import config


@unittest.skipUnless(os.environ.get("HARBINGER_BENCHMARK_RUNNER"), "runner supplied by benchmark CTest")
class PredictorPipeline(unittest.TestCase):
    def test_real_feedback_export_validates_and_trains_without_oracle_inputs(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            collection = {"version": "baseline-experiment-v1", "seeds": [101], "repeats": 1,
                "policies": ["disabled"], "telemetry_off_control": False, "broker": {"drain_ms": 500},
                "cases": [{"name": "predictor-pipeline", "distribution": "uniform", "count": 16,
                           "rate_per_s": 40, "workers": 1, "min_cost_us": 1000, "cap_cost_us": 2000}]}
            path = root / "config.json"; path.write_text(json.dumps(collection))
            run_experiment(path, Path(os.environ["HARBINGER_BENCHMARK_RUNNER"]), root / "runs")
            manifest = export_run(root / "runs", root / "dataset", .5)
            if not manifest["valid_for_learning"]:
                with self.assertRaises(ValueError):
                    TemporalDataset(root / "dataset", config())
                return  # Telemetry admission is intentionally nonblocking.
            with TemporalDataset(root / "dataset", config()) as dataset:
                run = next(iter(dataset.sources))
                report = replay(dataset.events(run), Candidate("global_mean", config()), config())
                self.assertGreater(report["updates"], 0)
                self.assertGreater(report["cohorts"]["evaluation"]["eligible_labels"], 0)
                self.assertEqual(report["cohorts"]["evaluation"]["prediction_coverage"], 1)
