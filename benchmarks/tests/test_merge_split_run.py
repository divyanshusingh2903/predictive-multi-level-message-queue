import json
from pathlib import Path
import tempfile
import unittest

from benchmarks.merge_split_run import merge, merge_dir

CLIENT = {"benchmark": "prodflow-v1", "arm": "predictive", "seed": 11, "messages": 10, "completed": 10, "dlq": 0,
          "submit_errors": 0, "role": "client", "broker_address": "10.0.0.5:50051"}
BROKER = {"arm": "predictive", "dlq": 2, "broker": {"routing": {"mode": "predictive"}}}


class MergeSplitRun(unittest.TestCase):
    def test_broker_counters_and_dlq_are_authoritative(self):
        run = merge(CLIENT, BROKER)
        self.assertEqual(run["dlq"], 2)
        self.assertEqual(run["broker"], BROKER["broker"])
        self.assertEqual(run["role"], "split")
        self.assertEqual(run["broker_address"], "10.0.0.5:50051")
        self.assertEqual(CLIENT["dlq"], 0)  # inputs are not modified

    def test_arm_mismatch_is_rejected(self):
        with self.assertRaises(ValueError):
            merge(CLIENT, {**BROKER, "arm": "fifo"})

    def test_embedded_run_is_rejected(self):
        with self.assertRaises(ValueError):
            merge({k: v for k, v in CLIENT.items() if k != "role"}, BROKER)

    def test_writes_run_json_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "client.json").write_text(json.dumps(CLIENT))
            (directory / "broker.json").write_text(json.dumps(BROKER))
            target = merge_dir(directory)
            self.assertEqual(json.loads(target.read_text())["dlq"], 2)
            with self.assertRaises(FileExistsError):
                merge_dir(directory)


if __name__ == "__main__":
    unittest.main()
