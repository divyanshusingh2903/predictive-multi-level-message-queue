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
import math
import random
import statistics
import sys
from pathlib import Path

from .prodflow_report import bootstrap


def block_bootstrap(values: list[float], block: int, samples: int, seed: int, confidence: float) -> tuple[float, list[float]]:
    """Moving-block bootstrap of the mean: resample overlapping blocks of `block` consecutive values, so adjacent windows
    that share a backlog stay together. A block of 1 is the ordinary bootstrap. Blocks are drawn until the series length
    is reached and the last is cut short; the interval is the percentile interval, as in `prodflow_report.bootstrap`."""
    n = len(values)
    if not 1 <= block <= n:
        raise ValueError(f"block length {block} must be between 1 and the {n} windows")
    rng = random.Random(seed)
    starts = n - block + 1
    means = []
    for _ in range(samples):
        picked: list[float] = []
        while len(picked) < n:
            at = rng.randrange(starts)
            picked.extend(values[at:at + block])
        means.append(statistics.mean(picked[:n]))
    means.sort()
    low = means[max(0, math.floor(samples * (1 - confidence) / 2))]
    high = means[min(samples - 1, math.ceil(samples * (1 + confidence) / 2) - 1)]
    return statistics.mean(values), [low, high]


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


def evaluate(config: dict, results: dict, drop_extreme: bool = False, data: dict | None = None, block: int | None = None) -> dict:
    """With `block`, intervals come from the moving-block bootstrap of that length (v4-sensitivity-1, part B); without
    it they are the independent-window bootstrap of the earlier registrations, unchanged."""
    b = config["bootstrap"]
    interval = (lambda values: block_bootstrap(values, block, b["samples"], b["seed"], b["confidence"])) if block else \
        (lambda values: bootstrap(values, b["samples"], b["seed"], b["confidence"]))
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
                point, ci = interval([a / c for a, c in pairs])
                verdicts[name] = {"kind": "ratio", "point": point, "ci95": ci,
                                  "status": "pass" if ci[1] <= rule["max_ratio"] else "fail"}
                continue
            point, ci = interval([(a - c) / c for a, c in pairs])
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
             f"tier boundaries (history quantiles) {' / '.join(f'{x:.0f}' for x in data['boundaries_ms'])} ms; "
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


def verdict_changes(old: dict, new: dict) -> list[str]:
    """Gate cells (load, gate) whose pass/fail differs between two `evaluation` dicts (T2 of v4-sensitivity-1)."""
    changes = []
    for load, cell in new.items():
        for gate, ok in cell["gates_passed"].items():
            before = old[load]["gates_passed"][gate]
            if before != ok:
                changes.append(f"load {float(load):.2f} gate {gate}: {'pass' if before else 'fail'} -> {'pass' if ok else 'fail'}")
    return changes


def block_report(config: dict, data: dict, blocks: list[int], primary: int, old: dict | None) -> tuple[dict, str]:
    """Gate verdicts at each block length; the primary length's intervals are the ones the paper uses."""
    summary = {str(b): evaluate(config, data["results"], data=data, block=b) for b in blocks}
    lines = [f"# Block-bootstrap re-estimation ({config['version']})", "",
             f"{len(next(iter(data['results'].values()))['fifo']['windows'])} windows; moving blocks of {', '.join(map(str, blocks))} "
             f"windows, {config['bootstrap']['samples']} resamples. Block {primary} is primary.", ""]
    for load in data["results"]:
        for name in config["criteria"]:
            if not name.endswith("P1_mean_latency_vs_fifo"):
                continue
            row = [f"{name} @ {float(load):.2f}"]
            for b in blocks:
                v = summary[str(b)][load]["criteria"][name]
                row.append(f"block {b}: {v['point']:+.1%} [{v['ci95'][0]:+.1%}, {v['ci95'][1]:+.1%}] {v['status']}"
                           if "point" in v else f"block {b}: {v['status']}")
            lines.append("- " + "; ".join(row))
    lines += [""]
    if old is not None:
        changed = verdict_changes(old, summary[str(primary)])
        lines += ["Cells whose verdict differs from v3-traces-1 (block %d):" % primary,
                  *(f"- {c}" for c in changed or ["none"]), ""]
    return summary, "\n".join(lines) + "\n"


def sensitivity(manifest: dict, outputs: dict, config: dict) -> dict:
    """Per variant, the P1 criteria of both gates at the one registered load, and the R1 to R3 readings per parameter.

    `manifest["variants"]` lists {id, parameter, value, default, step} where `step` is the value's position in the
    parameter's grid; `outputs[id]` is that variant's simulation. A variant passes P1 for a gate when the gate's P1
    criterion passes."""
    b = config["bootstrap"]
    verdicts: dict[str, dict] = {}
    for variant in manifest["variants"]:
        cfg = {"bootstrap": b, "criteria": config["criteria"], "gate": list(config["criteria"])}
        result = evaluate(cfg, outputs[variant["id"]]["results"], data=outputs[variant["id"]])
        (cell,) = result.values()
        verdicts[variant["id"]] = {name: cell["criteria"][name] for name in ("A_P1_mean_latency_vs_fifo", "B_P1_mean_latency_vs_fifo")}
    default = next(v["id"] for v in manifest["variants"] if v["default"])
    readings: dict[str, dict] = {}
    for gate in ("A_P1_mean_latency_vs_fifo", "B_P1_mean_latency_vs_fifo"):
        baseline = verdicts[default][gate]["status"]
        by_parameter: dict[str, list[dict]] = {}
        for v in manifest["variants"]:
            if not v["default"]:
                by_parameter.setdefault(v["parameter"], []).append(v)
        for parameter, variants in by_parameter.items():
            flipped = [v for v in variants if verdicts[v["id"]][gate]["status"] != baseline]
            readings.setdefault(gate, {})[parameter] = {
                "robust": not flipped,                                  # R1
                "flips_at": [v["value"] for v in flipped],              # R2
                "cliff": any(v["step"] == 1 for v in flipped),          # R3: one grid step from the default
            }
    passed = {gate: sum(verdicts[v["id"]][gate]["status"] == "pass" for v in manifest["variants"]) for gate in
              ("A_P1_mean_latency_vs_fifo", "B_P1_mean_latency_vs_fifo")}
    return {"default": default, "verdicts": verdicts, "readings": readings, "passing_variants": passed,
            "variants": len(manifest["variants"])}


def sensitivity_main(argv: list[str]) -> None:
    parser = argparse.ArgumentParser(prog="sim_report sensitivity")
    parser.add_argument("manifest", type=Path, help="manifest.json written by make_sensitivity_configs.py")
    parser.add_argument("outputs", type=Path, help="directory holding <variant id>.json simulation outputs")
    args = parser.parse_args(argv)
    manifest = json.loads(args.manifest.read_text())
    config = json.loads(Path(manifest["base_config"]).read_text())
    outputs = {v["id"]: json.loads((args.outputs / f"{v['id']}.json").read_text()) for v in manifest["variants"]}
    result = sensitivity(manifest, outputs, config)
    (args.outputs / "sensitivity.json").write_text(json.dumps(result, indent=1) + "\n")
    lines = [f"# Parameter sensitivity ({manifest['version']}, {manifest['trace']})", "",
             f"{result['variants']} variants at load {manifest['load']}; default variant `{result['default']}`.", "",
             "| Variant | Parameter | Value | A P1 | B P1 |", "|---|---|---|---|---|"]
    for v in manifest["variants"]:
        cells = []
        for gate in ("A_P1_mean_latency_vs_fifo", "B_P1_mean_latency_vs_fifo"):
            c = result["verdicts"][v["id"]][gate]
            cells.append(f"{c['point']:+.1%} [{c['ci95'][0]:+.1%}, {c['ci95'][1]:+.1%}] {c['status']}" if "point" in c else c["status"])
        lines.append(f"| {v['id']} | {v['parameter']} | {v['value']} | {cells[0]} | {cells[1]} |")
    lines += ["", f"Variants passing P1: {result['passing_variants']}", ""]
    for gate, params in result["readings"].items():
        lines += [f"**{gate}**", *(f"- {p}: {'robust (R1)' if r['robust'] else 'sensitive (R2), flips at ' + ', '.join(map(str, r['flips_at']))}"
                                    f"{'; default on a cliff (R3)' if r['cliff'] else ''}" for p, r in params.items()), ""]
    (args.outputs / "sensitivity.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


def main() -> None:
    if len(sys.argv) > 1 and sys.argv[1] == "sensitivity":
        return sensitivity_main(sys.argv[2:])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    parser.add_argument("simulation", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--blocks", type=int, nargs="+", help="also write block_bootstrap.{json,md} for these block lengths")
    parser.add_argument("--primary-block", type=int, default=4)
    parser.add_argument("--compare", type=Path, help="summary.json of v3-traces-1, for the verdict-change list")
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
    if args.blocks:
        old = json.loads(args.compare.read_text())["evaluation"] if args.compare else None
        blocks, text = block_report(config, data, args.blocks, args.primary_block, old)
        (out / "block_bootstrap.json").write_text(json.dumps(blocks, indent=1) + "\n")
        (out / "block_bootstrap.md").write_text(text)
        print(text)


if __name__ == "__main__":
    main()
