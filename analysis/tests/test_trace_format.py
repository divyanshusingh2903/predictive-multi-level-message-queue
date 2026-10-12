import json
import os
from pathlib import Path
import random
import re
import subprocess
import tempfile
import unittest

from analysis.trace_format import Row, from_azure2021, from_huawei2025, read, summary, write

TRACE_SIM = os.environ.get("HARBINGER_TRACE_SIM")
SIM_CONFIG = {
    "version": "test", "history_fraction": 0.5, "evaluation_days": 2, "workers": 2, "levels": 3,
    "default_priority": 1, "aging": {"threshold_ms": 50, "interval_ms": 10}, "utilizations": [0.8],
    "level_weights": [8, 3, 1], "misconfigured_seed": 1,
    "arms": ["fifo", "oracle", "static_history", "predictive", "predictive_warm+weights+pausing"],
}


def azure_fixture(path: Path, rows: int = 600, seed: int = 7) -> None:
    """A small Azure-2021-shaped file in end-time order with no two equal arrivals (so sort order is unambiguous)."""
    rng = random.Random(seed)
    records, clock = [], 1000.0
    for i in range(rows):
        clock += rng.uniform(0.05, 0.5)
        app = f"app{i % 4}"
        duration = round(rng.lognormvariate(-3 + (i % 4), 0.5), 3)
        records.append((clock + duration, app, f"f{i % 7}", duration))
    records.sort()
    lines = ["app,func,end_timestamp,duration"] + [f"{a},{f},{end!r},{d!r}" for end, a, f, d in records]
    path.write_text("\n".join(lines) + "\n")


class TraceFormat(unittest.TestCase):
    def test_round_trip_is_exact(self):
        rows = [Row(0.1, "a", 1 / 3), Row(0.1, "b", 2.0, 4096.0), Row(5.25, "a", 0.0, None)]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "t.csv"
            self.assertEqual(write(path, rows, with_size=True), 3)
            self.assertEqual(list(read(path)), rows)
            info = summary(path)
            self.assertEqual((info["rows"], info["keys"], info["rows_with_size"]), (3, 2, 1))

    def test_invalid_rows_are_rejected(self):
        bad = [
            [Row(2.0, "a", 1.0), Row(1.0, "a", 1.0)],  # unsorted
            [Row(0.0, "a,b", 1.0)],                    # comma in key
            [Row(0.0, "", 1.0)],                       # empty key
            [Row(0.0, "a", -1.0)],                     # negative duration
            [Row(0.0, "a", float("nan"))],             # not finite
        ]
        with tempfile.TemporaryDirectory() as tmp:
            for rows in bad:
                with self.subTest(rows=rows), self.assertRaises(ValueError):
                    write(Path(tmp) / "t.csv", rows)
            with self.assertRaises(ValueError):  # a size without a size column
                write(Path(tmp) / "t.csv", [Row(0.0, "a", 1.0, 5.0)])

    def test_reader_rejects_other_headers_and_unsorted_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "t.csv"
            path.write_text("time,key,duration\n0,a,1\n")
            with self.assertRaises(ValueError):
                list(read(path))
            path.write_text("arrival_ms,key,duration_ms\n2,a,1\n1,a,1\n")
            with self.assertRaises(ValueError):
                list(read(path))

    def test_azure_conversion(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "azure.txt"
            source.write_text("app,func,end_timestamp,duration\nA,f,10.5,0.5\nB,g,10.2,0.1\n")
            rows = from_azure2021(source)
            self.assertEqual([row.key for row in rows], ["A/f", "B/g"])
            self.assertEqual(rows[0], Row((10.5 - 0.5) * 1000.0, "A/f", 500.0))

    def test_huawei_conversion_uses_completion_times_and_worker_cost(self):
        header = ("time_worker,time_frontend,requestID,clusterName,funcName,podID,userID,totalCost_worker,"
                  "workerCost,runtimeCost,totalCost_frontend,frontendCost,busCost,readBodyCost,writeRspCost,"
                  "cpu_usage,memory_usage,requestBodySize\n")
        part0 = header + ("100.5,100.5,r1,3,411,pool22-300-128-0000177166,301,0.4,0.1,0.3,2.5,0,0,0,0,0,0,1024\n"
                          "100.0,,r2,3,411,pool22-300-128-0000177166,301,0.1,0,0.1,,0,,0,0,0,0,\n")
        part1 = header + "99.0,99.001,r3,3,34,pool9-1-2-0000000001,83,1.0E-3,0,9.0E-4,0.001,0,0,0,0,0,0,0\n"
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "part-00000-x.csv").write_text(part0)
            (directory / "part-00001-x.csv").write_text(part1)
            rows, notes = from_huawei2025(directory)
        self.assertEqual(notes, {"source_rows": 3, "dropped_incomplete": 1})
        self.assertEqual(rows[0], Row((100.5 - 2.5) * 1000.0, "411---301---pool22-300-128", 0.4 * 1000.0, 1024.0))
        self.assertEqual(rows[1], Row((99.001 - 0.001) * 1000.0, "34---83---pool9-1-2", 1.0e-3 * 1000.0, 0.0))

@unittest.skipUnless(TRACE_SIM, "HARBINGER_TRACE_SIM not set (build the harbinger_trace_sim target)")
class SimulatorReadsBothFormats(unittest.TestCase):
    def test_converted_azure_trace_simulates_identically(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            azure, common, config_path = tmp / "azure.txt", tmp / "common.csv", tmp / "config.json"
            azure_fixture(azure)
            write(common, from_azure2021(azure))
            config_path.write_text(json.dumps(SIM_CONFIG))
            outputs = []
            for trace in (azure, common):
                out = tmp / f"{trace.stem}.json"
                subprocess.run([TRACE_SIM, str(trace), str(config_path), str(out)], check=True, capture_output=True)
                outputs.append(re.sub(r'"sim_seconds":[^,]+,', "", out.read_text()))
            self.assertEqual(outputs[0], outputs[1])
            self.assertIn("predictive_warm+weights+pausing", json.loads(outputs[1])["results"]["0.8"])

    def test_unsorted_or_malformed_common_trace_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            config = tmp / "config.json"
            config.write_text(json.dumps(SIM_CONFIG))
            good = "".join(f"{i},k{i % 3},{1 + i % 5}\n" for i in range(200))
            run = lambda: subprocess.run([TRACE_SIM, str(trace), str(config), str(tmp / "o.json")], capture_output=True)
            trace = tmp / "t.csv"
            trace.write_text("arrival_ms,key,duration_ms\n" + good)
            self.assertEqual(run().returncode, 0)  # the valid baseline runs, so failures below come from the trace
            # After rows at 0..199: unsorted, missing field, negative duration, quoted key, extra field.
            for bad in ("150,a,1\n", "300,a\n", "300,a,-1\n", '300,"a",1\n', "300,a,1,7\n"):
                trace.write_text("arrival_ms,key,duration_ms\n" + good + bad)
                with self.subTest(bad=bad):
                    result = run()
                    self.assertEqual(result.returncode, 1)
                    self.assertIn(b"line ", result.stderr)


@unittest.skipUnless(TRACE_SIM, "HARBINGER_TRACE_SIM not set (build the harbinger_trace_sim target)")
class SimulatorV3TraceRules(unittest.TestCase):
    """history_split, window_split and class_bounds_ms (v3-traces, #46)."""

    def simulate(self, rows, **rules):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            trace, config, out = tmp / "t.csv", tmp / "c.json", tmp / "o.json"
            write(trace, rows)
            config.write_text(json.dumps({**SIM_CONFIG, "arms": ["fifo", "oracle"], **rules}))
            subprocess.run([TRACE_SIM, str(trace), str(config), str(out)], check=True, capture_output=True)
            return json.loads(out.read_text())

    def test_time_split_and_time_windows(self):
        # Dense first: 300 rows in the first 300 ms, then 300 rows spread over 300..9300 ms.
        rows = [Row(float(i), f"k{i % 3}", 1.0 + i % 50) for i in range(300)]
        rows += [Row(300.0 + 30.0 * i, f"k{i % 3}", 1.0 + i % 50) for i in range(300)]
        by_rows = self.simulate(rows)
        self.assertEqual(by_rows["history_rows"], 300)
        self.assertNotIn("window_rows", by_rows)  # old configs keep the old output
        self.assertNotIn("long_count", by_rows["results"]["0.8"]["fifo"]["windows"][0])
        by_time = self.simulate(rows, history_split="time", window_split="time", class_bounds_ms=[10, 40])
        cut = 4650.0  # half of the 0..9300 ms span
        self.assertEqual(by_time["history_rows"], sum(row.arrival_ms < cut for row in rows))
        evaluation = [row for row in rows if row.arrival_ms >= cut]
        middle = evaluation[0].arrival_ms + (evaluation[-1].arrival_ms - evaluation[0].arrival_ms) / 2
        first = sum(row.arrival_ms < middle for row in evaluation)
        self.assertEqual(by_time["window_rows"], [first, len(evaluation) - first])
        self.assertEqual(by_time["class_bounds_ms"], [10, 40])
        fifo = by_time["results"]["0.8"]["fifo"]["windows"]
        self.assertEqual([w["count"] for w in fifo], by_time["window_rows"])
        for w in fifo:  # class counts are reported and add up
            self.assertEqual(w["short_count"] + w["medium_count"] + w["long_count"], w["count"])

    def test_invalid_rule_is_rejected(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.simulate([Row(float(i), "k", 1.0) for i in range(200)], window_split="days")

if __name__ == "__main__":
    unittest.main()
