"""Simulator validation pipeline (v4-simvalidation). Uses only synthetic runs: the registered measured runs are never read
here, so the pre-registration's "the simulator has not seen this workload" stays true until the registered analysis.

The end-to-end checks need the simulator binary (HARBINGER_TRACE_SIM)."""
import json
import os
from pathlib import Path
import tempfile
import unittest

from analysis import trace_format
from benchmarks import sim_validation

SIM = os.environ.get("HARBINGER_TRACE_SIM")
COLUMNS = ["seq", "service", "job_type", "phase", "planned_ms", "submit_ns", "first_start_ns", "start_ns", "end_ns",
           "attempts", "handler_ms", "done"]


def write_run(directory: Path, jobs, workers=1, bounds=(20.0, 50.0), done=True):
    """jobs: (job_type, planned_ms, submit_ms, handler_ms). The real timestamps are unused by the simulator."""
    directory.mkdir(parents=True)
    with (directory / "messages.tsv").open("w") as stream:
        stream.write("\t".join(COLUMNS) + "\n")
        for seq, (job_type, planned, submit, handler) in enumerate(jobs):
            submit_ns = int(submit * 1e6)
            row = [seq, "svc", job_type, 0, planned, submit_ns, submit_ns, submit_ns, submit_ns + int(handler * 1e6), 1,
                   handler, 1 if done else 0]
            stream.write("\t".join(map(str, row)) + "\n")
    (directory / "run.json").write_text(json.dumps(
        {"arm": "fifo", "seed": 1, "workers": workers, "oracle_boundaries_ms": list(bounds), "dlq": 0, "submit_errors": 0}))


# One worker: a long job at 0, a second long at 0.5 ms, a short at 1 ms.
JOBS = [("export_report", 100.0, 0.0, 100.0), ("export_report", 100.0, 0.5, 100.0), ("reserve_inventory", 10.0, 1.0, 10.0)]


class Converter(unittest.TestCase):
    def test_rows_are_sorted_by_arrival_and_carry_planned_cost_as_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            write_run(Path(tmp) / "run", [JOBS[2], JOBS[0], JOBS[1]])
            rows, notes = trace_format.from_prodflow(Path(tmp) / "run")
        self.assertEqual([r.key for r in rows], ["export_report", "export_report", "reserve_inventory"])
        self.assertEqual(rows[0].arrival_ms, 0.0)
        self.assertEqual([r.size for r in rows], [100.0, 100.0, 10.0])
        self.assertEqual(notes["oracle_boundaries_ms"], [20.0, 50.0])

    def test_a_run_with_lost_work_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            write_run(Path(tmp) / "run", JOBS, done=False)
            with self.assertRaises(ValueError):
                trace_format.from_prodflow(Path(tmp) / "run")


@unittest.skipUnless(SIM, "HARBINGER_TRACE_SIM not set (build the harbinger_trace_sim target)")
class Replay(unittest.TestCase):
    def latencies(self, arm):
        with tempfile.TemporaryDirectory() as tmp:
            write_run(Path(tmp) / "run", JOBS)
            rows = sim_validation.simulate(Path(SIM), Path(tmp), Path(tmp) / "run", arm, {})
        return [round(int(r["end_ns"]) / 1e6, 3) for r in rows]

    def test_fifo_serves_in_arrival_order(self):
        # 0-100, 100-200, 200-210: the short job waits behind both long ones.
        self.assertEqual(self.latencies("fifo"), [100.0, 199.5, 209.0])

    def test_static_map_and_fixed_bounds_oracle_let_the_short_job_pass(self):
        # The first long job is already running when the others arrive; at 100 ms the short job (tier 0) beats the second
        # long one (tier 2): short 100-110, second long 110-210.
        expected = [100.0, 209.5, 109.0]
        self.assertEqual(self.latencies("static_tuned"), expected)
        self.assertEqual(self.latencies("oracle"), expected)

    def test_a_cold_predictor_cannot_reorder_before_it_has_seen_anything(self):
        self.assertEqual(self.latencies("predictive"), [100.0, 199.5, 209.0])


class Summaries(unittest.TestCase):
    def metrics(self, mean, phase_mean=None, short=None):
        phase_mean = phase_mean if phase_mean is not None else mean
        return {"mean_latency_ms": mean, "short_p50_ms": short if short is not None else mean,
                "cohorts": {p: {"mean_ms": phase_mean} for p in sim_validation.PHASES}}

    def table(self, factors):
        return {arm: {s: self.metrics(1000.0 * factors[arm]) for s in ("11", "22")} for arm in sim_validation.ARMS}

    def test_matching_effects_pass_and_a_large_gap_fails(self):
        factors = {"fifo": 1.0, "predictive": 0.7, "static_tuned": 0.8, "static_misconfigured": 1.2, "oracle": 0.65}
        real = self.table(factors)
        same = sim_validation.summarise(real, self.table(factors), ["11", "22"])
        self.assertTrue(same["gate"]["passed"])
        off = sim_validation.summarise(real, self.table({**factors, "predictive": 0.5}), ["11", "22"])
        self.assertFalse(off["V1"]["predictive"]["pass"])
        self.assertFalse(off["gate"]["passed"])

    def test_swapping_predictive_and_oracle_does_not_fail_v3_but_crossing_groups_does(self):
        base = {"fifo": 1.0, "predictive": 0.7, "static_tuned": 0.8, "static_misconfigured": 1.2, "oracle": 0.65}
        real = self.table(base)
        inside = sim_validation.summarise(real, self.table({**base, "predictive": 0.64}), ["11", "22"])
        self.assertEqual(inside["V3"]["seeds_with_group_order"], 2)
        self.assertTrue(inside["gate"]["V3"])
        crossed = sim_validation.summarise(real, self.table({**base, "predictive": 0.9}), ["11", "22"])
        self.assertEqual(crossed["V3"]["seeds_with_group_order"], 0)
        self.assertFalse(crossed["gate"]["V3"])
        flipped = sim_validation.summarise(real, self.table({**base, "static_misconfigured": 0.95}), ["11", "22"])
        self.assertFalse(flipped["gate"]["V3"])

    def test_a_sign_flip_in_one_phase_fails_v2(self):
        factors = {"fifo": 1.0, "predictive": 0.7, "static_tuned": 0.8, "static_misconfigured": 1.2, "oracle": 0.65}
        real = self.table(factors)
        sim = self.table(factors)
        for seed in sim["predictive"]:
            sim["predictive"][seed]["cohorts"]["recovery"]["mean_ms"] = 1100.0  # slower than FIFO's 1000
        result = sim_validation.summarise(real, sim, ["11", "22"])
        self.assertFalse(result["V2"]["recovery"]["pass"])
        self.assertFalse(result["gate"]["V2"])


if __name__ == "__main__":
    unittest.main()
