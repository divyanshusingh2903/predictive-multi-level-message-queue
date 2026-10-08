"""Pre-registered analysis for the production-flow benchmark (v2-prodflow-1, v3-sizebins-1).

Usage: python -m benchmarks.prodflow_report CONFIG RESULTS_DIR [--out DIR]

RESULTS_DIR holds seed-<seed>/<arm>/{messages.tsv,run.json}. Every criterion is computed from paired per-seed
values exactly as frozen in CONFIG; nothing here is tuned after seeing predictive results.

A criterion's metric is any key of the per-run metrics, including the per-class keys "class_mean_ms:<job_type>" and
"shifted_class_mean_ms:<job_type>" (mean latency in the shifted phase) and, when CONFIG lists "sized_classes",
"unsized_mean_ms" (mean latency of every other job type).
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import random
import statistics
from pathlib import Path


def percentile(values: list[float], p: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * p / 100) - 1)]


def load_run(directory: Path) -> tuple[dict, list[dict]]:
    run = json.loads((directory / "run.json").read_text())
    with (directory / "messages.tsv").open() as stream:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    return run, rows


def metrics(run: dict, rows: list[dict], config: dict) -> dict:
    done = [r for r in rows if r["done"] == "1"]
    latency = {int(r["seq"]): (int(r["end_ns"]) - int(r["submit_ns"])) / 1e6 for r in done}
    wait = {int(r["seq"]): (int(r["first_start_ns"]) - int(r["submit_ns"])) / 1e6 for r in done}
    service = {int(r["seq"]): max(float(r["handler_ms"]), 1.0) for r in done}
    all_latency = list(latency.values())
    classes = sorted({r["job_type"] for r in rows})
    per_class = {}
    for name in classes:
        ids = [int(r["seq"]) for r in done if r["job_type"] == name]
        values = [latency[i] for i in ids]
        per_class[name] = {
            "count": len(ids), "p50_ms": percentile(values, 50), "p95_ms": percentile(values, 95),
            "p99_ms": percentile(values, 99), "mean_ms": statistics.mean(values) if values else None,
            "max_wait_ms": max((wait[i] for i in ids), default=None),
            "median_service_ms": statistics.median([service[i] for i in ids]) if ids else None,
        }
    short = [latency[int(r["seq"])] for r in done if r["job_type"] in config["short_classes"]]
    long_waits = [wait[int(r["seq"])] for r in done if r["job_type"] == config["long_class"]]
    slowdown = [latency[i] / service[i] for i in latency]
    first_submit = min(int(r["submit_ns"]) for r in rows)
    last_end = max(int(r["end_ns"]) for r in done)
    cohorts = {}
    for phase, name in enumerate(["steady", "ramp", "flash_sale", "recovery", "shifted"]):
        values = [latency[int(r["seq"])] for r in done if r["phase"] == str(phase)]
        cohorts[name] = {"count": len(values), "mean_ms": statistics.mean(values) if values else None,
                         "p50_ms": percentile(values, 50), "p99_ms": percentile(values, 99)}
    routing = run.get("broker", {}).get("routing", {})
    per_class_keys = {}
    for name in classes:
        values = [latency[int(r["seq"])] for r in done if r["job_type"] == name]
        shifted = [latency[int(r["seq"])] for r in done if r["job_type"] == name and r["phase"] == "4"]
        per_class_keys[f"class_mean_ms:{name}"] = statistics.mean(values) if values else None
        per_class_keys[f"shifted_class_mean_ms:{name}"] = statistics.mean(shifted) if shifted else None
    if "sized_classes" in config:
        unsized = [latency[int(r["seq"])] for r in done if r["job_type"] not in config["sized_classes"]]
        per_class_keys["unsized_mean_ms"] = statistics.mean(unsized) if unsized else None
    return {
        **per_class_keys,
        "messages": len(rows), "completed": len(done), "dlq": run["dlq"], "submit_errors": run["submit_errors"],
        "mean_latency_ms": statistics.mean(all_latency), "p50_latency_ms": percentile(all_latency, 50),
        "p95_latency_ms": percentile(all_latency, 95), "p99_latency_ms": percentile(all_latency, 99),
        "short_p50_ms": percentile(short, 50), "short_mean_ms": statistics.mean(short),
        "long_max_wait_ms": max(long_waits) if long_waits else None,
        "slowdown_p50": percentile(slowdown, 50), "slowdown_p95": percentile(slowdown, 95),
        "slowdown_p99": percentile(slowdown, 99),
        "throughput_per_s": len(done) / ((last_end - first_submit) / 1e9),
        "lookup_mean_us": routing.get("lookup_mean_us"),
        "routing": routing, "per_class": per_class, "cohorts": cohorts,
    }


def bootstrap(values: list[float], samples: int, seed: int, confidence: float) -> tuple[float, list[float]]:
    rng = random.Random(seed)
    means = sorted(statistics.mean(rng.choices(values, k=len(values))) for _ in range(samples))
    low = means[max(0, math.floor(samples * (1 - confidence) / 2))]
    high = means[min(samples - 1, math.ceil(samples * (1 + confidence) / 2) - 1)]
    return statistics.mean(values), [low, high]


def evaluate(config: dict, table: dict) -> dict:
    """table[arm][seed] -> metrics. Relative deltas and ratios are paired by seed."""
    b = config["bootstrap"]
    results = {}
    for name, rule in config["criteria"].items():
        arm = rule.get("arm", config["primary_arm"])
        if arm not in table:
            results[name] = {"status": "missing_arm"}
            continue
        if name.startswith("G3"):
            ok = all(m["completed"] == m["messages"] and m["dlq"] <= rule["max_dlq"] and
                     m["submit_errors"] <= rule["max_submit_errors"] for m in table[arm].values())
            results[name] = {"status": "pass" if ok else "fail"}
            continue
        baseline = rule["baseline"]
        seeds = sorted(set(table[arm]) & set(table.get(baseline, {})))
        if len(seeds) < 2:
            results[name] = {"status": "insufficient_seeds", "seeds": len(seeds)}
            continue
        metric = rule["metric"]
        pairs = [(table[arm][s][metric], table[baseline][s][metric]) for s in seeds]
        if any(a is None or c is None or c == 0 for a, c in pairs):
            results[name] = {"status": "unevaluable"}
            continue
        if "max_ratio" in rule or "min_ratio" in rule:
            point, ci = bootstrap([a / c for a, c in pairs], b["samples"], b["seed"], b["confidence"])
            if "max_ratio" in rule:
                ok = ci[1] <= rule["max_ratio"]
                if "absolute_max_ms" in rule:
                    ok = ok and all(a <= rule["absolute_max_ms"] for a, _ in pairs)
            else:
                ok = ci[0] >= rule["min_ratio"]
            results[name] = {"status": "pass" if ok else "fail", "kind": "ratio", "point": point, "ci95": ci,
                             "seeds": len(seeds)}
            continue
        point, ci = bootstrap([(a - c) / c for a, c in pairs], b["samples"], b["seed"], b["confidence"])
        if "max_abs_relative_delta" in rule:
            limit = rule["max_abs_relative_delta"]
            ok = -limit <= ci[0] and ci[1] <= limit
            if "max_lookup_mean_us" in rule:
                ok = ok and all((table[arm][s]["lookup_mean_us"] or 0) <= rule["max_lookup_mean_us"] for s in seeds)
        elif rule["max_relative_delta"] < 0:
            # Improvement claims: the point estimate meets the target and the interval excludes no change.
            ok = point <= rule["max_relative_delta"] and ci[1] < 0
        elif rule["max_relative_delta"] == 0:
            ok = ci[1] < 0
        else:
            # Non-inferiority: the whole interval stays under the margin.
            ok = ci[1] <= rule["max_relative_delta"]
        results[name] = {"status": "pass" if ok else "fail", "kind": "relative_delta", "point": point, "ci95": ci,
                         "seeds": len(seeds)}
    gate = all(results.get(name, {}).get("status") == "pass" for name in config["gate"])
    return {"criteria": results, "gate_passed": gate}


def fmt(value, digits=1) -> str:
    return "–" if value is None else f"{value:.{digits}f}"


def report(config: dict, table: dict, evaluation: dict) -> str:
    arms = [a for a in config["arms"] if a in table]
    median = lambda arm, key: statistics.median(m[key] for m in table[arm].values() if m[key] is not None)
    lines = [f"# Production-flow benchmark ({config['version']})", "",
             f"Seeds per arm: {', '.join(str(len(table[a])) for a in arms)} ({', '.join(arms)}). "
             "Values are medians across seeds; criteria use paired per-seed deltas with a bootstrap 95% interval.", "",
             "| Arm | Mean ms | P50 ms | P95 ms | P99 ms | Short-class P50 ms | Export max wait s | Slowdown P50 / P99 | Throughput/s |",
             "|---|---|---|---|---|---|---|---|---|"]
    for arm in arms:
        lines.append(f"| {arm} | {fmt(median(arm, 'mean_latency_ms'))} | {fmt(median(arm, 'p50_latency_ms'))} | "
                     f"{fmt(median(arm, 'p95_latency_ms'))} | {fmt(median(arm, 'p99_latency_ms'))} | "
                     f"{fmt(median(arm, 'short_p50_ms'))} | {fmt(median(arm, 'long_max_wait_ms') / 1000, 2)} | "
                     f"{fmt(median(arm, 'slowdown_p50'))} / {fmt(median(arm, 'slowdown_p99'))} | "
                     f"{fmt(median(arm, 'throughput_per_s'))} |")
    lines += ["", "## Pre-registered criteria", "", "| Criterion | Arm vs baseline | Point | 95% CI | Result |", "|---|---|---|---|---|"]
    for name, result in evaluation["criteria"].items():
        rule = config["criteria"][name]
        who = f"{rule.get('arm', config['primary_arm'])} vs {rule.get('baseline', '–')}"
        point = result.get("point")
        ci = result.get("ci95")
        suffix = "×" if result.get("kind") == "ratio" else ""
        shown = (f"{point:+.1%}" if result.get("kind") == "relative_delta" else f"{point:.2f}{suffix}") if point is not None else "–"
        interval = (f"[{ci[0]:+.1%}, {ci[1]:+.1%}]" if result.get("kind") == "relative_delta" else
                    f"[{ci[0]:.2f}, {ci[1]:.2f}]") if ci else "–"
        lines.append(f"| {name} | {who} | {shown} | {interval} | **{result['status']}** |")
    lines += ["", f"**Gate ({', '.join(config['gate'])}): {'PASSED' if evaluation['gate_passed'] else 'NOT PASSED'}.**", ""]
    classes = sorted(next(iter(table[arms[0]].values()))["per_class"])
    lines += ["## Per-class P50 / P99 latency (ms, median across seeds)", "",
              "| Class | " + " | ".join(arms) + " |", "|---|" + "---|" * len(arms)]
    for name in classes:
        cells = []
        for arm in arms:
            p50 = statistics.median(m["per_class"][name]["p50_ms"] for m in table[arm].values())
            p99 = statistics.median(m["per_class"][name]["p99_ms"] for m in table[arm].values())
            cells.append(f"{fmt(p50)} / {fmt(p99)}")
        lines.append(f"| {name} | " + " | ".join(cells) + " |")
    lines += ["", "## Mean latency by phase (ms, median across seeds)", "",
              "| Phase | " + " | ".join(arms) + " |", "|---|" + "---|" * len(arms)]
    for phase in ["steady", "ramp", "flash_sale", "recovery", "shifted"]:
        cells = [fmt(statistics.median(m["cohorts"][phase]["mean_ms"] for m in table[arm].values())) for arm in arms]
        lines.append(f"| {phase} | " + " | ".join(cells) + " |")
    if "sized_classes" in config:
        lines += ["", "## Size-driven classes: mean latency overall / in the shifted phase (ms, median across seeds)", "",
                  "| Class | " + " | ".join(arms) + " |", "|---|" + "---|" * len(arms)]
        for name in config["sized_classes"]:
            cells = []
            for arm in arms:
                overall = [m.get(f"class_mean_ms:{name}") for m in table[arm].values()]
                shifted = [m.get(f"shifted_class_mean_ms:{name}") for m in table[arm].values()]
                med = lambda v: statistics.median(x for x in v if x is not None) if any(x is not None for x in v) else None
                cells.append(f"{fmt(med(overall))} / {fmt(med(shifted))}")
            lines.append(f"| {name} | " + " | ".join(cells) + " |")
        cells = [fmt(statistics.median(m["unsized_mean_ms"] for m in table[arm].values())) for arm in arms]
        lines.append("| *all other classes* | " + " | ".join(cells) + " |")
    for arm in arms:
        if not arm.startswith("predictive"):
            continue
        r = [m["routing"] for m in table[arm].values() if m["routing"].get("enabled")]
        if not r:
            continue
        outcomes = {k: statistics.median(x["outcomes"][k] for x in r) for k in r[0]["outcomes"]}
        extra = ""
        if "abs_log2_error" in r[0]:
            extra += f"; mean |log2 error| {statistics.median(x['abs_log2_error'] for x in r):.2f}"
        if "keys" in r[0]:
            extra += f"; keys {statistics.median(x['keys'] for x in r):.0f}"
        if "parent_fallbacks" in r[0]:
            extra += f"; parent fallbacks {statistics.median(x['parent_fallbacks'] for x in r):.0f}"
        lines += ["", f"## Predictor behaviour ({arm} arm, median across seeds)", "",
                  f"Lookups {statistics.median(x['lookups'] for x in r):.0f}, routed by prediction "
                  f"{statistics.median(x['routed'] for x in r):.0f}; outcomes {outcomes}; "
                  f"lookup mean {statistics.median(x['lookup_mean_us'] for x in r):.2f} µs; "
                  f"tier agreement {statistics.median(x['tier_agreement'] for x in r):.2f}; "
                  f"drift alerts {statistics.median(x['drift_alerts'] for x in r):.0f}{extra}."]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    parser.add_argument("results", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    table: dict[str, dict[int, dict]] = {}
    for seed in config["seeds"]:
        for arm in config["arms"]:
            directory = args.results / f"seed-{seed}" / arm
            if (directory / "run.json").exists():
                run, rows = load_run(directory)
                table.setdefault(arm, {})[seed] = metrics(run, rows, config)
    evaluation = evaluate(config, table)
    out = args.out or args.results
    out.mkdir(parents=True, exist_ok=True)
    (out / "summary.json").write_text(json.dumps({"config": config["version"], "evaluation": evaluation,
        "arms": {arm: {str(s): {k: v for k, v in m.items() if k != "routing"} for s, m in seeds.items()}
                 for arm, seeds in table.items()}}, indent=1, sort_keys=True) + "\n")
    (out / "report.md").write_text(report(config, table, evaluation))
    print((out / "report.md").read_text())


if __name__ == "__main__":
    main()
