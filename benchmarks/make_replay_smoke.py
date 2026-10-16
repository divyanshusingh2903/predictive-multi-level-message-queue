"""Write a synthetic trace and a replay config for the AWS smoke run (docs/v4-replay-preregistration.md, item 4).

Usage: python -m benchmarks.make_replay_smoke OUT_DIR [--rows N] [--seed S]

The pre-registration allows a smoke run only on a synthetic trace, read only for whether the pipeline runs, never for what
the arms did. This writes OUT_DIR/smoke-trace.csv (history rows, then evaluation rows, at about 500 messages/s at the
smoke load, with 200 keys of four duration classes) and OUT_DIR/smoke-config.json, which has every arm of the Huawei
config, 64 workers, an overhead probe, and inline criteria so that replay_report.py runs on its output.
"""
from __future__ import annotations

import argparse
import json
import random
from pathlib import Path

CLASSES_MS = (5.0, 20.0, 64.0, 250.0)


def write(out: Path, rows: int, seed: int) -> None:
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(seed)
    key_class = {f"smoke-{k}": CLASSES_MS[k % len(CLASSES_MS)] for k in range(200)}
    keys = list(key_class)
    with (out / "smoke-trace.csv").open("w") as f:
        f.write("arrival_ms,key,duration_ms\n")
        t = 0.0
        for _ in range(2 * rows):  # the first half is history, the second the evaluation half (the split is by time)
            t += rng.expovariate(1 / 2.0)
            key = rng.choice(keys)
            f.write(f"{t:.3f},{key},{key_class[key] * rng.uniform(0.8, 1.25):.3f}\n")
    arms = ["fifo", "predictive_warm", "predictive_warm+weights+pausing", "oracle", "oracle+weights+pausing",
            "static_history", "predictive_cold"]
    rule = lambda arm, metric, **kw: {"arm": arm, "baseline": "fifo", "metric": metric, **kw}
    config = {
        "version": "v4-replay-smoke", "frozen_on": None, "time_scale": 1, "history_split": "time", "history_fraction": 0.5,
        "class_bounds_ms": [10, 100], "workers": 64, "levels": 3, "default_priority": 1,
        "aging": {"threshold_ms": 5000, "interval_ms": 500}, "predictor": {"stale_after_ms": 1800000, "time_half_life_ms": 600000},
        "level_weights": [8, 3, 1], "utilizations": [0.5], "arms": arms, "repeats": 1, "producer_threads": 8, "drain_s": 600,
        "probe": {"load": 0.1, "seconds": 20}, "windows": 28,
        "bootstrap": {"samples": 1000, "seed": 2026, "confidence": 0.95, "block_length": 4},
        "criteria": {"A_P1": rule("predictive_warm", "mean_latency_ms", max_relative_delta=-0.15),
                     "A_G1": rule("predictive_warm", "p99_latency_ms", max_ratio=2.0)},
        "gates": {"A": ["A_P1", "A_G1"]},
    }
    (out / "smoke-config.json").write_text(json.dumps(config, indent=1) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("out", type=Path)
    parser.add_argument("--rows", type=int, default=20000, help="evaluation rows (and as many history rows)")
    parser.add_argument("--seed", type=int, default=7)
    args = parser.parse_args()
    write(args.out, args.rows, args.seed)
    print(f"wrote {args.out}/smoke-trace.csv and smoke-config.json")


if __name__ == "__main__":
    main()
