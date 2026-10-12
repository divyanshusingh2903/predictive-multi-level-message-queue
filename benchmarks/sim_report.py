"""Pre-registered analysis for trace simulations (v2-azure-sim-1, v3-aging-1, v3-traces-1).

Usage: python -m benchmarks.sim_report CONFIG SIM_OUTPUT.json [--out DIR]
Criteria pair arms by evaluation window (7 windows); intervals use the same bootstrap as the broker benchmark.
A criterion without "arm" uses the config's "primary_arm". A config has one "gate" (a list of criteria) or several
named "gates" ({name: [criteria]}), each passed or not on its own. A criterion with "floor" raises both values to at least
that floor before comparing, so a window where neither arm made a long job wait (0 vs 0) counts as equal, not as
unevaluable. "floor": "long_class_bound" uses the simulation's long-class lower bound (class_bounds_ms[1]), so the
floor follows the trace's own scale. A criterion with "min_class_count": {"class": "long", "count": N} is unevaluable
(not passed) unless every compared window holds at least N jobs of that class, so an empty class cannot pass. With "report_without_extreme_window", every criterion is
also evaluated without the window whose FIFO mean latency is highest (per load); that view is reported, not gated.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from .prodflow_report import bootstrap


def extreme_window(arms: dict) -> int:
    """Index of the evaluation window with the highest FIFO mean latency."""
    windows = arms["fifo"]["windows"]
    return max(range(len(windows)), key=lambda i: windows[i]["mean_latency_ms"])


def resolve_floor(rule: dict, data: dict | None) -> float:
    floor = rule.get("floor", 0.0)
    if floor == "long_class_bound":
        if not data or "class_bounds_ms" not in data:
            raise ValueError("floor long_class_bound needs a simulation with class_bounds_ms")
        return float(data["class_bounds_ms"][1])
    return float(floor)


def evaluate(config: dict, results: dict, drop_extreme: bool = False, data: dict | None = None) -> dict:
    b = config["bootstrap"]
    out = {}
    for utilization, arms in results.items():
        verdicts = {}
        skip = extreme_window(arms) if drop_extreme else None
        for name, rule in config["criteria"].items():
            arm, baseline, metric = rule.get("arm", config.get("primary_arm")), rule["baseline"], rule["metric"]
            floor = resolve_floor(rule, data)
            compared = [(w, v) for i, (w, v) in enumerate(zip(arms[arm]["windows"], arms[baseline]["windows"])) if i != skip]
            if "min_class_count" in rule:
                need = rule["min_class_count"]
                key = f"{need['class']}_count"
                if any(key not in w or w[key] < need["count"] or v[key] < need["count"] for w, v in compared):
                    verdicts[name] = {"status": "unevaluable", "reason": f"a window has fewer than {need['count']} {need['class']} jobs"}
                    continue
            pairs = [(max(w[metric], floor), max(v[metric], floor)) for w, v in compared]
            if any(c == 0 for _, c in pairs):
                verdicts[name] = {"status": "unevaluable"}
                continue
            if "max_ratio" in rule:
                point, ci = bootstrap([a / c for a, c in pairs], b["samples"], b["seed"], b["confidence"])
                verdicts[name] = {"kind": "ratio", "point": point, "ci95": ci,
                                  "status": "pass" if ci[1] <= rule["max_ratio"] else "fail"}
                continue
            point, ci = bootstrap([(a - c) / c for a, c in pairs], b["samples"], b["seed"], b["confidence"])
            limit = rule["max_relative_delta"]
            ok = (point <= limit and ci[1] < 0) if limit < 0 else (ci[1] < 0) if limit == 0 else ci[1] <= limit
            verdicts[name] = {"kind": "relative_delta", "point": point, "ci95": ci, "status": "pass" if ok else "fail"}
        if "gates" in config:
            passed = {gate: all(verdicts[name]["status"] == "pass" for name in members)
                      for gate, members in config["gates"].items()}
            out[utilization] = {"criteria": verdicts, "gates_passed": passed}
        else:
            gate = all(verdicts[name]["status"] == "pass" for name in config["gate"])
            out[utilization] = {"criteria": verdicts, "gate_passed": gate}
        if skip is not None:
            out[utilization]["excluded_window"] = skip
    return out


def criteria_table(config: dict, verdicts: dict) -> list[str]:
    lines = ["| Criterion | Arm vs baseline | Point | 95% CI | Result |", "|---|---|---|---|---|"]
    for name, v in verdicts.items():
        rule = config["criteria"][name]
        who = f"{rule.get('arm', config.get('primary_arm'))} vs {rule['baseline']}"
        if v.get("kind") == "ratio":
            shown, interval = f"{v['point']:.2f}×", f"[{v['ci95'][0]:.2f}, {v['ci95'][1]:.2f}]"
        elif v.get("kind"):
            shown, interval = f"{v['point']:+.1%}", f"[{v['ci95'][0]:+.1%}, {v['ci95'][1]:+.1%}]"
        else:
            shown = interval = "–"
        lines.append(f"| {name} | {who} | {shown} | {interval} | **{v['status']}** |")
    return lines


def report(config: dict, data: dict, evaluation: dict, without_extreme: dict | None = None) -> str:
    title = config["trace"].get("title", "Azure Functions trace simulation")
    lines = [f"# {title} ({config['version']})", "",
             f"{data['evaluation_rows']} simulated invocations after {data['history_rows']} history invocations; "
             f"tier boundaries (history terciles) {data['boundaries_ms'][0]:.0f} / {data['boundaries_ms'][1]:.0f} ms; "
             f"{config['workers']} workers, aging {config['aging']['threshold_ms']}/{config['aging']['interval_ms']} ms.", ""]
    if "window_rows" in data:  # v3-traces rules
        lines += [f"History split by {data['history_split']}, windows by {data['window_split']} "
                        f"(rows per window {', '.join(map(str, data['window_rows']))}); metric classes short < "
                        f"{data['class_bounds_ms'][0]:g} ms <= medium < {data['class_bounds_ms'][1]:g} ms <= long.", ""]
    for utilization, arms in data["results"].items():
        lines += [f"## Offered load {float(utilization):.2f}", "",
                  "| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |",
                  "|---|---|---|---|---|---|---|---|"]
        for arm in config["arms"]:
            a = arms[arm]["all"]
            lines.append(f"| {arm} | {a['mean_latency_ms'] / 1000:.2f} | {a['p50_latency_ms']:.0f} | {a['p99_latency_ms'] / 1000:.1f} | "
                         f"{a['short_p50_ms']:.0f} | {a['long_mean_ms'] / 1000:.1f} | {a['long_max_wait_ms'] / 1000:.1f} | "
                         f"{a['slowdown_p50']:.1f} / {a['slowdown_p99']:.0f} |")
        lines += [""] + criteria_table(config, evaluation[utilization]["criteria"])
        if "gates_passed" in evaluation[utilization]:
            lines += [""] + [f"**Gate {gate} at load {float(utilization):.2f}: {'PASSED' if ok else 'NOT PASSED'}.**"
                             for gate, ok in evaluation[utilization]["gates_passed"].items()] + [""]
        else:
            lines += ["", f"**Gate at load {float(utilization):.2f}: {'PASSED' if evaluation[utilization]['gate_passed'] else 'NOT PASSED'}.**", ""]
        if without_extreme:
            view = without_extreme[utilization]
            lines += [f"Without the extreme window (window {view['excluded_window']}, highest FIFO mean; reported, "
                      "not gated):", ""] + criteria_table(config, view["criteria"]) + [""]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    parser.add_argument("simulation", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    data = json.loads(args.simulation.read_text())
    evaluation = evaluate(config, data["results"], data=data)
    without_extreme = evaluate(config, data["results"], drop_extreme=True, data=data) if config.get("report_without_extreme_window") else None
    out = args.out or args.simulation.parent
    summary = {"config": config["version"], "evaluation": evaluation}
    if without_extreme:
        summary["without_extreme_window"] = without_extreme
    (out / "summary.json").write_text(json.dumps(summary, indent=1) + "\n")
    (out / "report.md").write_text(report(config, data, evaluation, without_extreme))
    print((out / "report.md").read_text())


if __name__ == "__main__":
    main()
