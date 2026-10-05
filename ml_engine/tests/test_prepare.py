import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from ml_engine.prepare import HEALTH, coverage
from ml_engine.tests.online_helpers import config, write_export


class Preparation(unittest.TestCase):
    def test_admission_uses_complete_counts_and_writer_health_not_model_quality(self):
        run = {"policy": "disabled", "case": "fixture", "seed": 101, "repeat": 0,
               "directory": "fixture-s101-r0-disabled", "metrics": {"valid": True, "full_drain": True,
               "all_messages_counts": {"offered": 4, "accepted": 4, "ack": 4, "terminal": 4, "unfinished": 0}},
               "summary": {**dict.fromkeys(HEALTH, "0"), "feedback_written": "8", "feedback_synced": "8"}}
        with tempfile.TemporaryDirectory() as root:
            root = Path(root) / run["directory"]; root.mkdir()
            def fake_export(_source, target, fraction):
                target.mkdir(); write_export(target)
            (root / "run.json").write_text(json.dumps(run))
            with patch("ml_engine.prepare.export_run", side_effect=fake_export):
                self.assertEqual(coverage(root, 4, config()), [])
            for mutation in ("feedback_drops", "feedback_pending_records", "feedback_sync_failures", "pull_errors"):
                changed = copy.deepcopy(run); changed["summary"][mutation] = "1"
                (root / "run.json").write_text(json.dumps(changed))
                self.assertIn("writer/observation/consumer health", coverage(root, 4, config()))
            changed = copy.deepcopy(run); changed["metrics"]["all_messages_counts"]["accepted"] = 3
            (root / "run.json").write_text(json.dumps(changed))
            self.assertTrue(coverage(root, 4, config()))
