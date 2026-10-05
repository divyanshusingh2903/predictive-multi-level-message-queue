"""Pre-registered analysis for the Azure Functions trace simulation (v2-azure-sim-1).

Usage: python -m benchmarks.sim_report CONFIG SIM_OUTPUT.json [--out DIR]
Criteria pair arms by evaluation window (7 windows); intervals use the same bootstrap as the broker benchmark.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from .prodflow_report import bootstrap


def evaluate(config: dict, results: dict) -> dict:
    b = config["bootstrap"]
    out = {}
    for utilization, arms in results.items():
        verdicts = {}
        for name, rule in config["criteria"].items():
            arm, baseline, metric = rule["arm"], rule["baseline"], rule["metric"]
            pairs = [(w[metric], v[metric]) for w, v in zip(arms[arm]["windows"], arms[baseline]["windows"])]
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
        gate = all(verdicts[name]["status"] == "pass" for name in config["gate"])
        out[utilization] = {"criteria": verdicts, "gate_passed": gate}
    return out


def report(config: dict, data: dict, evaluation: dict) -> str:
    lines = [f"# Azure Functions trace simulation ({config['version']})", "",
             f"{data['evaluation_rows']} simulated invocations after {data['history_rows']} history invocations; "
             f"tier boundaries (history terciles) {data['boundaries_ms'][0]:.0f} / {data['boundaries_ms'][1]:.0f} ms; "
             f"{config['workers']} workers, aging {config['aging']['threshold_ms']}/{config['aging']['interval_ms']} ms.", ""]
    for utilization, arms in data["results"].items():
        lines += [f"## Offered load {float(utilization):.2f}", "",
                  "| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |",
                  "|---|---|---|---|---|---|---|---|"]
        for arm in config["arms"]:
            a = arms[arm]["all"]
            lines.append(f"| {arm} | {a['mean_latency_ms'] / 1000:.2f} | {a['p50_latency_ms']:.0f} | {a['p99_latency_ms'] / 1000:.1f} | "
                         f"{a['short_p50_ms']:.0f} | {a['long_mean_ms'] / 1000:.1f} | {a['long_max_wait_ms'] / 1000:.1f} | "
                         f"{a['slowdown_p50']:.1f} / {a['slowdown_p99']:.0f} |")
        lines += ["", "| Criterion | Point | 95% CI | Result |", "|---|---|---|---|"]
        for name, v in evaluation[utilization]["criteria"].items():
            if v.get("kind") == "ratio":
                shown, interval = f"{v['point']:.2f}×", f"[{v['ci95'][0]:.2f}, {v['ci95'][1]:.2f}]"
            elif v.get("kind"):
                shown, interval = f"{v['point']:+.1%}", f"[{v['ci95'][0]:+.1%}, {v['ci95'][1]:+.1%}]"
            else:
                shown = interval = "–"
            lines.append(f"| {name} | {shown} | {interval} | **{v['status']}** |")
        lines += ["", f"**Gate at load {float(utilization):.2f}: {'PASSED' if evaluation[utilization]['gate_passed'] else 'NOT PASSED'}.**", ""]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    parser.add_argument("simulation", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    data = json.loads(args.simulation.read_text())
    evaluation = evaluate(config, data["results"])
    out = args.out or args.simulation.parent
    (out / "summary.json").write_text(json.dumps({"config": config["version"], "evaluation": evaluation}, indent=1) + "\n")
    (out / "report.md").write_text(report(config, data, evaluation))
    print((out / "report.md").read_text())


if __name__ == "__main__":
    main()
