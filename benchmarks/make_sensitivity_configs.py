"""Generate the v4-sensitivity-1 configs from a v3-traces-1 config, so that none is written by hand.

Usage: python -m benchmarks.make_sensitivity_configs BASE_CONFIG OUT_DIR [--freeze-date YYYY-MM-DD]

Writes, per trace, into OUT_DIR:
  - `<trace>/<variant id>.json` for each of the 18 one-at-a-time variants of Part A (one default and 17 off-default);
  - `<trace>/w28.json`, Part B: the same arms and loads as the base, with the evaluation span cut into 28 windows;
  - `<trace>/manifest.json`, which `python -m benchmarks.sim_report sensitivity` reads.

The grid is the table of docs/v4-sensitivity-preregistration.md. Decisions not stated there, flagged in that file:
  - the aging interval stays one tenth of the threshold (5000/500 ms -> 1000/100 and 30000/3000);
  - the level weights for 2 and 4 levels are (8, 1) and (8, 4, 2, 1); the default tier stays 1;
  - the metric classes (`class_bounds_ms`) stay fixed whatever the number of levels, so variants stay comparable.
"""
from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path

INF_SPREAD = 1e18  # JSON has no infinity; no key's spread ratio reaches this, so the check never triggers
LOAD = 0.8
ARMS = ["fifo", "predictive_warm", "predictive_warm+weights+pausing"]
GATE_CRITERIA = ["A_P1_mean_latency_vs_fifo", "A_G1_all_p99_vs_fifo", "A_G2_long_max_wait_vs_fifo",
                 "B_P1_mean_latency_vs_fifo", "B_G1_all_p99_vs_fifo", "B_G2_long_max_wait_vs_fifo"]

# parameter -> (grid in order, default). Values are what the paper prints; `apply` turns them into config fields.
GRID: dict[str, tuple[list, object]] = {
    "n_min": ([5, 20, 100], 20),
    "kappa": ([4, 16, 64, "inf"], 16),
    "decay": ([0.98, 0.995, 0.999, 1.0], 0.995),
    "tau": ["5min", "30min", "2h", "off"],
}
GRID["tau"] = (["5min", "30min", "2h", "off"], "30min")
GRID["aging_threshold"] = (["1s", "5s", "30s"], "5s")
GRID["weights"] = (["8-3-1", "4-2-1", "16-4-1"], "8-3-1")
GRID["levels"] = ([2, 3, 4], 3)

TAU_MS = {"5min": 300_000, "30min": 1_800_000, "2h": 7_200_000, "off": 0}
AGING_MS = {"1s": 1_000, "5s": 5_000, "30s": 30_000}
LEVEL_WEIGHTS = {2: [8, 1], 3: [8, 3, 1], 4: [8, 4, 2, 1]}


def apply(config: dict, parameter: str, value) -> None:
    predictor = config.setdefault("predictor", {})
    if parameter == "n_min":
        predictor["min_samples"] = value
    elif parameter == "kappa":
        predictor["max_spread_ratio"] = INF_SPREAD if value == "inf" else value
    elif parameter == "decay":
        predictor["decay"] = value
    elif parameter == "tau":
        predictor["stale_after_ms"] = TAU_MS[value]
    elif parameter == "aging_threshold":
        config["aging"] = {"threshold_ms": AGING_MS[value], "interval_ms": AGING_MS[value] // 10}
    elif parameter == "weights":
        config["level_weights"] = [int(w) for w in value.split("-")]
    elif parameter == "levels":
        config["levels"] = value
        config["level_weights"] = LEVEL_WEIGHTS[value]
    else:
        raise ValueError(parameter)


def slug(parameter: str, value) -> str:
    return f"{parameter}-{value}"


def variants() -> list[dict]:
    """The default first, then every off-default value in grid order. `step` is the distance in the grid from the default."""
    out = [{"id": "default", "parameter": "default", "value": "all defaults", "default": True, "step": 0}]
    for parameter, (grid, default) in GRID.items():
        at = grid.index(default)
        for i, value in enumerate(grid):
            if value != default:
                out.append({"id": slug(parameter, value), "parameter": parameter, "value": value, "default": False,
                            "step": abs(i - at)})
    return out


def variant_config(base: dict, variant: dict, freeze_date: str | None) -> dict:
    config = copy.deepcopy(base)
    config["version"] = "v4-sensitivity-1"
    config["frozen_on"] = freeze_date
    config["preregistration"] = "docs/v4-sensitivity-preregistration.md"
    config["utilizations"] = [LOAD]
    config["arms"] = ARMS
    config["criteria"] = {name: base["criteria"][name] for name in GATE_CRITERIA}
    config["gates"] = {gate: [n for n in GATE_CRITERIA if n.startswith(gate[0] + "_")] for gate in base["gates"]}
    if not variant["default"]:
        apply(config, variant["parameter"], variant["value"])
    config["variant"] = {k: variant[k] for k in ("id", "parameter", "value")}
    return config


def window_config(base: dict, freeze_date: str | None) -> dict:
    config = copy.deepcopy(base)
    config["version"] = "v4-sensitivity-1-w28"
    config["frozen_on"] = freeze_date
    config["preregistration"] = "docs/v4-sensitivity-preregistration.md"
    config["evaluation_days"] = 28  # the field counts windows
    return config


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("base", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--freeze-date")
    args = parser.parse_args()
    base = json.loads(args.base.read_text())
    trace = args.base.stem.removeprefix("v3-traces-")
    out = args.out / trace
    out.mkdir(parents=True, exist_ok=True)
    listing = variants()
    for variant in listing:
        (out / f"{variant['id']}.json").write_text(json.dumps(variant_config(base, variant, args.freeze_date), indent=1) + "\n")
    (out / "w28.json").write_text(json.dumps(window_config(base, args.freeze_date), indent=1) + "\n")
    manifest = {"version": "v4-sensitivity-1", "trace": trace, "load": LOAD, "base_config": str(args.base), "variants": listing}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    print(f"{trace}: {len(listing)} variants + w28 -> {out}")


if __name__ == "__main__":
    main()
