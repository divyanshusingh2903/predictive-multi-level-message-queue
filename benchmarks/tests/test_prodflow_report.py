import copy
import json
from pathlib import Path
import unittest

from benchmarks.prodflow_report import evaluate, metrics as run_metrics

CONFIG = json.loads((Path(__file__).resolve().parents[1] / "configs/v2-prodflow.json").read_text())
V3 = json.loads((Path(__file__).resolve().parents[1] / "configs/v3-sizebins.json").read_text())


def metrics(mean, short=None, p99=None, wait=1000.0, throughput=100.0, completed=1000, dlq=0, lookup=3.0):
    return {"mean_latency_ms": mean, "short_p50_ms": short if short is not None else mean / 2,
            "p99_latency_ms": p99 if p99 is not None else mean * 5, "long_max_wait_ms": wait,
            "throughput_per_s": throughput, "messages": 1000, "completed": completed, "dlq": dlq, "submit_errors": 0,
            "lookup_mean_us": lookup}


def table(predictive_factor=0.6, p99_factor=1.0, seeds=(11, 22, 33, 44, 55)):
    t = {}
    for i, seed in enumerate(seeds):
        fifo = 1000 + 10 * i
        t.setdefault("fifo", {})[seed] = metrics(fifo)
        t.setdefault("static_tuned", {})[seed] = metrics(fifo * 0.65)
        t.setdefault("static_misconfigured", {})[seed] = metrics(fifo * 1.2)
        t.setdefault("shadow", {})[seed] = metrics(fifo * (1.001 if i % 2 else 0.999))
        t.setdefault("predictive_producer", {})[seed] = metrics(fifo * 0.9)
        t.setdefault("predictive", {})[seed] = metrics(fifo * predictive_factor, short=fifo / 2 * 0.5,
                                                      p99=fifo * 5 * p99_factor)
    return t


class ProdflowReport(unittest.TestCase):
    def test_clear_improvement_passes_gate(self):
        result = evaluate(CONFIG, table())
        self.assertTrue(result["gate_passed"], result)
        self.assertEqual(result["criteria"]["S3_shadow_overhead"]["status"], "pass")
        self.assertEqual(result["criteria"]["S4_zero_config_vs_fifo"]["status"], "pass")

    def test_tail_harm_fails_guardrail_and_gate(self):
        result = evaluate(CONFIG, table(p99_factor=1.6))
        self.assertEqual(result["criteria"]["G1_all_p99_vs_fifo"]["status"], "fail")
        self.assertFalse(result["gate_passed"])

    def test_small_or_absent_effect_does_not_pass(self):
        result = evaluate(CONFIG, table(predictive_factor=0.95))
        self.assertEqual(result["criteria"]["P1_mean_latency_vs_fifo"]["status"], "fail")
        missing = copy.deepcopy(table())
        del missing["predictive"]
        self.assertEqual(evaluate(CONFIG, missing)["criteria"]["P1_mean_latency_vs_fifo"]["status"], "missing_arm")

    def test_lost_work_fails(self):
        t = table()
        t["predictive"][11]["dlq"] = 1
        self.assertEqual(evaluate(CONFIG, t)["criteria"]["G3_no_lost_work"]["status"], "fail")


READING = {"margin": 0.03, "labels": {"up": "worse", "tied": "tied", "down": "better"}}
REMOVAL = {"margin": 0.03, "labels": {"up": "supported", "tied": "not supported here", "down": "harmful here"}}


def reading_config(**criteria):
    return {"bootstrap": {"samples": 500, "seed": 1, "confidence": 0.95}, "primary_arm": "predictive", "gate": [],
            "criteria": criteria,
            "groups": {"decay": {"members": ["d_all", "d_shifted"], "labels": REMOVAL["labels"]}}}


def paired(arm_factors: dict[str, list[float]], seeds=range(10)):
    """arm -> per-seed factor of a 1000 ms baseline; the shifted-phase mean is the same factor of 2000 ms."""
    t = {}
    for arm, factors in arm_factors.items():
        for seed, f in zip(seeds, factors):
            t.setdefault(arm, {})[seed] = {**metrics(1000 * f), "phase_mean_ms:shifted": 2000 * f}
    return t


class Readings(unittest.TestCase):
    def check(self, arm_factors, **kw):
        config = reading_config(x={"arm": "predictive", "baseline": "other", "metric": "mean_latency_ms", "reading": READING})
        return evaluate(config, paired({"predictive": arm_factors[0], "other": arm_factors[1]}))["criteria"]["x"]

    def test_clear_win_tie_loss_and_inconclusive(self):
        base = [1.0] * 10
        self.assertEqual(self.check(([0.6] * 10, base))["status"], "better")
        self.assertEqual(self.check(([1.01, 0.99] * 5, base))["status"], "tied")
        self.assertEqual(self.check(([1.3] * 10, base))["status"], "worse")
        # Wide scatter around a small mean: neither tied (interval outside +-3%) nor clearly better or worse.
        self.assertEqual(self.check(([0.8, 1.25] * 5, base))["status"], "inconclusive")

    def test_a_tiny_but_certain_gain_is_tied_not_better(self):
        self.assertEqual(self.check(([0.99] * 10, [1.0] * 10))["status"], "tied")  # -1% with no spread, inside the 3% margin

    def test_removal_groups_combine_overall_and_shifted(self):
        def group(overall, shifted):
            config = reading_config(
                d_all={"arm": "ablate_decay", "baseline": "predictive", "metric": "mean_latency_ms", "reading": REMOVAL},
                d_shifted={"arm": "ablate_decay", "baseline": "predictive", "metric": "phase_mean_ms:shifted", "reading": REMOVAL})
            # both metrics come from one arm table, so scale by arm: overall and shifted share the factor unless set apart
            t = paired({"predictive": [1.0] * 10, "ablate_decay": [overall] * 10})
            for seed in t["ablate_decay"]:
                t["ablate_decay"][seed]["phase_mean_ms:shifted"] = 2000 * shifted
            return evaluate(config, t)["groups"]["decay"]
        self.assertEqual(group(1.0, 1.0), "not supported here")
        self.assertEqual(group(1.0, 1.2), "supported")      # the component matters after the shift only
        self.assertEqual(group(0.9, 1.0), "harmful here")
        self.assertEqual(group(1.2, 0.9), "mixed")

    def test_report_only_gives_the_change_without_a_verdict(self):
        config = reading_config(x={"arm": "predictive", "baseline": "other", "metric": "long_max_wait_ms", "report_only": True})
        t = paired({"predictive": [1.0] * 10, "other": [1.0] * 10})
        out = evaluate(config, t)["criteria"]["x"]
        self.assertEqual(out["status"], "reported")
        self.assertIn("ci95", out)

    def test_the_v4_baselines_config_is_consistent(self):
        path = Path(__file__).resolve().parents[1] / "configs/v4-baselines.json"
        if not path.exists():
            self.skipTest("v4-baselines.json is written at freeze")
        c = json.loads(path.read_text())
        arms = set(c["arms"])
        for name, rule in c["criteria"].items():
            self.assertIn(rule.get("arm", c["primary_arm"]), arms, name)
            if "baseline" in rule:
                self.assertIn(rule["baseline"], arms, name)
        for group in c.get("groups", {}).values():
            for member in group["members"]:
                self.assertIn(member, c["criteria"])
        self.assertEqual(c["seeds"], [11, 22, 33, 44, 55, 66, 77, 88, 99, 110])
        self.assertTrue(c.get("phase_metrics"))


def v3_table(sized_factor=0.9, unsized_factor=1.0, seeds=(11, 22, 33, 44, 55)):
    t = {}
    for i, seed in enumerate(seeds):
        fifo = 1000 + 10 * i
        base = metrics(fifo * 0.7)
        base.update({"unsized_mean_ms": fifo * 0.6, "class_mean_ms:resize_image": 4000.0 + i,
                     "shifted_class_mean_ms:resize_image": 6000.0 + i, "class_mean_ms:generate_invoice": 300.0 + i})
        sized = dict(base)
        sized.update({"mean_latency_ms": base["mean_latency_ms"] * sized_factor,
                      "unsized_mean_ms": base["unsized_mean_ms"] * unsized_factor,
                      "class_mean_ms:resize_image": base["class_mean_ms:resize_image"] * 0.7})
        t.setdefault("fifo", {})[seed] = metrics(fifo)
        t.setdefault("predictive", {})[seed] = base
        t.setdefault("predictive_sized", {})[seed] = sized
    return t


class SizeBinsReport(unittest.TestCase):
    def test_gain_without_harm_passes(self):
        result = evaluate(V3, v3_table())
        self.assertTrue(result["gate_passed"], result)
        self.assertEqual(result["criteria"]["S1_resize_mean_vs_predictive"]["status"], "pass")
        self.assertEqual(result["criteria"]["S3_invoice_mean_vs_predictive"]["status"], "fail")  # unchanged class

    def test_small_gain_fails_primary_and_harm_to_other_classes_fails_guardrail(self):
        self.assertEqual(evaluate(V3, v3_table(sized_factor=0.99))["criteria"]["P1_mean_latency_vs_predictive"]["status"], "fail")
        result = evaluate(V3, v3_table(unsized_factor=1.08))
        self.assertEqual(result["criteria"]["G2_unsized_classes_vs_predictive"]["status"], "fail")
        self.assertFalse(result["gate_passed"])

    def test_metrics_expose_per_class_and_unsized_means(self):
        def row(seq, job, phase, latency_ms):
            return {"seq": str(seq), "job_type": job, "phase": str(phase), "done": "1", "submit_ns": "0",
                    "first_start_ns": "0", "end_ns": str(int(latency_ms * 1e6)), "handler_ms": "1"}
        rows = [row(0, "resize_image", 0, 100), row(1, "resize_image", 4, 300), row(2, "send_sms", 4, 10),
                row(3, "export_report", 1, 1000), row(4, "push_notification", 0, 30)]
        m = run_metrics({"dlq": 0, "submit_errors": 0}, rows, V3)
        self.assertEqual(m["class_mean_ms:resize_image"], 200)
        self.assertEqual(m["shifted_class_mean_ms:resize_image"], 300)
        self.assertIsNone(m["shifted_class_mean_ms:export_report"])
        self.assertEqual(m["unsized_mean_ms"], 20)
        self.assertNotIn("unsized_mean_ms", run_metrics({"dlq": 0, "submit_errors": 0}, rows, CONFIG))


    def test_every_seed_checks_need_no_interval(self):
        config = {"bootstrap": {"samples": 100, "seed": 1, "confidence": 0.95}, "primary_arm": "predictive", "gate": [],
                  "criteria": {
                      "faster": {"metric": "mean_latency_ms", "baseline": "fifo", "max_relative_delta": 0.0, "every_seed": True},
                      "shadow_close": {"arm": "shadow", "metric": "mean_latency_ms", "baseline": "fifo",
                                       "max_abs_relative_delta": 0.05, "every_seed": True}}}
        t = table(seeds=(101, 202))
        result = evaluate(config, t)["criteria"]
        self.assertEqual((result["faster"]["status"], result["shadow_close"]["status"]), ("pass", "pass"))
        self.assertEqual(len(result["faster"]["deltas"]), 2)
        t["predictive"][202]["mean_latency_ms"] = t["fifo"][202]["mean_latency_ms"] * 1.01  # slower in one repeat
        self.assertEqual(evaluate(config, t)["criteria"]["faster"]["status"], "fail")

if __name__ == "__main__":
    unittest.main()
