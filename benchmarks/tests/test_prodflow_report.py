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


if __name__ == "__main__":
    unittest.main()
