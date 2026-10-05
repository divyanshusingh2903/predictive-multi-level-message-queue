import copy
from pathlib import Path
import tempfile
import unittest

from ml_engine.compare import compare, selection
from ml_engine.tests.online_helpers import config, write_export
from ml_engine.dataset import canonical


def reports(spec):
    output = []
    for name in spec["candidates"]:
        for seed in spec["seeds"]:
            for repeat in range(spec["repeats"]):
                output.append({"candidate": name, "case": "fixture", "seed": seed, "repeat": repeat,
                    "cohorts": {"evaluation_warm": {"eligible_labels": 1000, "scored_predictions": 1000,
                        "positive_scored_labels": 900, "eligible_positive_labels": 900, "prediction_coverage": 1.0, "mae_ms": 2,
                        "severe_underestimate_rate": .1}},
                    "profiles": [{"warm_prediction_ms": {"p99": .2, "count": 1900}, "peak_model_state": {"accounted_state_bytes": 1000}}]})
    return output


class Compare(unittest.TestCase):
    def test_complexity_selection_no_qualifier_and_incomplete(self):
        spec = config(); spec["seeds"] = [11,22,33,44,55]
        source = reports(spec)
        result = selection(source, spec)
        self.assertEqual(result["selected_candidate"], "global_mean")
        # A superior severe-underestimate baseline and superior MAE baseline
        # need not be the same candidate: both gates still apply.
        for row in source:
            if row["candidate"] == "job_mean":
                row["cohorts"]["evaluation_warm"]["mae_ms"] = 1
                row["cohorts"]["evaluation_warm"]["severe_underestimate_rate"] = .2
        self.assertEqual(selection(source, spec)["status"], "no_qualifier")
        self.assertEqual(selection(source[:-1], spec)["status"], "incomplete_evidence")
        bad = reports(spec); bad[0]["cohorts"]["evaluation_warm"]["prediction_coverage"] = .9
        self.assertFalse(selection(bad, spec)["candidates"][0]["passes"])
        # An invalid model forecast fails the candidate, not the data evidence.
        for row in bad:
            if row["candidate"] == "global_mean":
                row["cohorts"]["evaluation_warm"].update(prediction_coverage=0, scored_predictions=0,
                    positive_scored_labels=0, mae_ms=None, severe_underestimate_rate=None)
        self.assertEqual(selection(bad, spec)["status"], "no_qualifier")

    def test_profile_free_cli_artifacts_do_not_claim_selection(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            data = root / "dataset"; data.mkdir(); write_export(data)
            settings = root / "config.json"; settings.write_text(canonical(config()))
            output = root / "comparison"
            result = compare(data, settings, output, profile=False)
            self.assertEqual(result["status"], "incomplete_evidence")
            self.assertIsNone(result["model_version"])
            self.assertTrue((output / "predictions.jsonl").is_file())
            with self.assertRaisesRegex(ValueError, "fresh"):
                compare(data, settings, output, profile=False)
