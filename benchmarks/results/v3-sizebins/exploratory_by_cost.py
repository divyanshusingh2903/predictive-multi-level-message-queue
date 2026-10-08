"""Exploratory (not pre-registered): latency of the size-driven classes by planned-cost third, predictive vs
predictive_sized. Thirds are computed per seed from the predictive arm's messages (both arms see identical traffic).

Usage: python benchmarks/results/v3-sizebins/exploratory_by_cost.py RAW_DIR
"""
import collections
import csv
import statistics
import sys
from pathlib import Path

raw = Path(sys.argv[1])
seeds = sorted(int(p.name.split("-")[1]) for p in raw.glob("seed-*"))


def rows(seed: int, arm: str) -> list[dict]:
    with (raw / f"seed-{seed}" / arm / "messages.tsv").open() as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


for job in ("generate_invoice", "resize_image"):
    print(job)
    means = collections.defaultdict(list)
    for seed in seeds:
        planned = sorted(float(r["planned_ms"]) for r in rows(seed, "predictive") if r["job_type"] == job)
        cuts = [planned[len(planned) // 3], planned[2 * len(planned) // 3]]
        for arm in ("predictive", "predictive_sized"):
            selected = [r for r in rows(seed, arm) if r["job_type"] == job]
            for name, low, high in (("cheapest third", -1, cuts[0]), ("middle third", cuts[0], cuts[1]),
                                    ("costliest third", cuts[1], float("inf"))):
                latency = [(int(r["end_ns"]) - int(r["submit_ns"])) / 1e6 for r in selected
                           if low < float(r["planned_ms"]) <= high]
                means[(name, arm)].append(statistics.mean(latency))
    for name in ("cheapest third", "middle third", "costliest third"):
        a = statistics.median(means[(name, "predictive")])
        b = statistics.median(means[(name, "predictive_sized")])
        print(f"  {name:15s}: predictive {a:8.0f} ms  predictive_sized {b:8.0f} ms  ({b / a - 1:+.0%})")
