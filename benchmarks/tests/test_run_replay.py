"""benchmarks/run_replay.sh, driven with the real harbinger_replay binary (REPLAY_BIN, no Docker) on a synthetic trace.

Needs HARBINGER_REPLAY; skipped without it, and run by ctest as `replay_python_tests` with the other replay tests.
"""
import json
import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

from benchmarks.tests.test_replay import config, write_trace

BINARY = os.environ.get("HARBINGER_REPLAY")
ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(BINARY, "set HARBINGER_REPLAY to the harbinger_replay binary")
class RunReplay(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        write_trace(self.dir / "trace.csv")
        c = config()
        c.update({"arms": ["fifo", "predictive_warm", "oracle"], "repeats": 2, "probe": {"load": 0.5, "seconds": 1}, "utilizations": [3],
                  "frozen_on": None, "trace": {"converted_sha256": "0" * 64}})
        (self.dir / "config.json").write_text(json.dumps(c))

    def tearDown(self):
        self.tmp.cleanup()

    def script(self, mode="debug", env=None, binary=None):
        e = {**os.environ, "REPLAY_BIN": binary or BINARY, **(env or {})}
        return subprocess.run(["bash", str(ROOT / "benchmarks/run_replay.sh"), mode, str(self.dir / "config.json"),
                               str(self.dir / "results"), str(self.dir / "prep"), str(self.dir / "trace.csv")],
                              cwd=ROOT, env=e, capture_output=True, text=True)

    def test_embedded_matrix_layout_and_report(self):
        done = self.script(env={"ARMS": "fifo oracle", "REPEATS": "2"})
        self.assertEqual(done.returncode, 0, done.stderr + done.stdout)
        res = self.dir / "results"
        for arm in ("fifo", "oracle"):
            for rep in ("rep-1", "rep-2"):
                self.assertTrue((res / arm / "3" / rep / "run.json").exists(), f"{arm} {rep}")
        self.assertIn("arm=fifo load=3 rep-1", (res / "progress.log").read_text())
        self.assertIn("trace trace.csv sha256", (res / "manifest.txt").read_text())
        # the probe is skipped when the matrix is narrowed
        self.assertFalse((res / "_probe").exists())
        # a second invocation skips finished cells
        again = self.script(env={"ARMS": "fifo oracle", "REPEATS": "2"})
        self.assertEqual(again.stdout.count("skip "), 4)

    def test_split_roles_and_probe(self):
        done = self.script(env={"BROKER_SSH": "", "BROKER_HOST": "127.0.0.1", "BROKER_WORKDIR": str(self.dir / "broker"),
                                "SSH": "false", "REPEATS": "1", "SPLIT": "1", "BROKER_LOCAL": "1"})
        # BROKER_SSH is empty, so this is an embedded run; the split path is exercised by test_split_with_local_ssh.
        self.assertEqual(done.returncode, 0, done.stderr + done.stdout)
        self.assertTrue((self.dir / "results/_probe/fifo/0.5/rep-1/run.json").exists())
        probe = json.loads((self.dir / "results/_probe/fifo/0.5/rep-1/run.json").read_text())
        self.assertEqual(probe["seconds_limit"], 1)
        self.assertLess(probe["messages"], 60)

    def test_split_with_local_ssh(self):
        fake_ssh = self.dir / "fakessh"
        fake_ssh.write_text('#!/usr/bin/env bash\nshift\nexec bash -c "$1"\n')  # "ssh HOST CMD" runs CMD here
        fake_ssh.chmod(fake_ssh.stat().st_mode | stat.S_IEXEC)
        done = self.script(env={"BROKER_SSH": "local", "SSH": str(fake_ssh), "BROKER_WORKDIR": str(self.dir / "broker"),
                                "ARMS": "predictive_warm", "REPEATS": "1"})
        self.assertEqual(done.returncode, 0, done.stderr + done.stdout)
        run = json.loads((self.dir / "results/predictive_warm/3/rep-1/run.json").read_text())
        self.assertEqual(run["role"], "split")
        self.assertEqual(run["broker"]["routing"]["outcomes"]["predicted"], 60)  # warm snapshot reached the remote broker

    def test_a_run_that_does_not_count_is_kept_and_rerun_once(self):
        wrapper = self.dir / "flaky"
        marker = self.dir / "spoiled"
        wrapper.write_text(f'''#!/usr/bin/env bash
"{BINARY}" "$@"; status=$?
if [ "$1" = run ] && [[ " $* " == *" --arm fifo "* ]] && [ ! -e {marker} ]; then
  touch {marker}
  out=$(echo "$@" | sed -n 's/.*--out \\([^ ]*\\).*/\\1/p')
  python3 - "$out/run.json" <<'PY'
import json, sys
p = sys.argv[1]; r = json.load(open(p)); r["dlq"] = 2; json.dump(r, open(p, "w"))
PY
fi
exit $status
''')
        wrapper.chmod(wrapper.stat().st_mode | stat.S_IEXEC)
        done = self.script(env={"ARMS": "fifo", "REPEATS": "1"}, binary=str(wrapper))
        self.assertEqual(done.returncode, 0, done.stderr + done.stdout)
        res = self.dir / "results/fifo/3"
        self.assertTrue((res / "rep-1.failed/run.json").exists())
        self.assertEqual(json.loads((res / "rep-1/run.json").read_text())["dlq"], 0)
        self.assertIn("keeping it as rep-1.failed", (self.dir / "results/progress.log").read_text())

    def test_shards_split_the_matrix_without_overlap(self):
        env = {"ARMS": "fifo oracle predictive_warm", "REPEATS": "1"}
        for i in (0, 1):
            done = self.script(env={**env, "SHARD": f"{i}/2"})
            self.assertEqual(done.returncode, 0, done.stderr + done.stdout)
        ran = sorted(p.parent.parent.parent.name for p in (self.dir / "results").glob("*/3/rep-1/run.json"))
        self.assertEqual(ran, ["fifo", "oracle", "predictive_warm"])
        self.assertEqual(self.script(env={**env, "SHARD": "2/2"}).returncode, 2)

    def test_registered_mode_refuses_overrides_unfrozen_config_and_wrong_trace(self):
        self.assertEqual(self.script("registered", {"ROWS": "10"}).returncode, 2)
        done = self.script("registered")
        self.assertEqual(done.returncode, 2)
        self.assertIn("not frozen", done.stderr)

    def test_report_reads_what_the_runner_wrote(self):
        self.assertEqual(self.script(env={"ARMS": "fifo predictive_warm", "REPEATS": "1"}).returncode, 0)
        rc = {"version": "t", "workers": 1, "time_scale": 1, "windows": 6, "class_bounds_ms": [50, 500],
              "bootstrap": {"samples": 100, "seed": 1, "confidence": 0.95, "block_length": 2},
              "criteria": {"A_P1": {"arm": "predictive_warm", "baseline": "fifo", "metric": "mean_latency_ms", "max_relative_delta": -0.15}},
              "gates": {"A": ["A_P1"]}}
        (self.dir / "rc.json").write_text(json.dumps(rc))
        done = subprocess.run(["python3", "-m", "benchmarks.replay_report", str(self.dir / "rc.json"), str(self.dir / "results")],
                              cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("Gate A at load 3.00", done.stdout)


if __name__ == "__main__":
    unittest.main()


@unittest.skipUnless(BINARY, "set HARBINGER_REPLAY to the harbinger_replay binary")
class SmokeKit(unittest.TestCase):
    def test_generated_smoke_runs_end_to_end_with_every_arm(self):
        from benchmarks.make_replay_smoke import write
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            write(d, 1500, 7)
            done = subprocess.run(["bash", str(ROOT / "benchmarks/run_replay.sh"), "debug", str(d / "smoke-config.json"),
                                   str(d / "results"), str(d / "prep"), str(d / "smoke-trace.csv")], cwd=ROOT,
                                  env={**os.environ, "REPLAY_BIN": BINARY}, capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr + done.stdout)
            runs = sorted(p.relative_to(d / "results").parts[0] for p in (d / "results").glob("*/0.5/rep-1/run.json"))
            self.assertEqual(len(runs), 7)
            self.assertTrue((d / "results/_probe/fifo/0.1/rep-1/run.json").exists())
            rep = subprocess.run(["python3", "-m", "benchmarks.replay_report", str(d / "smoke-config.json"), str(d / "results")],
                                 cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(rep.returncode, 0, rep.stderr)
