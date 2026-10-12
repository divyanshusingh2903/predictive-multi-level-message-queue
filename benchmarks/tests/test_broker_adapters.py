"""Production-queue arms of harbinger_prodflow (#37, #47).

The argument checks need only the binary (HARBINGER_PRODFLOW). The end-to-end checks also need a build with
HARBINGER_BUILD_BROKER_ADAPTERS and running servers, named by HARBINGER_RABBITMQ and HARBINGER_REDIS (HOST:PORT), for
example from benchmarks/docker:
  docker run -d --network host harbinger-bench rabbitmq ; docker run -d --network host harbinger-bench redis
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BINARY = os.environ.get("HARBINGER_PRODFLOW")
RABBITMQ = os.environ.get("HARBINGER_RABBITMQ")
REDIS = os.environ.get("HARBINGER_REDIS")


def run(*args, timeout=120):
    return subprocess.run([BINARY, *args], capture_output=True, text=True, timeout=timeout)


@unittest.skipUnless(BINARY, "HARBINGER_PRODFLOW not set (build with HARBINGER_BUILD_BENCHMARKS)")
class ExternalArmArguments(unittest.TestCase):
    def test_external_arm_needs_a_broker_address_and_no_broker_role(self):
        with tempfile.TemporaryDirectory() as tmp:
            for args in (["--arm", "redis_fifo", "--seed", "1", "--out", f"{tmp}/a"],
                         ["--role", "broker", "--arm", "rabbitmq_fifo", "--broker", "x:1", "--out", f"{tmp}/b"]):
                with self.subTest(args=args):
                    result = run(*args)
                    self.assertEqual(result.returncode, 2)
                    self.assertIn("--broker HOST:PORT", result.stderr)
                    self.assertFalse(Path(args[-1]).exists())


class EndToEnd(unittest.TestCase):
    def check_arm(self, arm, broker, kind, version_prefix):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / arm
            result = run("--arm", arm, "--broker", broker, "--seed", "5", "--seconds", "4", "--rate-scale", "0.5",
                         "--drain-s", "60", "--workers", "2", "--out", str(out))
            self.assertEqual(result.returncode, 0, result.stderr)
            record = json.loads((out / "run.json").read_text())
            self.assertEqual(record["completed"], record["messages"])
            self.assertEqual((record["dlq"], record["submit_errors"]), (0, 0))
            self.assertEqual(record["broker"]["kind"], kind)
            self.assertTrue(record["broker"]["server_version"].startswith(version_prefix))
            self.assertNotIn("worker_error", record)
            rows = (out / "messages.tsv").read_text().splitlines()
            self.assertEqual(len(rows) - 1, record["messages"])
            return record

    @unittest.skipUnless(BINARY and RABBITMQ, "HARBINGER_RABBITMQ not set")
    def test_rabbitmq_arms(self):
        for arm in ("rabbitmq_fifo", "rabbitmq_priority"):
            with self.subTest(arm=arm):
                self.assertFalse(self.check_arm(arm, RABBITMQ, "rabbitmq", "3.")["broker"]["routing"]["enabled"])
        routing = self.check_arm("rabbitmq_predictive", RABBITMQ, "rabbitmq", "3.")["broker"]["routing"]
        self.assertTrue(routing["enabled"])
        self.assertGreater(routing["lookups"], 0)
        self.assertEqual(sum(routing["outcomes"].values()), routing["lookups"])

    @unittest.skipUnless(BINARY and REDIS, "HARBINGER_REDIS not set")
    def test_redis_fifo(self):
        self.check_arm("redis_fifo", REDIS, "redis", "7.")


if __name__ == "__main__":
    unittest.main()
