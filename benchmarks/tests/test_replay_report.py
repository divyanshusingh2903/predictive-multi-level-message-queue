import csv
import json
import tempfile
import unittest
from pathlib import Path

from benchmarks import replay_report as rr

CLASS_BOUNDS = [100.0, 1000.0]
ROWS, WINDOWS = 2800, 28
DURATIONS = (10.0, 500.0, 5000.0)  # short, medium, long: row i % 3
CRITERIA = {
    "A_P1_mean_latency_vs_fifo": {"arm": "predictive_warm", "baseline": "fifo", "metric": "mean_latency_ms", "max_relative_delta": -0.15},
    "A_G1_all_p99_vs_fifo": {"arm": "predictive_warm", "baseline": "fifo", "metric": "p99_latency_ms", "max_ratio": 2.0},
    "A_G2_long_max_wait_vs_fifo": {"arm": "predictive_warm", "baseline": "fifo", "metric": "long_max_wait_ms", "max_ratio": 2.0,
                                   "floor": "long_class_bound", "min_class_count": {"class": "long", "count": 30}},
}
CONFIG = {"version": "test", "workers": 64, "time_scale": 2, "windows": WINDOWS, "class_bounds_ms": CLASS_BOUNDS,
          "bootstrap": {"samples": 300, "seed": 1, "confidence": 0.95, "block_length": 4},
          "criteria": CRITERIA, "gates": {"A": list(CRITERIA)}}


def make_run(directory: Path, factor: float, *, scale: float = 2.0, dlq: int = 0, lateness: float = 0.2, completed: int = ROWS,
             wait_ms: float = 100.0) -> None:
    """A run whose latency in trace ms is factor x (1000 + 10 (i mod 7)), recorded in real ms (divided by the scale)."""
    directory.mkdir(parents=True)
    with (directory / "messages.tsv").open("w", newline="") as f:
        w = csv.writer(f, delimiter="\t", lineterminator="\n")
        w.writerow(["idx", "key", "trace_arrival_ms", "trace_duration_ms", "sched_ns", "submit_ns", "start_ns", "end_ns", "attempts", "done"])
        for i in range(ROWS):
            sched = 1_000_000_000 + i * 1_000_000
            latency_real_ms = factor * (1000 + 10 * (i % 7)) / scale
            start = sched + int(wait_ms / scale * 1e6)
            w.writerow([i, f"k{i % 3}", i * 1000.0, DURATIONS[i % 3], sched, sched + 100_000, start,
                        sched + int(latency_real_ms * 1e6), 1, 1])
    (directory / "run.json").write_text(json.dumps({
        "messages": ROWS, "completed": completed, "dlq": dlq, "submit_errors": 0, "leases_lost": 0, "time_scale": scale,
        "timing": {"lateness_p99_ms": lateness}}))


class Helpers(unittest.TestCase):
    def test_quantile_is_the_simulators(self):
        self.assertEqual(rr.quantile([5, 1, 3, 2, 4], 0.5), 3)
        self.assertEqual(rr.quantile(list(range(1, 101)), 0.99), 99)
        self.assertEqual(rr.quantile([7], 0.99), 7)

    def test_windows_have_equal_trace_time_not_equal_rows(self):
        arrivals = [0.0, 1.0, 2.0, 3.0, 90.0, 91.0, 99.0, 100.0]  # four windows of 25: rows 4 + 0 + 0 + 4... by time
        edges = rr.window_edges(arrivals, 4)
        self.assertEqual(edges, [0, 4, 4, 4, 8])

    def test_validity_reasons(self):
        ok = {"messages": 10, "completed": 10, "dlq": 0, "submit_errors": 0, "leases_lost": 0, "timing": {"lateness_p99_ms": 3.0}}
        self.assertEqual(rr.validity(ok), [])
        self.assertEqual(len(rr.validity({**ok, "completed": 9})), 1)
        self.assertEqual(len(rr.validity({**ok, "dlq": 1, "leases_lost": 2})), 2)
        self.assertEqual(len(rr.validity({**ok, "timing": {"lateness_p99_ms": 25.0}})), 1)
        self.assertEqual(len(rr.validity({**ok, "timing": {}})), 1)


class Analysis(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def run_set(self, arms: dict[str, list[float]], load: str = "0.8"):
        for arm, factors in arms.items():
            for n, factor in enumerate(factors, 1):
                make_run(self.root / arm / load / f"rep-{n}", factor)
        cells = rr.collect(self.root, CLASS_BOUNDS, (WINDOWS, 7))
        return cells, rr.evaluate_cells(CONFIG, cells, f"windows_{WINDOWS}", 4, CLASS_BOUNDS)

    def test_time_scale_returns_latency_to_trace_time(self):
        cells, _ = self.run_set({"fifo": [1.0]})
        mean = cells["0.8"]["fifo"]["reps"]["rep-1"]["all"]["mean_latency_ms"]
        self.assertAlmostEqual(mean, 1000 + 10 * 2.9, delta=2.0)  # mean of 10 (i mod 7) is about 29/10 x 10
        wait = cells["0.8"]["fifo"]["reps"]["rep-1"]["all"]["long_max_wait_ms"]
        self.assertAlmostEqual(wait, 100.0, places=3)

    def test_classes_follow_the_registered_bounds(self):
        cells, _ = self.run_set({"fifo": [1.0]})
        all_ = cells["0.8"]["fifo"]["reps"]["rep-1"]["all"]
        self.assertEqual((all_["short_count"], all_["medium_count"], all_["long_count"]), (934, 933, 933))

    def test_a_faster_arm_passes_and_an_equal_one_fails(self):
        _, ev = self.run_set({"fifo": [1.0], "predictive_warm": [0.4]})
        self.assertTrue(ev["0.8"]["gates_passed"]["A"])
        self.assertAlmostEqual(ev["0.8"]["criteria"]["A_P1_mean_latency_vs_fifo"]["point"], -0.6, places=6)
        _, ev = self.run_set({"fifo": [1.0], "predictive_warm": [0.95]}, load="0.95")
        self.assertFalse(ev["0.95"]["gates_passed"]["A"])
        self.assertEqual(ev["0.95"]["criteria"]["A_P1_mean_latency_vs_fifo"]["status"], "fail")

    def test_invalid_run_is_left_out_and_repeats_are_paired(self):
        make_run(self.root / "fifo/0.8/rep-1", 1.0)
        make_run(self.root / "fifo/0.8/rep-2", 1.0)
        make_run(self.root / "predictive_warm/0.8/rep-1", 0.4)
        make_run(self.root / "predictive_warm/0.8/rep-2", 0.4, dlq=3)  # not valid
        cells = rr.collect(self.root, CLASS_BOUNDS, (WINDOWS, 7))
        ev = rr.evaluate_cells(CONFIG, cells, f"windows_{WINDOWS}", 4, CLASS_BOUNDS)["0.8"]
        self.assertEqual(ev["repeats_used"], ["rep-1"])
        self.assertIn("3 dead-lettered", cells["0.8"]["predictive_warm"]["reps"]["rep-2"]["reasons"][0])
        self.assertIn("rep-2", rr.report(CONFIG, cells, {"0.8": ev}, {}, {}, {}, {}))

    def test_pooling_averages_the_repeats(self):
        cells, ev = self.run_set({"fifo": [1.0, 1.2], "predictive_warm": [0.4, 0.4]})
        fifo_mean = ev["0.8"]["all"]["fifo"]["mean_latency_ms"]
        single = cells["0.8"]["fifo"]["reps"]["rep-1"]["all"]["mean_latency_ms"]
        self.assertAlmostEqual(fifo_mean, single * 1.1, places=6)
        spread = rr.noise(cells)["0.8"]["fifo"]
        self.assertAlmostEqual(spread["max_over_min"], 1.2, places=6)
        repeats = rr.per_repeat_verdicts(CONFIG, cells, f"windows_{WINDOWS}", 4, CLASS_BOUNDS)
        self.assertEqual(sorted(repeats["0.8"]), ["rep-1", "rep-2"])

    def test_block_and_independent_intervals_both_available_and_block_is_not_narrower(self):
        cells, block = self.run_set({"fifo": [1.0], "predictive_warm": [0.4]})
        indep = rr.evaluate_cells(CONFIG, cells, "windows_7", None, CLASS_BOUNDS)
        self.assertIn("gates_passed", indep["0.8"])
        self.assertEqual(block["0.8"]["criteria"]["A_P1_mean_latency_vs_fifo"]["status"], "pass")

    def test_no_fifo_is_reported_not_crashed(self):
        _, ev = self.run_set({"predictive_warm": [0.4]})
        self.assertIn("error", ev["0.8"])

    def test_simulator_comparison_gives_ratio_and_agreement(self):
        cells, ev = self.run_set({"fifo": [1.0], "predictive_warm": [0.5]})
        sim = {"results": {"0.8": {"fifo": {"all": {"mean_latency_ms": 1000.0}}, "predictive_warm": {"all": {"mean_latency_ms": 400.0}}}}}
        summary = {"evaluation": {"0.8": {"gates_passed": {"A": False}}}}
        out = rr.simulator_comparison(ev, sim, summary)["0.8"]
        self.assertAlmostEqual(out["mean_latency_change"]["predictive_warm"]["ratio"], 0.5 / 0.6, places=3)
        self.assertEqual(out["gates"]["A"], {"replay": True, "simulator": False, "same": False})


if __name__ == "__main__":
    unittest.main()
