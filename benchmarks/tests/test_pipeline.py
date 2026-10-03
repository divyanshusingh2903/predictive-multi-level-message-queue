import json
import os
from pathlib import Path
import tempfile
import subprocess
import unittest

from benchmarks.evaluate import run_experiment
from benchmarks.export_feedback import export_run


@unittest.skipUnless(os.environ.get("HARBINGER_BENCHMARK_RUNNER"), "runner supplied by benchmark CTest")
class Pipeline(unittest.TestCase):
    def test_replay_reports_saturation_overflow_and_cutoff_censoring(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            config = root / "negative.json"
            config.write_text(json.dumps({"version": "baseline-experiment-v1", "seeds": [11],
                "policies": ["disabled"], "telemetry_off_control": False,
                "broker": {"drain_ms": 100}, "cases": [
                    {"name": "burst", "distribution": "uniform", "count": 200, "rate_per_s": 100,
                     "burst_size": 200, "workers": 1, "broker": {"submitters": 1}},
                    {"name": "overflow", "distribution": "uniform", "count": 8, "rate_per_s": 100,
                     "min_cost_us": 1000, "cap_cost_us": 2000, "workers": 1, "observation_capacity": 1},
                    {"name": "cutoff", "distribution": "uniform", "count": 8, "rate_per_s": 1000,
                     "min_cost_us": 100000, "cap_cost_us": 100000, "workers": 1,
                     "broker": {"drain_ms": 1, "lease_ms": 1000}}
                ]}))
            runner = Path(os.environ["HARBINGER_BENCHMARK_RUNNER"])
            results = run_experiment(config, runner, root / "negative")
            by_case = {row["case"]: row["metrics"] for row in results["runs"]}
            self.assertGreater(by_case["burst"]["counts"].get("missed", 0), 0)
            self.assertFalse(by_case["burst"]["valid"])
            self.assertGreater(by_case["overflow"]["observation_drops"], 0)
            self.assertFalse(by_case["overflow"]["valid"])
            self.assertGreater(by_case["cutoff"]["counts"].get("unfinished", 0), 0)
            self.assertFalse(by_case["cutoff"]["full_drain"])
            self.assertGreater(by_case["cutoff"]["unfinished_age_ms"]["count"], 0)
            (root / "bad.tsv").write_text("unsupported-trace-version\n")
            output = root / "bad-output"; output.mkdir()
            failed = subprocess.run([str(runner), str(root / "bad.tsv"), str(config), str(output)], capture_output=True)
            self.assertNotEqual(failed.returncode, 0)
            self.assertFalse(list(output.iterdir()))

    def test_real_rpc_export_reordered_feedback_and_group_embargo(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            spec = {"version": "baseline-experiment-v1", "seeds": [11], "repeats": 1,
                    "policies": ["disabled"], "telemetry_off_control": False,
                    "broker": {"drain_ms": 1000},
                    "cases": [{"name": "pipeline", "distribution": "uniform", "count": 16,
                               "rate_per_s": 100, "min_cost_us": 1000, "cap_cost_us": 2000, "workers": 1}]}
            config = root / "config.json"; config.write_text(json.dumps(spec))
            run = root / "run"
            result = run_experiment(config, Path(os.environ["HARBINGER_BENCHMARK_RUNNER"]), run)
            self.assertEqual(result["runs"][0]["metrics"]["counts"]["ack"], 16)
            directory = next(run.glob("*/run.json")).parent
            segment = next((directory / "feedback").glob("*.jsonl"))
            lines = segment.read_text().splitlines()
            segment.write_text("\n".join(list(reversed(lines)) + [lines[0]]) + "\n")
            export = root / "export"
            audit = export_run(run, export)
            self.assertEqual(audit["valid_for_learning"], not bool(audit["stats"].get("telemetry_loss", 0) or
                              audit["stats"].get("unresolved", 0) or audit["stats"].get("attempt_ordinal_gaps", 0)))
            self.assertEqual(audit["sources"][0]["duplicates"], 1)
            training = [json.loads(line) for line in (export / "train.jsonl").read_text().splitlines()]
            evaluation = [json.loads(line) for line in (export / "evaluation_labels.jsonl").read_text().splitlines()]
            self.assertTrue(training); self.assertTrue(evaluation)
            self.assertFalse({row["message_id"] for row in training} & {row["message_id"] for row in evaluation})
            self.assertEqual([row["label_available_ns"] for row in training], sorted(row["label_available_ns"] for row in training))
            for row in training + evaluation:
                self.assertEqual(set(row["features"]["headers"]), {"job"})
                self.assertEqual(row["features"]["payload_size_bytes"], "16")
                self.assertNotIn("cost_us", row)
            # Delay one pre-cutoff message's label to the evaluation boundary.
            target = training[-1]["message_id"]
            path = directory / "observations.tsv"
            changed = []
            for line in path.read_text().splitlines():
                columns = line.split("\t")
                if columns[0] == target and columns[-1] == "ack": columns[5] = "90000000"
                changed.append("\t".join(columns))
            path.write_text("\n".join(changed) + "\n")
            embargo_export = root / "embargo"
            embargo = export_run(run, embargo_export)
            self.assertEqual(embargo["stats"]["embargo"], 1)
            self.assertNotIn(target, {json.loads(line)["message_id"] for line in
                                     (embargo_export / "train.jsonl").read_text().splitlines()})


if __name__ == "__main__": unittest.main()
