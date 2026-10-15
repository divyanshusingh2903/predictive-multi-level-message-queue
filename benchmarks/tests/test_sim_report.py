import unittest

from benchmarks.sim_report import evaluate

CRITERION = {"baseline": "fifo", "metric": "mean_latency_ms", "max_relative_delta": -0.15}
BASE = {"bootstrap": {"samples": 200, "seed": 1, "confidence": 0.95},
        "criteria": {"good": {**CRITERION, "arm": "fast"}, "bad": {**CRITERION, "arm": "slow"}}}


def results(**factors):
    windows = [{"mean_latency_ms": 100.0 + i} for i in range(7)]
    arms = {"fifo": {"windows": windows}}
    for arm, factor in factors.items():
        arms[arm] = {"windows": [{"mean_latency_ms": w["mean_latency_ms"] * factor} for w in windows]}
    return {"0.8": arms}


class SimReportGates(unittest.TestCase):
    def test_single_gate_is_unchanged(self):
        out = evaluate({**BASE, "gate": ["good", "bad"]}, results(fast=0.5, slow=1.0))
        self.assertEqual(out["0.8"]["gate_passed"], False)
        self.assertNotIn("gates_passed", out["0.8"])

    def test_named_gates_are_judged_separately(self):
        config = {**BASE, "gates": {"A": ["good"], "B": ["bad"]}}
        out = evaluate(config, results(fast=0.5, slow=1.0))
        self.assertEqual(out["0.8"]["gates_passed"], {"A": True, "B": False})
        self.assertNotIn("gate_passed", out["0.8"])


    def test_floor_makes_idle_windows_comparable(self):
        windows = [{"long_max_wait_ms": w} for w in (0.0, 0.0, 5000.0, 8000.0, 0.0, 3000.0, 0.0)]
        arms = {"fifo": {"windows": windows},
                "fast": {"windows": [{"long_max_wait_ms": w * 1.2} for w in (0.0, 0.0, 5000.0, 8000.0, 0.0, 3000.0, 0.0)]}}
        rule = {"arm": "fast", "baseline": "fifo", "metric": "long_max_wait_ms", "max_ratio": 2.0}
        config = {**BASE, "criteria": {"g2": rule}, "gate": ["g2"]}
        self.assertEqual(evaluate(config, {"0.8": arms})["0.8"]["criteria"]["g2"]["status"], "unevaluable")
        config["criteria"]["g2"] = {**rule, "floor": 1000.0}
        verdict = evaluate(config, {"0.8": arms})["0.8"]["criteria"]["g2"]
        self.assertEqual(verdict["status"], "pass")
        self.assertLess(verdict["point"], 1.2)  # idle windows count as 1.0


    def test_floor_follows_long_class_bound_and_empty_class_cannot_pass(self):
        # A millisecond-scale trace: "long" registered as >= 100 ms, so the floor is 100 ms, not 1 s.
        def arms(fifo_waits, arm_waits, long_count):
            make = lambda waits: {"windows": [{"long_max_wait_ms": w, "long_count": long_count} for w in waits]}
            return {"0.8": {"fifo": make(fifo_waits), "fast": make(arm_waits)}}
        rule = {"arm": "fast", "baseline": "fifo", "metric": "long_max_wait_ms", "max_ratio": 2.0,
                "floor": "long_class_bound", "min_class_count": {"class": "long", "count": 30}}
        config = {**BASE, "criteria": {"g2": rule}, "gate": ["g2"]}
        data = {"class_bounds_ms": [10, 100]}
        # FIFO never makes a long job wait; the arm makes them wait 500 ms: 5x the 100 ms floor, a failure.
        verdict = evaluate(config, arms([0.0] * 7, [500.0] * 7, 50), data=data)["0.8"]["criteria"]["g2"]
        self.assertEqual(verdict["status"], "fail")
        self.assertAlmostEqual(verdict["point"], 5.0)
        # With only a handful of long jobs per window the guardrail is not evaluable, so it cannot pass vacuously.
        verdict = evaluate(config, arms([0.0] * 7, [0.0] * 7, 3), data=data)["0.8"]["criteria"]["g2"]
        self.assertEqual(verdict["status"], "unevaluable")
        with self.assertRaises(ValueError):  # the symbolic floor needs the simulation's class bounds
            evaluate(config, arms([0.0] * 7, [0.0] * 7, 50))


if __name__ == "__main__":
    unittest.main()


class BlockBootstrap(unittest.TestCase):
    def test_block_one_covers_the_mean_and_constant_series_is_exact(self):
        from benchmarks.sim_report import block_bootstrap
        point, ci = block_bootstrap([-0.3] * 28, 4, 500, 1, 0.95)
        self.assertEqual((point, ci), (-0.3, [-0.3, -0.3]))
        point, ci = block_bootstrap([float(i % 5) for i in range(28)], 1, 500, 1, 0.95)
        self.assertLess(ci[0], point)
        self.assertGreater(ci[1], point)

    def test_dependence_widens_the_interval(self):
        # A slowly drifting series: neighbours are alike, so independent resampling understates the spread.
        from benchmarks.sim_report import block_bootstrap
        series = [(i // 7) * 1.0 for i in range(28)]
        width = lambda block: (lambda ci: ci[1] - ci[0])(block_bootstrap(series, block, 4000, 7, 0.95)[1])
        self.assertGreater(width(7), width(1))

    def test_block_longer_than_series_is_refused(self):
        from benchmarks.sim_report import block_bootstrap
        with self.assertRaises(ValueError):
            block_bootstrap([1.0] * 5, 6, 10, 1, 0.95)

    def test_evaluate_without_block_is_unchanged_by_the_option(self):
        config = {**BASE, "gate": ["good", "bad"]}
        self.assertEqual(evaluate(config, results(fast=0.5, slow=1.0)), evaluate(config, results(fast=0.5, slow=1.0), block=None))
        out = evaluate(config, results(fast=0.5, slow=1.0), block=2)
        self.assertEqual(out["0.8"]["criteria"]["good"]["status"], "pass")


class Sensitivity(unittest.TestCase):
    def make(self, factors):
        """Outputs per variant id: A arm at `a` times FIFO, B arm at `b` times FIFO (seven windows)."""
        outputs = {}
        for vid, (a, b) in factors.items():
            res = results(predictive_warm=a, **{"predictive_warm+weights+pausing": b})["0.8"]
            outputs[vid] = {"results": {"0.8": res}, "class_bounds_ms": [100, 1000]}
        return outputs

    def test_readings_flag_flips_and_cliffs(self):
        from benchmarks.sim_report import sensitivity
        criteria = {"A_P1_mean_latency_vs_fifo": {**CRITERION, "arm": "predictive_warm"},
                    "B_P1_mean_latency_vs_fifo": {**CRITERION, "arm": "predictive_warm+weights+pausing"}}
        manifest = {"variants": [
            {"id": "default", "parameter": "default", "value": "d", "default": True, "step": 0},
            {"id": "kappa-4", "parameter": "kappa", "value": 4, "default": False, "step": 1},
            {"id": "kappa-inf", "parameter": "kappa", "value": "inf", "default": False, "step": 2},
            {"id": "decay-1.0", "parameter": "decay", "value": 1.0, "default": False, "step": 2}]}
        outputs = self.make({"default": (0.5, 0.5), "kappa-4": (0.95, 0.5), "kappa-inf": (0.5, 0.5), "decay-1.0": (0.5, 0.5)})
        out = sensitivity(manifest, outputs, {"bootstrap": BASE["bootstrap"], "criteria": criteria})
        a = out["readings"]["A_P1_mean_latency_vs_fifo"]
        self.assertEqual(a["kappa"], {"robust": False, "flips_at": [4], "cliff": True})  # flips one step from the default
        self.assertTrue(a["decay"]["robust"])
        self.assertTrue(out["readings"]["B_P1_mean_latency_vs_fifo"]["kappa"]["robust"])
        self.assertEqual(out["passing_variants"], {"A_P1_mean_latency_vs_fifo": 3, "B_P1_mean_latency_vs_fifo": 4})


class VerdictChanges(unittest.TestCase):
    def test_lists_only_differing_cells(self):
        from benchmarks.sim_report import verdict_changes
        old = {"0.8": {"gates_passed": {"A": False, "B": True}}, "0.95": {"gates_passed": {"A": False, "B": True}}}
        new = {"0.8": {"gates_passed": {"A": False, "B": False}}, "0.95": {"gates_passed": {"A": True, "B": True}}}
        self.assertEqual(verdict_changes(old, new), ["load 0.80 gate B: pass -> fail", "load 0.95 gate A: fail -> pass"])


class SensitivityConfigs(unittest.TestCase):
    def test_grid_has_eighteen_variants_with_one_default(self):
        from benchmarks.make_sensitivity_configs import variants
        listing = variants()
        self.assertEqual(len(listing), 18)
        self.assertEqual(sum(v["default"] for v in listing), 1)
        self.assertEqual(len({v["id"] for v in listing}), 18)

    def test_variants_change_exactly_one_thing(self):
        import json
        from pathlib import Path
        from benchmarks.make_sensitivity_configs import variant_config, variants, window_config
        base = json.loads(Path("benchmarks/configs/v3-traces-azure2021.json").read_text())
        default = variant_config(base, variants()[0], None)
        self.assertEqual(default["utilizations"], [0.8])
        self.assertNotIn("predictor", default)
        by_id = {v["id"]: v for v in variants()}
        self.assertEqual(variant_config(base, by_id["kappa-inf"], None)["predictor"], {"max_spread_ratio": 1e18})
        self.assertEqual(variant_config(base, by_id["tau-off"], None)["predictor"], {"stale_after_ms": 0})
        self.assertEqual(variant_config(base, by_id["aging_threshold-30s"], None)["aging"], {"threshold_ms": 30000, "interval_ms": 3000})
        four = variant_config(base, by_id["levels-4"], None)
        self.assertEqual((four["levels"], len(four["level_weights"])), (4, 4))
        self.assertEqual(window_config(base, None)["evaluation_days"], 28)
        self.assertEqual(window_config(base, None)["arms"], base["arms"])
