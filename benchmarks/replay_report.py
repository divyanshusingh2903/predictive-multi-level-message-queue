"""Analysis for real traces replayed through the broker (docs/v4-replay-preregistration.md).

Usage: python -m benchmarks.replay_report CONFIG RESULTS_DIR [--simulation SIM.json] [--simulation-summary SUMMARY.json]
                                          [--out DIR]
       python -m benchmarks.replay_report check RUN_DIR      exit 0 if the run counts, 1 if not (used by run_replay.sh)

RESULTS_DIR holds `<arm>/<load>/<repeat>/` directories, each with the run.json and messages.tsv that
`harbinger_replay run` writes (a split run is joined with benchmarks/merge_split_run.py first). Latency is a message's
handler end minus its *scheduled* arrival, wait is its handler start minus the same, both multiplied by the run's time
scale back to trace time. Metrics, classes and gate rules are those of the simulator (`tools/trace_sim.cpp`,
`benchmarks/sim_report.py`), so a replay cell and a simulator cell are computed alike.

Windows and intervals. The replayed span is cut into `windows` (28) windows of equal trace time. The primary interval is
the moving-block bootstrap over the paired per-window changes (`bootstrap.block_length` windows, 4); the independent-window
interval over seven windows, as in v3-traces-1, is computed alongside. Where a cell has several valid repeats, the
per-window metrics are averaged over the repeats that every arm of that load has (the pooled analysis); each repeat's
own verdicts are also reported, so run-to-run noise is visible. If the single-repeat verdicts of a gate cell disagree
with each other, the cell is reported as unstable and not counted as a pass.

A run is valid only if every message completed, with none in the dead-letter queue, no submit error, no lost lease, and a
99th-percentile producer lateness of at most `max_lateness_p99_ms` (real time). Invalid runs are listed and left out.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import json
import math
import statistics
from pathlib import Path

from .sim_report import evaluate

MAX_LATENESS_P99_MS = 10.0
CLASS_NAMES = ("short", "medium", "long")


def quantile(values: list[float], q: float) -> float:
    """The simulator's quantile: the ceil(n q)-th smallest value."""
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, max(0, math.ceil(len(ordered) * q) - 1))]


def validity(run: dict, max_lateness_p99_ms: float = MAX_LATENESS_P99_MS) -> list[str]:
    """Reasons a run does not count; empty when it does."""
    reasons = []
    if run["completed"] != run["messages"]:
        reasons.append(f"{run['messages'] - run['completed']} of {run['messages']} messages did not complete")
    for field, label in (("dlq", "dead-lettered"), ("submit_errors", "submit errors"), ("leases_lost", "lost leases")):
        if run.get(field, 0):
            reasons.append(f"{run[field]} {label}")
    lateness = run.get("timing", {}).get("lateness_p99_ms")
    if lateness is None:
        reasons.append("no producer lateness recorded")
    elif lateness > max_lateness_p99_ms:
        reasons.append(f"producer lateness p99 {lateness:.1f} ms exceeds {max_lateness_p99_ms:g} ms")
    return reasons


def read_messages(path: Path, scale: float, class_bounds: list[float]) -> list[dict]:
    """Per message, in arrival order: trace arrival and duration, latency and wait in trace ms, and the class index."""
    rows = []
    with path.open() as f:
        for r in csv.DictReader(f, delimiter="\t"):
            sched, start, end = int(r["sched_ns"]), int(r["start_ns"]), int(r["end_ns"])
            submit = int(r["submit_ns"])
            duration = float(r["trace_duration_ms"])
            rows.append({"arrival": float(r["trace_arrival_ms"]), "duration": duration,
                         "latency": (end - sched) / 1e6 * scale, "wait": (start - sched) / 1e6 * scale,
                         "latency_from_submit": (end - submit) / 1e6 * scale,
                         "cls": bisect.bisect_right(class_bounds, duration), "idx": int(r["idx"])})
    rows.sort(key=lambda m: m["idx"])
    return rows


def window_edges(arrivals: list[float], windows: int) -> list[int]:
    """Row index where each window starts, with windows of equal trace time between the first and last arrival."""
    first, span = arrivals[0], arrivals[-1] - arrivals[0]
    edges = [0] + [bisect.bisect_left(arrivals, first + span * w / windows) for w in range(1, windows)]
    return edges + [len(arrivals)]


def metrics(rows: list[dict]) -> dict:
    """One window's metrics, with the simulator's names."""
    latency = [m["latency"] for m in rows]
    slowdown = [m["latency"] / max(m["duration"], 1.0) for m in rows]
    out = {"count": len(rows), "mean_latency_ms": statistics.fmean(latency), "p50_latency_ms": quantile(latency, 0.5),
           "p95_latency_ms": quantile(latency, 0.95), "p99_latency_ms": quantile(latency, 0.99),
           "slowdown_p50": quantile(slowdown, 0.5), "slowdown_p99": quantile(slowdown, 0.99),
           "long_max_wait_ms": max((m["wait"] for m in rows if m["cls"] == 2), default=0.0)}
    for c, name in enumerate(CLASS_NAMES):
        mine = [m["latency"] for m in rows if m["cls"] == c]
        out[f"{name}_mean_ms"] = statistics.fmean(mine) if mine else 0.0
        out[f"{name}_p50_ms"] = quantile(mine, 0.5) if mine else 0.0
        out[f"{name}_count"] = len(mine)
    return out


def analyze_run(directory: Path, class_bounds: list[float], window_counts: tuple[int, ...]) -> dict:
    run = json.loads((directory / "run.json").read_text())
    reasons = validity(run)
    result = {"name": directory.name, "valid": not reasons, "reasons": reasons,
              "lateness_p99_ms": run.get("timing", {}).get("lateness_p99_ms"), "messages": run["messages"]}
    if reasons:
        return result
    rows = read_messages(directory / "messages.tsv", run["time_scale"], class_bounds)
    arrivals = [m["arrival"] for m in rows]
    result["all"] = metrics(rows)
    # Sensitivity to the latency basis: the same run measured from the actual submit instead of the scheduled arrival.
    from_submit = [m["latency_from_submit"] for m in rows]
    result["submit_basis"] = {"mean_latency_ms": statistics.fmean(from_submit), "p99_latency_ms": quantile(from_submit, 0.99)}
    result["lateness_p99_trace_ms"] = run["timing"]["lateness_p99_ms"] * run["time_scale"]
    for n in window_counts:
        edges = window_edges(arrivals, n)
        result[f"windows_{n}"] = [metrics(rows[a:b]) for a, b in zip(edges, edges[1:])]
    return result


def average_windows(runs: list[list[dict]]) -> list[dict]:
    """Element-wise mean over repeats of each window's metrics (counts are identical across repeats)."""
    return [{k: (windows[0][k] if k.endswith("count") else statistics.fmean(w[k] for w in windows)) for k in windows[0]}
            for windows in zip(*runs)]


def collect(results: Path, class_bounds: list[float], window_counts: tuple[int, ...]) -> dict:
    """cells[load][arm] = {reps: {name: analysis}}, from RESULTS/<arm>/<load>/<repeat>/run.json."""
    cells: dict[str, dict[str, dict]] = {}
    for arm_dir in sorted(p for p in results.iterdir() if p.is_dir() and not p.name.startswith("_")):  # _probe is separate
        for load_dir in sorted(p for p in arm_dir.iterdir() if p.is_dir()):
            for rep in sorted(p for p in load_dir.iterdir() if (p / "run.json").exists()):
                cells.setdefault(load_dir.name, {}).setdefault(arm_dir.name, {"reps": {}})["reps"][rep.name] = \
                    analyze_run(rep, class_bounds, window_counts)
    return cells


def pooled(cell: dict[str, dict], windows_key: str) -> tuple[dict[str, dict], list[str], dict[str, list[str]]]:
    """Per arm, the pooled windows and `all` over the repeats valid for every arm that has any; also the repeats used and
    the arms left out."""
    valid = {arm: {n for n, r in info["reps"].items() if r["valid"]} for arm, info in cell.items()}
    present = {arm: names for arm, names in valid.items() if names}
    common = sorted(set.intersection(*present.values())) if present else []
    left_out = {arm: sorted(info["reps"]) for arm, info in cell.items() if arm not in present or not common}
    arms = {}
    if common:
        for arm in present:
            reps = [cell[arm]["reps"][n] for n in common]
            arms[arm] = {"windows": average_windows([r[windows_key] for r in reps]),
                         "all": {k: statistics.fmean(r["all"][k] for r in reps) for k in reps[0]["all"]}}
    return arms, common, left_out


def criteria_for(config: dict, available: set[str]) -> tuple[dict, dict]:
    """The registered criteria and gates, keeping those whose arms are all present in this replay."""
    source = config if "criteria" in config else json.loads(Path(config["simulator_config"]).read_text())
    criteria = {name: rule for name, rule in source["criteria"].items()
                if rule["arm"] in available and rule["baseline"] in available}
    gates = {gate: members for gate, members in source.get("gates", {}).items() if all(m in criteria for m in members)}
    return criteria, gates


def evaluate_cells(config: dict, cells: dict, windows_key: str, block: int | None, class_bounds: list[float]) -> dict:
    out = {}
    for load, cell in cells.items():
        arms, common, left_out = pooled(cell, windows_key)
        if "fifo" not in arms:
            out[load] = {"repeats_used": common, "left_out": left_out, "error": "no valid FIFO run"}
            continue
        criteria, gates = criteria_for(config, set(arms))
        if not criteria:
            out[load] = {"repeats_used": common, "left_out": left_out, "error": "no registered criterion has its arms in this replay",
                         "all": {a: v["all"] for a, v in arms.items()}}
            continue
        spec = {"bootstrap": {k: config["bootstrap"][k] for k in ("samples", "seed", "confidence")},
                "criteria": criteria, "gates": gates} if gates else \
               {"bootstrap": config["bootstrap"], "criteria": criteria, "gate": list(criteria)}
        verdicts = evaluate(spec, {load: {a: {"windows": v["windows"]} for a, v in arms.items()}},
                            data={"class_bounds_ms": class_bounds}, block=block)[load]
        verdicts.update({"repeats_used": common, "left_out": left_out,
                         "all": {a: v["all"] for a, v in arms.items()}})
        out[load] = verdicts
    return out


def per_repeat_verdicts(config: dict, cells: dict, windows_key: str, block: int | None, class_bounds: list[float]) -> dict:
    """Each repeat on its own (arm n against FIFO n), so run-to-run noise shows."""
    out: dict[str, dict] = {}
    for load, cell in cells.items():
        names = sorted({n for info in cell.values() for n in info["reps"]})
        for name in names:
            single = {arm: {"reps": {name: info["reps"][name]}} for arm, info in cell.items() if name in info["reps"]}
            verdict = evaluate_cells(config, {load: single}, windows_key, block, class_bounds)[load]
            if "gates_passed" in verdict:
                out.setdefault(load, {})[name] = {"gates_passed": verdict["gates_passed"],
                                                  "criteria": {k: v.get("point") for k, v in verdict["criteria"].items()}}
    return out


def stability(primary: dict, repeats: dict) -> dict:
    """Per load and gate: 'pass' or 'fail' by the pooled analysis, or 'unstable' when the single-repeat verdicts disagree
    among themselves (the registered rule: such a cell is reported as unstable and not counted as a pass). With fewer than
    two repeats there is nothing to disagree and the pooled verdict stands."""
    out: dict[str, dict] = {}
    for load, verdict in primary.items():
        for gate, ok in verdict.get("gates_passed", {}).items():
            single = {r["gates_passed"][gate] for r in repeats.get(load, {}).values() if gate in r["gates_passed"]}
            out.setdefault(load, {})[gate] = "unstable" if len(single) > 1 else ("pass" if ok else "fail")
    return out


def submit_basis_difference(cells: dict) -> dict:
    """Largest relative difference over valid runs between mean latency from the scheduled arrival and from the actual submit."""
    worst = 0.0
    for cell in cells.values():
        for info in cell.values():
            for r in info["reps"].values():
                if r["valid"]:
                    worst = max(worst, abs(r["submit_basis"]["mean_latency_ms"] / r["all"]["mean_latency_ms"] - 1.0))
    return {"max_relative_difference_in_mean_latency": worst}


def relative_change(all_metrics: dict, arm: str) -> float | None:
    if arm not in all_metrics or "fifo" not in all_metrics:
        return None
    return all_metrics[arm]["mean_latency_ms"] / all_metrics["fifo"]["mean_latency_ms"] - 1.0


def simulator_comparison(primary: dict, simulation: dict | None, sim_summary: dict | None) -> dict:
    """Per load: gate verdicts of the replay and the simulator, and the ratio of mean-latency changes against FIFO."""
    if not simulation:
        return {}
    out = {}
    for load, verdict in primary.items():
        sim_arms = simulation["results"].get(load)
        if not sim_arms or "all" not in verdict:
            continue
        sim_all = {a: v["all"] for a, v in sim_arms.items()}
        rows = {}
        for arm in verdict["all"]:
            real, sim = relative_change(verdict["all"], arm), relative_change(sim_all, arm)
            if arm != "fifo" and real is not None and sim is not None:
                rows[arm] = {"replay": real, "simulator": sim, "ratio": real / sim if sim else None}
        entry = {"mean_latency_change": rows}
        if sim_summary and "gates_passed" in verdict and load in sim_summary["evaluation"]:
            sim_gates = sim_summary["evaluation"][load]["gates_passed"]
            entry["gates"] = {g: {"replay": ok, "simulator": sim_gates.get(g), "same": ok == sim_gates.get(g)}
                              for g, ok in verdict["gates_passed"].items()}
        out[load] = entry
    return out


def overhead_probe(results: Path) -> dict:
    """The broker's added latency at light load (RESULTS/_probe/<arm>/<load>/<repeat>): latency minus the job's own duration,
    in trace ms, overall and by job length. Reported, not gated; it states the floor under the short-job results."""
    out: dict[str, dict] = {}
    edges = (1.0, 10.0, 100.0, float("inf"))
    for run_json in sorted((results / "_probe").glob("*/*/*/run.json")) if (results / "_probe").exists() else []:
        run = json.loads(run_json.read_text())
        reasons = validity(run)
        key = "/".join(run_json.parts[-4:-1])
        if reasons:
            out[key] = {"valid": False, "reasons": reasons}
            continue
        rows = read_messages(run_json.parent / "messages.tsv", run["time_scale"], [])
        extra = [max(0.0, m["latency"] - m["duration"]) for m in rows]
        entry = {"valid": True, "messages": len(rows), "overhead_p50_ms": quantile(extra, 0.5), "overhead_p99_ms": quantile(extra, 0.99)}
        low = 0.0
        for edge in edges:
            mine = [e for m, e in zip(rows, extra) if low <= m["duration"] < edge]
            if mine:
                entry[f"duration_{low:g}_to_{edge:g}_ms"] = {"count": len(mine), "p50_ms": quantile(mine, 0.5), "p99_ms": quantile(mine, 0.99)}
            low = edge
        out[key] = entry
    return out


def noise(cells: dict) -> dict:
    """Run-to-run spread of each cell's all-message mean latency over its valid repeats."""
    out = {}
    for load, cell in cells.items():
        for arm, info in cell.items():
            means = [r["all"]["mean_latency_ms"] for r in info["reps"].values() if r["valid"]]
            if len(means) > 1:
                out.setdefault(load, {})[arm] = {"repeats": len(means), "min_ms": min(means), "max_ms": max(means),
                                                 "max_over_min": max(means) / min(means)}
    return out


def table(config: dict, evaluation: dict, load: str) -> list[str]:
    from .sim_report import criteria_table
    return criteria_table({"criteria": config_criteria(config, evaluation[load]), "primary_arm": "predictive_warm"},
                          evaluation[load]["criteria"])


def config_criteria(config: dict, verdict: dict) -> dict:
    return criteria_for(config, set(verdict["all"]))[0]


def report(config: dict, cells: dict, primary: dict, independent: dict, comparison: dict, repeats: dict, spread: dict,
           status: dict | None = None) -> str:
    lines = [f"# Real trace replay ({config['version']}{', ' + config['trace']['name'] if 'trace' in config else ''})", "",
             f"{config['workers']} workers, time scale {config['time_scale']:g}, {config['windows']} windows of equal trace time; "
             f"primary interval: moving-block bootstrap, block {config['bootstrap']['block_length']}, "
             f"{config['bootstrap']['samples']} resamples. The independent seven-window interval of v3-traces-1 is listed after.", ""]
    invalid = [(load, arm, name, r["reasons"]) for load, cell in cells.items() for arm, info in cell.items()
               for name, r in info["reps"].items() if not r["valid"]]
    lines += ["## Runs left out", ""] + ([f"- {a} @ {l} / {n}: {'; '.join(why)}" for l, a, n, why in invalid] or ["none"]) + [""]
    for load, verdict in primary.items():
        lines += [f"## Offered load {float(load):.2f}", ""]
        if "error" in verdict:
            lines += [f"Not analysed: {verdict['error']}.", ""]
            continue
        lines += [f"Repeats used (valid for every arm): {', '.join(verdict['repeats_used'])}.", "",
                  "| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |",
                  "|---|---|---|---|---|---|---|---|"]
        for arm, a in verdict["all"].items():
            lines.append(f"| {arm} | {a['mean_latency_ms'] / 1000:.2f} | {a['p50_latency_ms']:.0f} | {a['p99_latency_ms'] / 1000:.1f} | "
                         f"{a['short_p50_ms']:.0f} | {a['long_mean_ms'] / 1000:.1f} | {a['long_max_wait_ms'] / 1000:.1f} | "
                         f"{a['slowdown_p50']:.1f} / {a['slowdown_p99']:.0f} |")
        lines += ["", "Block bootstrap (primary):", ""] + table(config, primary, load)
        for gate, ok in verdict.get("gates_passed", {}).items():
            state = (status or {}).get(load, {}).get(gate)
            label = "UNSTABLE (the repeats disagree; not counted as a pass)" if state == "unstable" else ("PASSED" if ok else "NOT PASSED")
            lines.append(f"\n**Gate {gate} at load {float(load):.2f}: {label}.**")
        if load in independent and "criteria" in independent[load]:
            lines += ["", "Independent seven windows (v3-traces-1 rule):", ""] + table(config, independent, load)
            for gate, ok in independent[load].get("gates_passed", {}).items():
                lines.append(f"\n**Gate {gate} (seven windows): {'PASSED' if ok else 'NOT PASSED'}.**")
        if load in repeats:
            lines += ["", "Each repeat on its own:", ""] + [f"- {name}: {r['gates_passed']}" for name, r in repeats[load].items()]
        lines.append("")
    if comparison:
        lines += ["## Against the simulator", "",
                  "Mean-latency change against FIFO (all messages), replay and simulator, and their ratio; gate verdicts side by side.", ""]
        for load, entry in comparison.items():
            lines += [f"### Load {float(load):.2f}", "", "| Arm | Replay | Simulator | Ratio |", "|---|---|---|---|"]
            ratio = lambda v: "" if v["ratio"] is None else f"{v['ratio']:.2f}"
            lines += [f"| {a} | {v['replay']:+.1%} | {v['simulator']:+.1%} | {ratio(v)} |"
                      for a, v in entry["mean_latency_change"].items()]
            lines += [""] + [f"- gate {g}: replay {'pass' if v['replay'] else 'fail'}, simulator "
                              f"{'pass' if v['simulator'] else 'fail'}{'' if v['same'] else ' (differs)'}"
                              for g, v in entry.get("gates", {}).items()] + [""]
    if spread:
        lines += ["## Run-to-run spread (all-message mean latency over valid repeats)", "",
                  "| Load | Arm | Repeats | Min s | Max s | Max/min |", "|---|---|---|---|---|---|"]
        lines += [f"| {l} | {a} | {s['repeats']} | {s['min_ms'] / 1000:.2f} | {s['max_ms'] / 1000:.2f} | {s['max_over_min']:.3f} |"
                  for l, arms in spread.items() for a, s in arms.items()] + [""]
    return "\n".join(lines) + "\n"


def check(directory: Path) -> int:
    """Exit status for the runner: 0 if the run counts, 1 (with the reasons) if it does not."""
    reasons = validity(json.loads((directory / "run.json").read_text()))
    for reason in reasons:
        print(f"invalid: {reason}")
    return 1 if reasons else 0


def main() -> None:
    import sys
    if len(sys.argv) == 3 and sys.argv[1] == "check":
        raise SystemExit(check(Path(sys.argv[2])))
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("config", type=Path)
    parser.add_argument("results", type=Path)
    parser.add_argument("--simulation", type=Path, help="the simulator's simulation.json for the same trace")
    parser.add_argument("--simulation-summary", type=Path, help="the simulator's summary.json (v3-traces-1 gate verdicts)")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    bounds = [float(b) for b in config["class_bounds_ms"]]
    cells = collect(args.results, bounds, (config["windows"], 7))
    w, block = f"windows_{config['windows']}", config["bootstrap"]["block_length"]
    primary = evaluate_cells(config, cells, w, block, bounds)
    independent = evaluate_cells(config, cells, "windows_7", None, bounds)
    repeats = per_repeat_verdicts(config, cells, w, block, bounds)
    comparison = simulator_comparison(primary, json.loads(args.simulation.read_text()) if args.simulation else None,
                                      json.loads(args.simulation_summary.read_text()) if args.simulation_summary else None)
    spread = noise(cells)
    status = stability(primary, repeats)
    basis = submit_basis_difference(cells)
    probe = overhead_probe(args.results)
    out = args.out or args.results
    summary = {"config": config["version"], "primary_block_bootstrap": primary, "independent_seven_windows": independent,
               "per_repeat": repeats, "gate_status": status, "latency_basis": basis, "simulator_comparison": comparison, "run_to_run": spread, "overhead_probe": probe,
               "runs": {l: {a: {n: {k: r[k] for k in ("valid", "reasons", "lateness_p99_ms", "lateness_p99_trace_ms") if k in r} for n, r in i["reps"].items()}
                            for a, i in c.items()} for l, c in cells.items()}}
    (out / "replay_summary.json").write_text(json.dumps(summary, indent=1) + "\n")
    text = report(config, cells, primary, independent, comparison, repeats, spread, status)
    text += (f"\nLatency basis: measured from the actual submit instead of the scheduled arrival, mean latency differs by at most "
             f"{basis['max_relative_difference_in_mean_latency']:.2%} on any valid run.\n")
    if probe:
        text += "\n## Overhead probe (light load; latency minus the job's own duration, trace ms)\n\n" + "\n".join(
            f"- {k}: " + (f"p50 {v['overhead_p50_ms']:.2f}, p99 {v['overhead_p99_ms']:.2f} over {v['messages']} messages" if v["valid"]
                          else "invalid: " + "; ".join(v["reasons"])) for k, v in probe.items()) + "\n"
    (out / "replay_report.md").write_text(text)
    print(text)


if __name__ == "__main__":
    main()
