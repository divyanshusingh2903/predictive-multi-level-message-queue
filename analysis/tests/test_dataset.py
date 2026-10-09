import copy
import json
from pathlib import Path
import tempfile
import unittest

from analysis.dataset import TemporalDataset, canonical
from analysis.tests.online_helpers import config, events, write_export


class Dataset(unittest.TestCase):
    def test_valid_fingerprint_chronology_duplicate_and_run_isolation(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            write_export(root)
            with TemporalDataset(root, config()) as dataset:
                run = next(iter(dataset.sources))
                self.assertEqual(list(dataset.events(run)), events())
                original = dataset.fingerprint
                delayed = list(dataset.events(run, 100))
                self.assertEqual(delayed[0]["event_type"], "ingress")
                self.assertEqual(delayed[-1]["benchmark_time_ns"], 110)
            with (root / "events.jsonl").open("a") as stream:
                stream.write(canonical(events()[0])+"\n")
            with TemporalDataset(root, config()) as dataset:
                self.assertEqual(dataset.audit["identical_duplicates"], 1)
                self.assertNotEqual(dataset.fingerprint, original)

    def test_invalid_manifest_and_favorable_missing_label_population(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            write_export(root)
            path = root / "manifest.json"
            manifest = json.loads(path.read_text())
            manifest["valid_for_learning"] = False
            path.write_text(canonical(manifest))
            with self.assertRaisesRegex(ValueError, "manifest"):
                TemporalDataset(root, config())
            write_export(root)
            path = root / "evaluation_labels.jsonl"
            path.write_text("".join(path.read_text().splitlines(keepends=True)[1:]))
            with self.assertRaisesRegex(ValueError, "missing evaluation_labels"):
                TemporalDataset(root, config())

    def test_future_feature_and_conflicting_attempt_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            source = events()
            next(r for r in source if r["event_type"] == "outcome")["routing"]["features"]["headers"]["job"] = "class2"
            write_export(root, source)
            with self.assertRaisesRegex(ValueError, "immutable"):
                TemporalDataset(root, config())
            write_export(root)
            row = copy.deepcopy(events()[0]); row["elapsed_since_arrival_ms"] += 1
            with (root / "events.jsonl").open("a") as stream:
                stream.write(canonical(row)+"\n")
            with self.assertRaisesRegex(ValueError, "conflicting"):
                TemporalDataset(root, config())

    def test_unknown_schema_and_negative_chronology_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            for key, value in (("benchmark_time_ns", -1), ("benchmark_publication_ns", -1)):
                source = events(); source[0][key] = value
                write_export(root, source)
                with self.assertRaisesRegex(ValueError, "chronology"):
                    TemporalDataset(root, config())
            changed = config(); changed["schema"]["version"] = "other"
            write_export(root)
            with self.assertRaisesRegex(ValueError, "unregistered"):
                TemporalDataset(root, changed)
