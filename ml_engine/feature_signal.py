"""Does any ingress feature predict duration better than the key? (#24, frozen as feature-signal-v1)

Usage: python -m ml_engine.feature_signal CONFIG RUN.csv [RUN.csv ...] [--out report.json]

Each run is split in time order (first 60% train, last 40% test, no look-ahead). Models:
  global_median           one median for everything
  per_key_median          the broker's model: one statistic per key (service/job_type)
  per_key_size_bin_median per key and floor(log2(primary size feature)); sparse bins fall back to the key median
  gradient_boosting       sklearn HistGradientBoostingRegressor on log1p(duration), key + log1p(numeric features)
The decision rule is read from CONFIG and applied across runs. Gradient boosting needs scikit-learn
(`ml_engine/requirements-analysis.txt`); without it that model is reported as unavailable.
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

NUMERIC = ("payload_bytes", "width", "height", "pixels", "rows")
MIN_BIN = 20


def load(path: Path) -> list[dict]:
    with path.open() as stream:
        rows = list(csv.DictReader(stream))
    for r in rows:
        r["key"] = r["service"] + "/" + r["job_type"]
        r["duration_ms"] = float(r["duration_ms"])
        r["submit_unix"] = float(r["submit_unix"])
        for f in NUMERIC:
            r[f] = float(r[f]) if r.get(f) not in (None, "") else None
    rows.sort(key=lambda r: (r["submit_unix"], int(r["seq"])))
    return rows


def primary_size(row: dict) -> float | None:
    for f in ("pixels", "rows", "payload_bytes"):
        if row.get(f):
            return row[f]
    return None


def size_bin(row: dict) -> int | None:
    size = primary_size(row)
    return int(math.floor(math.log2(size))) if size and size > 0 else None


def metrics(predicted: list[float], actual: list[float], boundaries: list[float]) -> dict:
    log_err = [abs(math.log2(max(p, 0.01) / max(a, 0.01))) for p, a in zip(predicted, actual)]
    tiers = sum(bisect.bisect_right(boundaries, p) == bisect.bisect_right(boundaries, a) for p, a in zip(predicted, actual))
    return {"median_abs_log2_error": statistics.median(log_err), "within_2x": sum(e <= 1 for e in log_err) / len(log_err),
            "tier_accuracy": tiers / len(actual),
            "mean_abs_error_ms": statistics.mean(abs(p - a) for p, a in zip(predicted, actual))}


def evaluate_run(rows: list[dict], train_fraction: float, gbm: dict) -> dict:
    cut = int(len(rows) * train_fraction)
    train, test = rows[:cut], rows[cut:]
    durations = sorted(r["duration_ms"] for r in train)
    boundaries = [durations[len(durations) // 3], durations[2 * len(durations) // 3]]
    global_median = statistics.median(durations)
    by_key, by_bin = defaultdict(list), defaultdict(list)
    for r in train:
        by_key[r["key"]].append(r["duration_ms"])
        by_bin[(r["key"], size_bin(r))].append(r["duration_ms"])
    key_median = {k: statistics.median(v) for k, v in by_key.items()}
    bin_median = {k: statistics.median(v) for k, v in by_bin.items() if len(v) >= MIN_BIN}
    actual = [r["duration_ms"] for r in test]

    def per_key(r):
        return key_median.get(r["key"], global_median)

    def per_bin(r):
        return bin_median.get((r["key"], size_bin(r)), per_key(r))

    results = {
        "global_median": metrics([global_median] * len(test), actual, boundaries),
        "per_key_median": metrics([per_key(r) for r in test], actual, boundaries),
        "per_key_size_bin_median": metrics([per_bin(r) for r in test], actual, boundaries),
    }
    try:
        import numpy as np
        from sklearn.ensemble import HistGradientBoostingRegressor
    except ImportError:
        results["gradient_boosting"] = None
    else:
        keys = sorted(by_key)
        def encode(r):
            # Missing numeric features are NaN, which the estimator handles natively.
            return [keys.index(r["key"]) if r["key"] in by_key else -1] + \
                   [math.log1p(r[f]) if r[f] is not None else float("nan") for f in NUMERIC]
        model = HistGradientBoostingRegressor(max_iter=gbm["max_iter"], random_state=gbm["random_state"],
                                              categorical_features=[0])
        model.fit(np.array([encode(r) for r in train]), np.log1p([r["duration_ms"] for r in train]))
        predicted = np.expm1(model.predict(np.array([encode(r) for r in test]))).tolist()
        results["gradient_boosting"] = metrics(predicted, actual, boundaries)
    per_class = {}
    for key in sorted(by_key):
        idx = [i for i, r in enumerate(test) if r["key"] == key]
        if len(idx) < 20:
            continue
        a = [actual[i] for i in idx]
        per_class[key] = {
            "test_rows": len(idx),
            "key_spread_p99_over_p50": sorted(by_key[key])[int(0.99 * (len(by_key[key]) - 1))] / max(key_median[key], 0.01),
            "per_key_median": metrics([per_key(test[i]) for i in idx], a, boundaries)["median_abs_log2_error"],
            "per_key_size_bin_median": metrics([per_bin(test[i]) for i in idx], a, boundaries)["median_abs_log2_error"],
        }
    return {"train_rows": len(train), "test_rows": len(test), "boundaries_ms": boundaries,
            "models": results, "per_key": per_class}


def decide(config: dict, runs: dict) -> dict:
    needed = math.ceil(len(runs) * 5 / 6)  # "at least 5 of the 6 runs", never fewer than one
    bin_wins = gbm_wins = 0
    for r in runs.values():
        m = r["models"]
        if (m["per_key_median"]["median_abs_log2_error"] - m["per_key_size_bin_median"]["median_abs_log2_error"] >= 0.25 and
                m["per_key_size_bin_median"]["tier_accuracy"] - m["per_key_median"]["tier_accuracy"] >= 0.05):
            bin_wins += 1
        if m["gradient_boosting"] and (m["per_key_size_bin_median"]["median_abs_log2_error"] -
                                       m["gradient_boosting"]["median_abs_log2_error"] >= 0.25):
            gbm_wins += 1
    binned = bin_wins >= needed
    richer = gbm_wins >= needed
    verdict = ("richer model worthwhile" if richer else
               "binned size feature worthwhile" if binned else "per-key statistics sufficient")
    return {"runs": len(runs), "required_wins": needed, "binned_feature_wins": bin_wins,
            "gradient_boosting_wins": gbm_wins, "verdict": verdict}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("config", type=Path)
    parser.add_argument("runs", type=Path, nargs="+")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    runs = {}
    for path in args.runs:
        rows = load(path)
        runs[rows[0]["run"]] = evaluate_run(rows, config["split"]["train_fraction"], config["gradient_boosting"])
    result = {"config": config["version"], "runs": runs, "decision": decide(config, runs)}
    text = json.dumps(result, indent=1, sort_keys=True)
    if args.out:
        args.out.write_text(text + "\n")
    print(json.dumps(result["decision"], indent=1))
    for name, r in runs.items():
        print(name, {m: (round(v["median_abs_log2_error"], 3), round(v["tier_accuracy"], 3)) if v else None
                     for m, v in r["models"].items()})


if __name__ == "__main__":
    main()
