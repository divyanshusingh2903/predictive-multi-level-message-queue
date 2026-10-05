import hashlib
import json
from pathlib import Path
import tarfile
import unittest

from ml_engine.dataset import fingerprint, rows
from ml_engine.publish import sha256

ROOT = Path(__file__).resolve().parents[2]
EVIDENCE = ROOT / "benchmarks/results/issue5-v1"


@unittest.skipUnless(EVIDENCE.exists(), "published issue-5 evidence not present yet")
class PublishedPredictors(unittest.TestCase):
    def test_frozen_configuration_complete_pairs_and_decision_consistency(self):
        config = json.loads((ROOT / "ml_engine/configs/online-comparison-v1.json").read_text())
        manifest = json.loads((EVIDENCE / "manifest.json").read_text())
        decision = json.loads((EVIDENCE / "decision.json").read_text())
        self.assertEqual(manifest["config_sha256"], fingerprint(config))
        self.assertEqual(decision["config_sha256"], manifest["config_sha256"])
        self.assertEqual(decision["dataset_sha256"], fingerprint(manifest["dataset_files"]))
        self.assertFalse(decision["predictive_activation"])
        self.assertIn(decision["status"], ("selected", "no_qualifier"))
        winner = next((row["candidate"] for row in decision["candidates"] if row["passes"]), None)
        self.assertEqual(winner, decision["selected_candidate"])
        expected = {(name, case, seed, repeat) for name in config["candidates"] for case in config["required_cases"]
                    for seed in config["seeds"] for repeat in range(config["repeats"])}
        seen = set()
        for row in rows(EVIDENCE / "runs.jsonl"):
            key = row["candidate"], row["case"], row["seed"], row["repeat"]
            self.assertNotIn(key, seen); seen.add(key)
            warm = row["cohorts"]["evaluation_warm"]
            self.assertGreaterEqual(warm["eligible_labels"], config["minimum_evaluation_labels"])
            self.assertEqual(sum(map(sum, warm["bucket_confusion"])), warm["eligible_labels"])
            self.assertEqual(len(row["profiles"]), config["profiling"]["repeats"])
            self.assertEqual(row["updates"], row["cohorts"]["train"]["eligible_labels"] + row["cohorts"]["evaluation"]["eligible_labels"])
        self.assertEqual(seen, expected)

    def test_raw_inventory_and_temporal_export_fingerprints(self):
        inventory = json.loads((EVIDENCE / "raw/inventory.json").read_text())
        if not all((EVIDENCE / "raw" / item["file"]).exists() for item in inventory):
            self.skipTest("raw archives are not tracked in git; see raw/README.md")
        for item in inventory:
            path = EVIDENCE / "raw" / item["file"]
            self.assertEqual(path.stat().st_size, item["bytes"])
            self.assertEqual(sha256(path), item["sha256"])
        manifest = json.loads((EVIDENCE / "manifest.json").read_text())
        with tarfile.open(EVIDENCE / "raw/dataset-v1.tar.gz") as archive:
            for name, expected in manifest["dataset_files"].items():
                digest = hashlib.sha256()
                with archive.extractfile("dataset/" + name) as stream:
                    for chunk in iter(lambda: stream.read(65536), b""):
                        digest.update(chunk)
                self.assertEqual(digest.hexdigest(), expected)
