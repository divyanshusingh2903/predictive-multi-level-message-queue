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
