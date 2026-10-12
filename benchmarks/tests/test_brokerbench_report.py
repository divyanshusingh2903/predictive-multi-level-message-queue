import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from benchmarks.brokerbench_report import load, report

CONFIG = {"version": "test", "kinds": ["harbinger", "redis"], "rates": [0, 1000], "repetitions": 3,
          "seconds": 10, "warmup_s": 2, "producers": 4, "consumers": 4, "payload_bytes": 64}
BINARY = os.environ.get("HARBINGER_BROKERBENCH")


def run_file(kind, rate, delivered, p99, lost=0):
    record = {"kind": kind, "rate": rate, "lost": lost, "publish_errors": 0, "duplicates": 0,
              "send_rate_per_s": delivered * 1.1, "delivery_rate_per_s": delivered,
              "latency_ms": {"p50": p99 / 2, "p90": p99 / 1.5, "p99": p99, "p999": p99 * 2, "max": p99 * 3}}
    if rate:
        record["lateness_ms"] = {"p50": 0.01, "p90": 0.02, "p99": 0.05, "p999": 0.1, "max": 1.0}
    return record


class BrokerbenchReport(unittest.TestCase):
    def test_medians_across_repetitions_and_tables(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for rep, factor in enumerate((1.0, 1.2, 0.9), start=1):
                for kind, peak in (("harbinger", 5000), ("redis", 20000)):
                    for rate in (0, 1000):
                        path = root / f"rep-{rep}" / kind / f"rate-{rate}.json"
                        path.parent.mkdir(parents=True, exist_ok=True)
                        delivered = peak * factor if rate == 0 else 1000
                        path.write_text(json.dumps(run_file(kind, rate, delivered, 0.3 * factor, lost=rep == 2)))
            text, summary = report(CONFIG, load(root))
        self.assertEqual(summary["harbinger"]["0.0"]["delivery_rate_per_s"], 5000)
        self.assertEqual(summary["harbinger"]["0.0"]["delivery_rate_range"], [4500, 6000])
        self.assertEqual(summary["redis"]["1000.0"]["lost"], 1)
        self.assertAlmostEqual(summary["redis"]["1000.0"]["latency_ms"]["p99"], 0.3)
        self.assertIn("## Offered 1,000 msg/s", text)
        self.assertIn("| redis | 20,000 |", text)
        self.assertEqual(summary["redis"]["1000.0"]["kept_up"], 2)  # the repetition that lost a message did not
        self.assertIn("| harbinger | 2/3 | 1,000 |", text)


@unittest.skipUnless(BINARY, "HARBINGER_BROKERBENCH not set")
class BrokerbenchArguments(unittest.TestCase):
    def test_rejects_unknown_kind_and_existing_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            existing = Path(tmp) / "x.json"
            existing.write_text("{}")
            for extra in (["--kind", "kafka", "--out", f"{tmp}/y.json"], ["--kind", "harbinger", "--out", str(existing)]):
                with self.subTest(extra=extra):
                    result = subprocess.run([BINARY, "--broker", "127.0.0.1:1", *extra], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 2)
            self.assertEqual(existing.read_text(), "{}")


if __name__ == "__main__":
    unittest.main()
