"""Common invocation-trace format, harbinger-trace-v1 (#47).

Every public trace is converted to this one format, which `harbinger_trace_sim` and `analysis.trace_analysis` read.
The Azure Functions 2021 file is still read directly as well, so the published `v2-azure-sim` and `v3-aging` results
reproduce exactly.

Format: UTF-8 CSV, with the header `arrival_ms,key,duration_ms` or `arrival_ms,key,duration_ms,size`.

- `arrival_ms`: when the request arrived, in milliseconds from any origin. Rows are sorted by arrival; ties keep file
  order.
- `key`: what the predictor keys on (function, user + executable, ...). Non-empty, with no comma, quote or line break.
- `duration_ms`: service time, the handler's own run time without queueing. Finite and >= 0.
- `size`: optional size hint known at submission (request bytes, requested run time, ...). Finite and >= 0, or empty
  when unknown.

Usage:
  python -m analysis.trace_format convert azure2021 AZURE.txt OUT.csv
  python -m analysis.trace_format convert huawei2025 R5/ OUT.csv     (directory of request-table part-*.csv files)
  python -m analysis.trace_format check TRACE.csv
"""
from __future__ import annotations

import argparse
import csv
import glob
import hashlib
import json
import math
from pathlib import Path
from typing import Iterable, Iterator, NamedTuple

HEADER = ["arrival_ms", "key", "duration_ms"]
HEADER_WITH_SIZE = HEADER + ["size"]
_FORBIDDEN = set(',"\r\n')


class Row(NamedTuple):
    arrival_ms: float
    key: str
    duration_ms: float
    size: float | None = None


def _check(row: Row, previous: float, line: int) -> None:
    if not math.isfinite(row.arrival_ms):
        raise ValueError(f"line {line}: arrival_ms is not finite")
    if row.arrival_ms < previous:
        raise ValueError(f"line {line}: rows are not sorted by arrival_ms")
    if not row.key or _FORBIDDEN & set(row.key):
        raise ValueError(f"line {line}: key must be non-empty without comma, quote or line break")
    if not math.isfinite(row.duration_ms) or row.duration_ms < 0:
        raise ValueError(f"line {line}: duration_ms must be finite and >= 0")
    if row.size is not None and (not math.isfinite(row.size) or row.size < 0):
        raise ValueError(f"line {line}: size must be finite and >= 0")


def write(path: Path, rows: Iterable[Row], with_size: bool = False) -> int:
    """Write validated rows (already sorted by arrival); returns the row count. Floats round-trip exactly."""
    count, previous = 0, -math.inf
    with path.open("w", newline="") as stream:
        stream.write(",".join(HEADER_WITH_SIZE if with_size else HEADER) + "\n")
        for row in rows:
            count += 1
            _check(row, previous, count + 1)
            previous = row.arrival_ms
            fields = [repr(float(row.arrival_ms)), row.key, repr(float(row.duration_ms))]
            if with_size:
                fields.append("" if row.size is None else repr(float(row.size)))
            elif row.size is not None:
                raise ValueError("row has a size but the file was opened without a size column")
            stream.write(",".join(fields) + "\n")
    return count


def read(path: Path) -> Iterator[Row]:
    """Yield validated rows; raises on any malformed or unsorted row."""
    with path.open(newline="") as stream:
        reader = csv.reader(stream)
        header = next(reader, None)
        if header not in (HEADER, HEADER_WITH_SIZE):
            raise ValueError(f"not a harbinger-trace-v1 file: header {header}")
        sized, previous = len(header) == 4, -math.inf
        for line, fields in enumerate(reader, start=2):
            if len(fields) != len(header):
                raise ValueError(f"line {line}: expected {len(header)} fields")
            size = float(fields[3]) if sized and fields[3] != "" else None
            row = Row(float(fields[0]), fields[1], float(fields[2]), size)
            _check(row, previous, line)
            previous = row.arrival_ms
            yield row


def from_azure2021(path: Path) -> list[Row]:
    """Azure Functions 2021 (app,func,end_timestamp,duration in seconds). Arrival is reconstructed as end - duration,
    with the same arithmetic as harbinger_trace_sim's own Azure reader, and the key is app/func."""
    rows = []
    with path.open() as stream:
        reader = csv.reader(stream)
        if next(reader) != ["app", "func", "end_timestamp", "duration"]:
            raise ValueError("unexpected Azure 2021 header")
        for app, func, end, duration in reader:
            seconds = float(duration)
            rows.append(Row((float(end) - seconds) * 1000.0, f"{app}/{func}", seconds * 1000.0))
    rows.sort(key=lambda row: row.arrival_ms)  # stable: ties keep file order
    return rows


def from_huawei2025(directory: Path) -> tuple[list[Row], dict]:
    """Huawei Public Cloud 2025 request tables (one region's part-*.csv files, read in name order).

    - Timestamps are completion times. Checked on Region 5: if they were start times, consecutive requests on a pod
      would overlap 11% of the time; read as end times, 1%.
    - Arrival at the frontend is therefore `time_frontend - totalCost_frontend`, in seconds.
    - Duration is `totalCost_worker`, the time the request spent in the function's pod. `runtimeCost` (time in the
      runtime alone) would be closer, but it is 0 on 40,953 Region 5 rows that ran for about a second, a logging gap.
      Where both are set, the worker's overhead is about 0.1 ms (median 0.4% of the duration).
    - Key is the publishers' funcID, `funcName---userID---poolName`, where poolName is podID without its pod number.
    - Size is `requestBodySize` in bytes.
    - Rows missing any of these fields are dropped and counted.
    """
    rows, dropped, total = [], 0, 0
    for part in sorted(glob.glob(str(directory / "part-*.csv"))):
        with open(part, newline="") as stream:
            for record in csv.DictReader(stream):
                total += 1
                fields = [record[k] for k in ("time_frontend", "totalCost_frontend", "totalCost_worker", "funcName",
                                              "userID", "podID", "requestBodySize")]
                if "" in fields:
                    dropped += 1
                    continue
                end, latency, duration, func, user, pod, size = fields
                pool = pod.rsplit("-", 1)[0]
                rows.append(Row((float(end) - float(latency)) * 1000.0, f"{func}---{user}---{pool}",
                                float(duration) * 1000.0, float(size)))
    if not total:
        raise ValueError(f"no part-*.csv rows under {directory}")
    rows.sort(key=lambda row: row.arrival_ms)  # stable: ties keep file order
    return rows, {"source_rows": total, "dropped_incomplete": dropped}


CONVERTERS = {
    "azure2021": lambda path: (from_azure2021(path), {}),
    "huawei2025": from_huawei2025,
}


def summary(path: Path) -> dict:
    rows = list(read(path))
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    sized = [row.size for row in rows if row.size is not None]
    return {
        "format": "harbinger-trace-v1", "sha256": digest, "rows": len(rows), "keys": len({row.key for row in rows}),
        "span_ms": rows[-1].arrival_ms - rows[0].arrival_ms if rows else 0.0,
        "work_ms": sum(row.duration_ms for row in rows), "rows_with_size": len(sized),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    convert = commands.add_parser("convert")
    convert.add_argument("source", choices=sorted(CONVERTERS))
    convert.add_argument("input", type=Path)
    convert.add_argument("output", type=Path)
    check = commands.add_parser("check")
    check.add_argument("trace", type=Path)
    args = parser.parse_args()
    if args.command == "convert":
        if args.output.exists():
            raise SystemExit(f"refusing to overwrite {args.output}")
        rows, notes = CONVERTERS[args.source](args.input)
        write(args.output, rows, with_size=any(row.size is not None for row in rows))
        print(json.dumps({**summary(args.output), "source": args.source, **notes}, indent=1))
    else:
        print(json.dumps(summary(args.trace), indent=1))


if __name__ == "__main__":
    main()
