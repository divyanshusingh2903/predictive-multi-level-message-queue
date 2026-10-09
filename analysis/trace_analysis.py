"""Per-key predictability on real traces (issue #24).

Input: rows of (timestamp_s, key, duration_ms[, numeric features]) in time order. Reports, with a time-ordered
split (no look-ahead):
- within-key spread: p50, p90, p99 and p99/p50 per key, weighted by volume;
- held-out error of a per-key median (learned on the earlier part) versus a global median;
- how much of the traffic the broker's predictor would actually route (N_min and the p99/p50 spread gate).

Usage:
  python -m analysis.trace_analysis azure2021 TRACE.txt [--split 0.5] [--out report.json]
  python -m analysis.trace_analysis csv FILE.csv --time COL --key COL[,COL] --duration COL [--duration-unit ms|s]
"""
from __future__ import annotations

import argparse
import bisect
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path


def azure2021(path: Path):
    """Azure Functions 2021: app,func,end_timestamp(s),duration(s); arrival is reconstructed as end - duration."""
    with path.open() as stream:
        reader = csv.reader(stream)
        if next(reader) != ["app", "func", "end_timestamp", "duration"]:
            raise ValueError("unexpected Azure 2021 header")
        for app, func, end, duration in reader:
            seconds = float(duration)
            yield float(end) - seconds, app[:12] + "/" + func[:12], seconds * 1000.0


def generic(path: Path, time: str, keys: list[str], duration: str, unit: str):
    scale = 1000.0 if unit == "s" else 1.0
    with path.open() as stream:
        for row in csv.DictReader(stream):
            yield float(row[time]), "/".join(row[k] for k in keys), float(row[duration]) * scale


def quantile(sorted_values: list[float], q: float) -> float:
    return sorted_values[min(len(sorted_values) - 1, max(0, math.ceil(len(sorted_values) * q) - 1))]


def analyse(rows, split: float = 0.5, min_samples: int = 20, max_spread: float = 16.0, levels: int = 3) -> dict:
    rows = sorted(rows)
    if len(rows) < 100:
        raise ValueError("need at least 100 rows")
    cut = int(len(rows) * split)
    train, test = rows[:cut], rows[cut:]
    by_key: dict[str, list[float]] = defaultdict(list)
    for _, key, duration in train:
        by_key[key].append(duration)
    for values in by_key.values():
        values.sort()
    global_sorted = sorted(d for _, _, d in train)
    global_median = quantile(global_sorted, 0.5)
    boundaries = [quantile(global_sorted, (i + 1) / levels) for i in range(levels - 1)]

    def key_stats(values):
        p50 = quantile(values, 0.5)
        return {"n": len(values), "p50": p50, "p90": quantile(values, 0.9), "p99": quantile(values, 0.99),
                "spread": quantile(values, 0.99) / max(p50, 1.0)}

    stats = {k: key_stats(v) for k, v in by_key.items()}
    routable = {k for k, s in stats.items() if s["n"] >= min_samples and s["spread"] <= max_spread}

    def errors(predict):
        log_errors, abs_errors, within2, tiers = [], [], 0, 0
        for _, key, actual in test:
            predicted = predict(key)
            log_errors.append(abs(math.log2(max(predicted, 1.0) / max(actual, 1.0))))
            abs_errors.append(abs(predicted - actual))
            within2 += log_errors[-1] <= 1.0
            tiers += bisect.bisect_right(boundaries, predicted) == bisect.bisect_right(boundaries, actual)
        n = len(test)
        return {"median_abs_log2_error": statistics.median(log_errors), "mean_abs_error_ms": statistics.mean(abs_errors),
                "within_2x": within2 / n, "tier_accuracy": tiers / n}

    per_key = errors(lambda k: stats[k]["p50"] if k in routable else global_median)
    per_key_all = errors(lambda k: stats[k]["p50"] if k in stats else global_median)
    global_only = errors(lambda k: global_median)
    test_routed = sum(1 for _, k, _ in test if k in routable)
    weighted_spread = sorted((s["spread"], s["n"]) for s in stats.values())
    total = sum(n for _, n in weighted_spread)

    def weighted_quantile(q):
        running = 0
        for value, n in weighted_spread:
            running += n
            if running >= q * total:
                return value
        return weighted_spread[-1][0]

    durations = sorted(d for _, _, d in rows)
    return {
        "rows": len(rows), "train_rows": len(train), "test_rows": len(test), "keys_in_train": len(stats),
        "duration_ms": {"p50": quantile(durations, 0.5), "p90": quantile(durations, 0.9), "p99": quantile(durations, 0.99),
                        "max": durations[-1]},
        "within_key_spread_p99_over_p50_volume_weighted": {"p25": weighted_quantile(0.25), "p50": weighted_quantile(0.5),
                                                          "p75": weighted_quantile(0.75), "p90": weighted_quantile(0.9)},
        "keys_routable": len(routable), "test_share_routable": test_routed / len(test),
        "test_share_unseen_key": sum(1 for _, k, _ in test if k not in stats) / len(test),
        "tier_boundaries_ms": boundaries,
        "held_out": {"per_key_median_with_gates": per_key, "per_key_median_all_keys": per_key_all,
                     "global_median": global_only},
        "settings": {"split": split, "min_samples": min_samples, "max_spread": max_spread, "levels": levels},
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("format", choices=["azure2021", "csv"])
    parser.add_argument("path", type=Path)
    parser.add_argument("--time")
    parser.add_argument("--key")
    parser.add_argument("--duration")
    parser.add_argument("--duration-unit", choices=["ms", "s"], default="ms")
    parser.add_argument("--split", type=float, default=0.5)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    if args.format == "azure2021":
        rows = list(azure2021(args.path))
    else:
        rows = list(generic(args.path, args.time, args.key.split(","), args.duration, args.duration_unit))
    result = analyse(rows, args.split)
    text = json.dumps(result, indent=1, sort_keys=True)
    if args.out:
        args.out.write_text(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
