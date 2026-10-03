"""Check conservation and paired covariates in the retained evidence, without timing gates."""
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1] / "results/issue4-v1"


class PublishedEvidence(unittest.TestCase):
    def test_raw_history_checksums(self):
        directory = ROOT / "raw"
        checksums = [line.split() for line in (directory / "SHA256SUMS").read_text().splitlines()]
        self.assertEqual({name for _, name in checksums}, {path.name for path in directory.glob("*.tar.gz")})
        for expected, name in checksums:
            with self.subTest(archive=name):
                digest = hashlib.sha256()
                with (directory / name).open("rb") as stream:
                    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                        digest.update(chunk)
                self.assertEqual(digest.hexdigest(), expected)

    def test_all_trials_reconcile_and_retain_invalid_coverage(self):
        for directory in (ROOT / "calibration", ROOT / "baselines"):
            manifest = json.loads((directory / "manifest.json").read_text())
            trials = [json.loads(line) for line in (directory / "trials.jsonl").read_text().splitlines()]
            summary = json.loads((directory / "summary.json").read_text())
            self.assertEqual(len(trials), summary["total_trials"])
            self.assertEqual(sum(row["metrics"]["valid"] for row in trials), summary["valid_trials"])
            cases = {row["name"]: row for row in manifest["config"]["cases"]}
            for trial in trials:
                measured = trial["metrics"]
                all_messages = measured["all_messages_counts"]
                self.assertEqual(all_messages["offered"], cases[trial["case"]]["count"])
                self.assertEqual(all_messages["accepted"], all_messages["terminal"] + all_messages["unfinished"])
                counts = measured["counts"]
                self.assertEqual(counts["accepted"], sum(counts.get(name, 0) for name in ("ack", "ttl", "max_retries", "unfinished")))
                self.assertEqual(measured["offered"], sum(counts.get(name, 0) for name in
                    ("ack", "ttl", "max_retries", "unfinished", "missed", "rejected", "unconfirmed")))
                if measured["telemetry_loss"] or measured["observation_drops"]:
                    self.assertFalse(measured["valid"])
                self.assertEqual(measured["observation_drops"], 0)
                self.assertTrue(measured["full_drain"])
                self.assertEqual(len(trial["trace_sha256"]), 64)

    def test_paired_trace_settings_and_policy_coverage(self):
        groups = defaultdict(list)
        for line in (ROOT / "baselines/trials.jsonl").read_text().splitlines():
            trial = json.loads(line)
            groups[(trial["case"], trial["seed"], trial["repeat"])].append(trial)
        for trials in groups.values():
            self.assertEqual({row["policy"] for row in trials}, {"fifo", "static", "round_robin", "disabled", "disabled_off"})
            self.assertEqual(len({row["trace_sha256"] for row in trials}), 1)
            settings = [{key: value for key, value in row["parameters"].items() if key not in ("policy", "feedback")}
                        for row in trials]
            self.assertTrue(all(row == settings[0] for row in settings))


if __name__ == "__main__": unittest.main()
