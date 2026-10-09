from pathlib import Path
import json
import tarfile
import tempfile
import unittest

from analysis.publish import archive, publish, sha256


class Publication(unittest.TestCase):
    def test_archives_are_checksummable_reproducible_and_retain_raw_bytes(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            source = root / "source"; source.mkdir()
            (source / "data.jsonl").write_bytes(b'{"zero":0}\n')
            first, second = root / "first.tar.gz", root / "second.tar.gz"
            archive(source, first); archive(source, second)
            self.assertEqual(sha256(first), sha256(second))
            with tarfile.open(first) as tar:
                self.assertEqual(tar.extractfile("source/data.jsonl").read(), b'{"zero":0}\n')
                self.assertEqual(tar.getmember("source/data.jsonl").mtime, 0)

    def test_refresh_cannot_replace_a_different_frozen_experiment(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            source, prepared, comparison, output = (root / name for name in ("source", "prepared", "comparison", "output"))
            for directory in (source, prepared, comparison, output):
                directory.mkdir()
            (output / "manifest.json").write_text(json.dumps({"config_sha256": "original", "dataset_sha256": "same"}))
            original = (output / "manifest.json").read_bytes()
            (comparison / "manifest.json").write_text(json.dumps({"config_sha256": "changed", "dataset_sha256": "same"}))
            (comparison / "decision.json").write_text(json.dumps({"dataset_sha256": "same"}))
            (prepared / "preparation.json").write_text(json.dumps({"status": "complete", "dataset_sha256": "same",
                                                                  "initial_source": str(source)}))
            with self.assertRaisesRegex(ValueError, "identical frozen"):
                publish(source, prepared, comparison, output, refresh=True)
            self.assertEqual((output / "manifest.json").read_bytes(), original)
