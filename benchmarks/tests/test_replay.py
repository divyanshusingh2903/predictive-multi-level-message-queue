"""Trace replay through the real broker (harbinger_replay), on a small synthetic trace.

Needs HARBINGER_REPLAY (the binary); skipped without it, and not part of the 60 s Python suite: ctest runs it as
`replay_python_tests`. The trace is two job types, short (10 ms) and long (100 ms), so every expectation can be worked
out by hand; no number here comes from a registered trace.
"""
import csv
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

BINARY = os.environ.get("HARBINGER_REPLAY")
SHORT_MS, LONG_MS = 10.0, 100.0


def write_trace(path: Path) -> None:
    """600 history rows (alternating types, one per second) then 60 evaluation rows, 10 ms apart, 30 of each type."""
    with path.open("w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["arrival_ms", "key", "duration_ms"])
        for i in range(600):
            w.writerow([i * 1000.0, "short" if i % 2 == 0 else "long", SHORT_MS if i % 2 == 0 else LONG_MS])
        for i in range(60):
            short = i % 2 == 0
            w.writerow([600_000.0 + i * 10.0, "short" if short else "long", SHORT_MS if short else LONG_MS])


def config(time_scale: float = 1.0) -> dict:
    return {"version": "test", "time_scale": time_scale, "history_fraction": 0.999,  # the time split falls between the last history row and the first evaluation row
            "workers": 1, "levels": 3,
            "default_priority": 1, "aging": {"threshold_ms": 5000, "interval_ms": 500},
            "predictor": {"stale_after_ms": 1800000, "time_half_life_ms": 600000}, "level_weights": [8, 3, 1],
            "utilizations": [3.0], "producer_threads": 2, "drain_s": 60}


@unittest.skipUnless(BINARY, "set HARBINGER_REPLAY to the harbinger_replay binary")
class Replay(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        cls.trace = cls.dir / "trace.csv"
        write_trace(cls.trace)
        cls.prepared = {}
        cls.runs = {}

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def prepare(self, scale: float = 1.0) -> tuple[Path, Path]:
        if scale not in self.prepared:
            cfg = self.dir / f"config-{scale}.json"
            cfg.write_text(json.dumps(config(scale)))
            prep = self.dir / f"prep-{scale}"
            subprocess.run([BINARY, "prepare", str(self.trace), str(cfg), str(prep)], check=True, capture_output=True)
            self.prepared[scale] = (cfg, prep)
        return self.prepared[scale]

    def run_arm(self, arm: str, scale: float = 1.0):
        if (arm, scale) in self.runs:
            return self.runs[(arm, scale)]
        cfg, prep = self.prepare(scale)
        out = self.dir / f"run-{scale}-{arm}"
        done = subprocess.run([BINARY, "run", "--arm", arm, "--load", "3", "--trace", str(self.trace), "--config", str(cfg),
                               "--prep", str(prep), "--out", str(out)], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr + done.stdout)
        run = json.loads((out / "run.json").read_text())
        with (out / "messages.tsv").open() as f:
            rows = list(csv.DictReader(f, delimiter="\t"))
        self.runs[(arm, scale)] = (run, rows)
        return run, rows

    @staticmethod
    def mean_latency_ms(rows) -> float:
        return sum((int(r["end_ns"]) - int(r["sched_ns"])) / 1e6 for r in rows) / len(rows)

    def test_plan_matches_the_simulator_rules(self):
        _, prep = self.prepare()
        plan = json.loads((prep / "plan.json").read_text())
        self.assertEqual((plan["history_rows"], plan["eval_rows"]), (600, 60))
        work = 30 * SHORT_MS + 30 * LONG_MS
        span = 59 * 10.0
        self.assertAlmostEqual(plan["loads"]["3"]["compress"], 3.0 * 1 * span / work, places=9)  # load x workers x span / work
        # keys travel as k<index>, in order of first appearance: "short" is k0 and "long" is k1
        self.assertEqual(sorted(plan["static_history"]), ["k0", "k1"])
        self.assertLess(plan["static_history"]["k0"], plan["static_history"]["k1"])

    def test_every_arm_completes_every_message(self):
        for arm in ("fifo", "oracle", "static_history", "predictive_warm", "predictive_cold", "predictive_warm+weights+pausing"):
            with self.subTest(arm=arm):
                run, rows = self.run_arm(arm)
                self.assertEqual((run["messages"], run["completed"], run["dlq"], run["leases_lost"], run["submit_errors"]),
                                 (60, 60, 0, 0, 0))
                self.assertTrue(all(int(r["done"]) == 1 and int(r["attempts"]) == 1 for r in rows))

    def test_scheduling_is_accurate(self):
        run, _ = self.run_arm("fifo")
        self.assertLess(run["timing"]["lateness_p99_ms"], 50.0)  # generous: shared test machine

    def test_priority_arms_beat_fifo_by_hand_computed_order(self):
        # One worker at load 3: a backlog forms, so serving the 10 ms jobs ahead of the 100 ms jobs lowers the mean.
        fifo = self.mean_latency_ms(self.run_arm("fifo")[1])
        for arm in ("oracle", "predictive_warm", "static_history"):
            with self.subTest(arm=arm):
                self.assertLess(self.mean_latency_ms(self.run_arm(arm)[1]), fifo * 0.9)

    def test_fifo_serves_in_arrival_order(self):
        _, rows = self.run_arm("fifo")
        starts = [int(r["start_ns"]) for r in sorted(rows, key=lambda r: int(r["idx"]))]
        misordered = sum(a > b for a, b in zip(starts, starts[1:]))
        self.assertLessEqual(misordered, 2)  # two producer threads may swap neighbours submitted within one RTT

    def test_oracle_serves_short_jobs_before_long_ones(self):
        _, rows = self.run_arm("oracle")
        by_start = sorted(rows, key=lambda r: int(r["start_ns"]))
        shorts_first_half = sum(float(r["trace_duration_ms"]) == SHORT_MS for r in by_start[:30])
        self.assertGreaterEqual(shorts_first_half, 20)  # FIFO would give 15; 24 locally, 23 on slow CI runners

    def test_time_scale_divides_the_handler_duration(self):
        _, rows = self.run_arm("fifo", scale=2.0)
        for r in rows:
            handler_ms = (int(r["end_ns"]) - int(r["start_ns"])) / 1e6
            expected = float(r["trace_duration_ms"]) / 2.0
            self.assertGreaterEqual(handler_ms, expected * 0.99)
            self.assertLess(handler_ms, expected + 20.0)

    def test_no_aging_arm_runs_and_serves_short_jobs_first_even_without_promotion(self):
        run, rows = self.run_arm("predictive_warm+no_aging")
        self.assertEqual((run["messages"], run["completed"], run["dlq"], run["leases_lost"]), (60, 60, 0, 0))
        fifo = self.mean_latency_ms(self.run_arm("fifo")[1])
        self.assertLess(self.mean_latency_ms(rows), fifo * 0.9)  # strict priority still beats FIFO on the hand-built backlog

    def test_misuse_is_refused(self):
        cfg, prep = self.prepare()
        base = [BINARY, "run", "--load", "3", "--trace", str(self.trace), "--config", str(cfg), "--prep", str(prep)]
        for arm in ("nonsense", "predictive_warm+weights", "fifo+pausing", "predictive_warm+weights+pausing+no_aging"):
            with self.subTest(arm=arm):
                done = subprocess.run(base + ["--arm", arm, "--out", str(self.dir / f"bad-{arm}")], capture_output=True)
                self.assertEqual(done.returncode, 2)
        existing = self.dir / "run-1.0-fifo"
        self.run_arm("fifo")
        done = subprocess.run(base + ["--arm", "fifo", "--out", str(existing)], capture_output=True)
        self.assertEqual(done.returncode, 2)  # refuses to overwrite a run
        done = subprocess.run(base + ["--arm", "fifo", "--load", "7", "--out", str(self.dir / "bad-load")], capture_output=True)
        self.assertEqual(done.returncode, 2)


if __name__ == "__main__":
    unittest.main()
