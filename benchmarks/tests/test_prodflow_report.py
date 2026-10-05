import copy
import json
from pathlib import Path
import unittest

from benchmarks.prodflow_report import evaluate

CONFIG = json.loads((Path(__file__).resolve().parents[1] / "configs/v2-prodflow.json").read_text())


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


if __name__ == "__main__":
    unittest.main()
