#!/usr/bin/env python3
"""Simulator validation (v4-simvalidation, docs/v4-simvalidation-preregistration.md).

Replays measured production-flow runs through harbinger_trace_sim and compares the simulator's effects with the real
broker's. Each real run (one seed, one arm) is converted to a harbinger-trace-v1 file, replayed in replay mode under the
matching simulator arm, and both are summarised with the same metric code (prodflow_report.metrics).

Usage:
  python benchmarks/sim_validation.py --sim build/harbinger_trace_sim --work DIR --out DIR \
      --set local=~/data/rerun-2026-10-12/v2-prodflow/raw --set aws=~/data/aws-run/.../e1
where each SET holds seed-N/ARM/{messages.tsv,run.json}.

This is the analysis for the experiment. It does not tune the simulator: it reports what the pre-registered criteria
say, whichever way they come out.
"""
import argparse
import csv
import json
import math
import statistics
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from analysis import trace_format  # noqa: E402
from benchmarks import prodflow_report  # noqa: E402

CRITERIA = json.loads((Path(__file__).parent / "configs" / "v4-simvalidation.json").read_text())
PHASES = CRITERIA["criteria"]["V2_phase_direction"]["phases"]
ARMS = CRITERIA["arms"]  # real arm -> simulator arm; the real predictive arm starts empty, hence predictive_cold
EFFECT_ARMS = CRITERIA["criteria"]["V1_effect_agreement"]["arms"]
SIM = CRITERIA["simulator"]
STATIC_MAPS = {k: v for k, v in CRITERIA["static_maps"].items() if k != "source"}
EFFECT_MARGIN = CRITERIA["criteria"]["V1_effect_agreement"]["max_gap_points"] / 100
GROUPS = CRITERIA["criteria"]["V3_group_order"]["groups_best_to_worst"]
SHARE_SEEDS_SAME_ORDER = CRITERIA["criteria"]["V3_group_order"]["min_share_of_seeds"]


def read_messages(directory: Path) -> list[dict]:
    with (directory / "messages.tsv").open(newline="") as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


def sorted_like_converter(rows: list[dict]) -> list[dict]:
    """The converter's row order: by submission time, ties by seq."""
    return sorted(rows, key=lambda r: (int(r["submit_ns"]), int(r["seq"])))


def simulate(sim: Path, work: Path, run_dir: Path, arm: str, config: dict) -> list[dict]:
    """Replay one measured run under the simulator arm matching `arm`; returns messages.tsv-shaped rows whose
    timestamps are the simulator's, so that prodflow_report.metrics applies unchanged."""
    name = f"{run_dir.parent.name}-{arm}"
    trace_rows, notes = trace_format.from_prodflow(run_dir)
    trace = work / f"{name}.csv"
    if trace.exists():
        trace.unlink()
    trace_format.write(trace, trace_rows, with_size=True)
    bounds = notes["oracle_boundaries_ms"]
    sim_config = {
        "version": CRITERIA["version"], "replay": SIM["replay"], "workers": json.loads((run_dir / "run.json").read_text())["workers"],
        "levels": SIM["levels"], "default_priority": SIM["default_priority"], "aging": SIM["aging"],
        "history_fraction": SIM["history_fraction"], "evaluation_days": 1, "class_bounds_ms": bounds,
        "oracle_bounds_ms": bounds, "misconfigured_seed": SIM["misconfigured_seed"], "arms": [ARMS[arm]],
        "static_maps": STATIC_MAPS,
        "per_message_dir": str(work / f"{name}-messages"),
    }
    config_path, out = work / f"{name}.json", work / f"{name}.out.json"
    config_path.write_text(json.dumps(sim_config))
    subprocess.run([str(sim), str(trace), str(config_path), str(out)], check=True, capture_output=True)
    with (work / f"{name}-messages" / f"{ARMS[arm]}.tsv").open() as stream:
        dumped = list(csv.DictReader(stream, delimiter="\t"))
    ordered = sorted_like_converter(read_messages(run_dir))
    if len(dumped) != len(ordered):
        raise RuntimeError(f"{name}: simulator returned {len(dumped)} messages for {len(ordered)}")
    rows = []
    for real, sim_row in zip(ordered, dumped):
        if real["job_type"] != trace_rows[int(sim_row["index"])].key:
            raise RuntimeError(f"{name}: message order differs between the trace and messages.tsv")
        latency_ns, wait_ns = float(sim_row["latency_ms"]) * 1e6, float(sim_row["wait_ms"]) * 1e6
        rows.append({**real, "submit_ns": "0", "first_start_ns": str(int(wait_ns)), "start_ns": str(int(wait_ns)),
                     "end_ns": str(int(latency_ns)), "done": "1"})
    return rows


def paired_delta(values: dict, arm: str, seeds: list[str], key=lambda m: m) -> float:
    """Mean over seeds of (arm / fifo - 1), paired by seed, as equation (1) of the paper."""
    return statistics.mean(key(values[arm][s]) / key(values["fifo"][s]) - 1 for s in seeds)


def ranking(table: dict, seed: str) -> list[str]:
    return [a for a, _ in sorted(((a, table[a][seed]["mean_latency_ms"]) for a in ARMS), key=lambda x: x[1])]


def groups_in_order(table: dict, seed: str) -> bool:
    """V3: every arm of a group is faster than every arm of the next group. Order inside a group is not compared."""
    mean = {a: table[a][seed]["mean_latency_ms"] for a in ARMS}
    return all(max(mean[a] for a in earlier) < min(mean[a] for a in later) for earlier, later in zip(GROUPS, GROUPS[1:]))


def summarise(real: dict, sim: dict, seeds: list[str]) -> dict:
    """real[arm][seed] and sim[arm][seed] are prodflow_report metrics. Applies V1 to V5 to one measured set."""
    out = {"seeds": seeds, "V1": {}, "V2": {}, "V4": {}, "V5": {}}
    mean = lambda m: m["mean_latency_ms"]
    for arm in EFFECT_ARMS:
        d_real, d_sim = paired_delta(real, arm, seeds, mean), paired_delta(sim, arm, seeds, mean)
        out["V1"][arm] = {"real": d_real, "sim": d_sim, "gap_points": abs(d_sim - d_real) * 100,
                          "pass": abs(d_sim - d_real) <= EFFECT_MARGIN}
    for phase in PHASES:
        cohort = lambda m, p=phase: m["cohorts"][p]["mean_ms"]
        d_real, d_sim = paired_delta(real, "predictive", seeds, cohort), paired_delta(sim, "predictive", seeds, cohort)
        out["V2"][phase] = {"real": d_real, "sim": d_sim, "pass": (d_real < 0) == (d_sim < 0)}
    orders = []
    for seed in seeds:
        orders.append({"seed": seed, "real_holds": groups_in_order(real, seed), "sim_holds": groups_in_order(sim, seed),
                       "real": ranking(real, seed), "sim": ranking(sim, seed)})
    out["V3"] = {"seeds_with_group_order": sum(o["sim_holds"] for o in orders),
                 "real_seeds_with_group_order": sum(o["real_holds"] for o in orders), "orders": orders}
    for arm in ARMS:
        out["V4"][arm] = statistics.mean(sim[arm][s]["mean_latency_ms"] / real[arm][s]["mean_latency_ms"] for s in seeds)
    short = lambda m: m["short_p50_ms"]
    for arm in EFFECT_ARMS:
        out["V5"][arm] = {"real": paired_delta(real, arm, seeds, short), "sim": paired_delta(sim, arm, seeds, short)}
    out["gate"] = {"V1": all(v["pass"] for v in out["V1"].values()), "V2": all(v["pass"] for v in out["V2"].values()),
                   "V3": out["V3"]["seeds_with_group_order"] >= math.ceil(SHARE_SEEDS_SAME_ORDER * len(seeds))}
    out["gate"]["passed"] = all(out["gate"].values())
    return out


def render(results: dict) -> str:
    pct = lambda v: f"{v * 100:+.1f}%"
    lines = ["# Simulator validation (v4-simvalidation-1)", ""]
    for name, r in results.items():
        g = r["gate"]
        lines += [f"## Measured set: {name}  (seeds {', '.join(r['seeds'])})", "",
                  f"**Gate {'PASSED' if g['passed'] else 'FAILED'}**: V1 {g['V1']}, V2 {g['V2']}, V3 {g['V3']}", "",
                  "### V1 effect against FIFO, mean latency", "", "| Arm | Real | Simulator | Gap (points) | Pass |", "|---|---|---|---|---|"]
        lines += [f"| {a} | {pct(v['real'])} | {pct(v['sim'])} | {v['gap_points']:.1f} | {v['pass']} |" for a, v in r["V1"].items()]
        lines += ["", "### V2 predictive against FIFO, by phase", "", "| Phase | Real | Simulator | Same sign |", "|---|---|---|---|"]
        lines += [f"| {p} | {pct(v['real'])} | {pct(v['sim'])} | {v['pass']} |" for p, v in r["V2"].items()]
        lines += ["", f"### V3 group order {GROUPS}: holds in {r['V3']['seeds_with_group_order']} of {len(r['seeds'])} simulated seeds "
                  f"({r['V3']['real_seeds_with_group_order']} real)", ""]
        lines += [f"- seed {o['seed']}: real {' < '.join(o['real'])} (groups {o['real_holds']}); "
                  f"sim {' < '.join(o['sim'])} (groups {o['sim_holds']})" for o in r["V3"]["orders"]]
        lines += ["", "### V4 absolute mean latency, simulator / real", "", "| Arm | Ratio |", "|---|---|"]
        lines += [f"| {a} | {v:.2f} |" for a, v in r["V4"].items()]
        lines += ["", "### V5 short-class P50 against FIFO", "", "| Arm | Real | Simulator |", "|---|---|---|"]
        lines += [f"| {a} | {pct(v['real'])} | {pct(v['sim'])} |" for a, v in r["V5"].items()]
        lines.append("")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sim", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True, help="scratch directory for traces and simulator output")
    parser.add_argument("--out", type=Path, required=True, help="new directory for summary.json and report.md")
    parser.add_argument("--set", action="append", required=True, metavar="NAME=DIR")
    parser.add_argument("--config", type=Path, default=Path(__file__).parent / "configs" / "v2-prodflow.json")
    args = parser.parse_args()
    if args.out.exists():
        raise SystemExit(f"refusing to overwrite {args.out}")
    config = json.loads(args.config.read_text())
    args.work.mkdir(parents=True, exist_ok=True)
    results = {}
    for item in args.set:
        name, directory = item.split("=", 1)
        root = Path(directory).expanduser()
        seeds = sorted(p.name.removeprefix("seed-") for p in root.glob("seed-*"))
        real, sim = {a: {} for a in ARMS}, {a: {} for a in ARMS}
        work = args.work / name
        work.mkdir(exist_ok=True)
        for seed in seeds:
            for arm in ARMS:
                run_dir = root / f"seed-{seed}" / arm
                run = json.loads((run_dir / "run.json").read_text())
                real[arm][seed] = prodflow_report.metrics(run, read_messages(run_dir), config)
                rows = simulate(args.sim, work, run_dir, arm, config)
                sim[arm][seed] = prodflow_report.metrics({**run, "dlq": 0, "submit_errors": 0, "broker": {}}, rows, config)
        results[name] = summarise(real, sim, seeds)
    args.out.mkdir(parents=True)
    (args.out / "summary.json").write_text(json.dumps(results, indent=1))
    (args.out / "report.md").write_text(render(results))
    print(render(results))


if __name__ == "__main__":
    main()
