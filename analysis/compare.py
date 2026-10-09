"""Reproducible delayed online-model comparison and explicit selection decision."""
from __future__ import annotations

import argparse
from collections import defaultdict
import importlib.metadata
import json
import math
from pathlib import Path
import platform
import re
import resource
import statistics
import subprocess
import sys

from .dataset import TemporalDataset, canonical, fingerprint
from .features import FeatureSchema
from .metrics import paired_interval
from .models import CANDIDATES, Candidate
from .policy import DurationPolicy
from .replay import replay


def dump(path: Path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n")


def validate_config(config: dict):
    if config.get("version") != "online-comparison-v1" or config.get("adapter_version") != "numeric-scale-indicator-zero-v1":
        raise ValueError("unknown comparison/adapter version")
    FeatureSchema.from_dict(config["schema"])
    DurationPolicy(config["policy"])
    if (not config["candidates"] or len(set(config["candidates"])) != len(config["candidates"]) or
            not set(config["candidates"]) <= set(CANDIDATES) or
            not {"global_mean", "job_mean"} <= set(config["candidates"])):
        raise ValueError("candidate list needs unique candidates and both arithmetic baselines")
    for key in ("lease_ms", "pending_limit", "dimension_limit", "group_min_labels", "repeats", "minimum_evaluation_labels"):
        if type(config[key]) is not int or config[key] <= 0:
            raise ValueError(f"invalid {key}")
    if not 0 < config["ewma_alpha"] <= 1 or not 0 < config["train_fraction"] < 1:
        raise ValueError("invalid EWMA or temporal fraction")
    for key in ("required_cases", "seeds", "warmup_label_windows", "availability_delay_sensitivity_ms"):
        if not isinstance(config[key], list) or len(config[key]) != len(set(config[key])):
            raise ValueError(f"invalid {key}")
    if (not config["required_cases"] or not config["seeds"] or
            any(type(x) is not int or x < 0 for x in config["seeds"] + config["warmup_label_windows"] +
                config["availability_delay_sensitivity_ms"]) or
            not config["warmup_label_windows"] or config["warmup_label_windows"][0] != 0):
        raise ValueError("invalid seeds/windows")
    for value in config["budgets"].values():
        if type(value) not in (int, float) or not math.isfinite(value) or value <= 0:
            raise ValueError("invalid resource/coverage budget")
    if config["budgets"]["warm_prediction_coverage"] != 1.0:
        raise ValueError("v1 requires complete warm coverage")
    if (type(config["bootstrap_samples"]) is not int or not 100 <= config["bootstrap_samples"] <= 100000 or
            type(config["bootstrap_seed"]) is not int or
            any(type(x) is not int or x <= 0 for x in config["profiling"].values())):
        raise ValueError("invalid profiling/bootstrap parameters")
    # Validate adapter/group semantics without importing optional models.
    baseline = Candidate("job_mean", config)
    baseline.adapter.group({"headers": {config["group_header"]: None}}, config["group_header"])


def run_identity(run: str) -> tuple:
    match = re.fullmatch(r"(.+)-s([0-9]+)-r([0-9]+)-disabled", run)
    if not match:
        raise ValueError("invalid benchmark run name")
    return match[1], int(match[2]), int(match[3])


def selection(reports: list[dict], config: dict) -> dict:
    by = {(row["candidate"], row["case"], row["seed"], row["repeat"]): row for row in reports}
    if len(by) != len(reports):
        raise ValueError("duplicate candidate/run report")
    expected = [(case, seed, repeat) for case in config["required_cases"]
                for seed in config["seeds"] for repeat in range(config["repeats"])]
    candidates = []
    complete = len(config["seeds"]) >= 5
    for name in config["candidates"]:
        failures, comparisons = [], []
        for case in config["required_cases"]:
            case_keys = [key for key in expected if key[0] == case]
            for key in case_keys:
                row = by.get((name, *key))
                warm = row["cohorts"].get("evaluation_warm", {}) if row else {}
                if (not row or warm.get("eligible_labels", 0) < config["minimum_evaluation_labels"] or
                        warm.get("eligible_positive_labels", 0) == 0):
                    failures.append(f"{key}: insufficient evidence")
                    complete = False
                    continue
                if warm["prediction_coverage"] < config["budgets"]["warm_prediction_coverage"]:
                    failures.append(f"{key}: incomplete warm predictions")
                costs = row.get("profiles", [])
                if not costs:
                    complete = False
                    failures.append(f"{key}: resources not measured")
                for costs_row in costs:
                    timing = costs_row["warm_prediction_ms"]
                    if timing.get("count", 0) < config["minimum_evaluation_labels"]:
                        failures.append(f"{key}: prediction timing sample floor")
                    if timing["p99"] is None or timing["p99"] > config["budgets"]["prediction_p99_ms"]:
                        failures.append(f"{key}: prediction P99 budget")
                    if costs_row["peak_model_state"]["accounted_state_bytes"] > config["budgets"]["model_state_bytes"]:
                        failures.append(f"{key}: model-state budget")
            for metric in ("mae_ms", "severe_underestimate_rate"):
                for baseline in ("global_mean", "job_mean"):
                    deltas = []
                    for seed in config["seeds"]:
                        repeat_deltas = []
                        for repeat in range(config["repeats"]):
                            lhs, rhs = by.get((name, case, seed, repeat)), by.get((baseline, case, seed, repeat))
                            left = lhs["cohorts"].get("evaluation_warm", {}) if lhs else {}
                            right = rhs["cohorts"].get("evaluation_warm", {}) if rhs else {}
                            if not lhs or not rhs or left.get("eligible_labels") != right.get("eligible_labels"):
                                complete = False
                                break
                            if (left.get(metric) is None or right.get(metric) is None or
                                    left.get("scored_predictions") != right.get("scored_predictions")):
                                break
                            repeat_deltas.append(left[metric] - right[metric])
                        if len(repeat_deltas) == config["repeats"]:
                            deltas.append(statistics.mean(repeat_deltas))
                    interval = paired_interval(deltas, config["bootstrap_samples"], config["bootstrap_seed"])
                    comparison = {"case": case, "metric": metric, "baseline": baseline, **interval}
                    comparisons.append(comparison)
                    if len(deltas) != len(config["seeds"]):
                        failures.append(f"{case}/{metric}/{baseline}: unmatched population")
                    elif statistics.mean(deltas) > 0:
                        failures.append(f"{case}/{metric}: worse than {baseline}")
        candidates.append({"candidate": name, "passes": not failures, "failures": failures, "comparisons": comparisons})
    winner = next((r["candidate"] for r in candidates if r["passes"]), None) if complete else None
    return {"status": "incomplete_evidence" if not complete else "selected" if winner else "no_qualifier",
            "selected_candidate": winner, "candidates": candidates, "selection_rule": config["selection"],
            "predictive_activation": False}


def compare(dataset_path: Path, config_path: Path, output: Path, profile: bool = True) -> dict:
    config = json.loads(config_path.read_text())
    validate_config(config)
    if output.exists():
        raise ValueError("comparison output must be fresh")
    with TemporalDataset(dataset_path, config) as dataset:
        # Instantiate before creating outputs so a missing optional dependency is a clear error.
        for name in config["candidates"]:
            Candidate(name, config)
        output.mkdir(parents=True)
        dependencies = {}
        for line in (Path(__file__).parent / "requirements-models.txt").read_text().splitlines():
            if "==" in line:
                name, required = line.split("==")
                try:
                    installed = importlib.metadata.version(name)
                except importlib.metadata.PackageNotFoundError:
                    installed = None
                dependencies[name] = installed
                if profile and any(n.startswith(("linear_", "tree_", "adaptive_")) for n in config["candidates"]) and installed != required:
                    raise ValueError(f"pinned dependency mismatch: {name}")
        manifest = {"artifact_version": 1, "configuration": config, "config_sha256": fingerprint(config),
                    "dataset_sha256": dataset.fingerprint, "dataset_files": dataset.hashes,
                    "schema_sha256": fingerprint(config["schema"]), "dataset_manifest": dataset.manifest,
                    "python": sys.version, "platform": platform.platform(), "dependencies": dependencies,
                    "model_memory_method": "cycle-safe Python deep size plus serialized native-state proxy; not a hard allocator/RSS bound",
                    "availability": dataset.manifest["availability"], "audit": dict(dataset.audit)}
        root = Path(__file__).resolve().parents[1]
        manifest["revision"] = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
        manifest["source_sha256"] = {str(path.relative_to(root)): __import__("hashlib").sha256(path.read_bytes()).hexdigest()
                                     for path in sorted((root / "analysis").glob("*.py"))}
        dump(output / "manifest.json", manifest)
        reports, sensitivity = [], []
        collection = json.loads((root / config["collection_config"]).read_text())
        with (output / "predictions.jsonl").open("w") as predictions:
            for run in sorted(dataset.sources):
                case, seed, repeat = run_identity(run)
                spec = next((r for r in collection["cases"] if r["name"] == case), None)
                settings = dict(config)
                if spec and "shift_index" in spec:
                    # Exact integer arrival schedule from the frozen generator.
                    settings["shift_time_ns"] = round(spec["shift_index"] * 1e6 / spec["rate_per_s"]) * 1000
                for name in config["candidates"]:
                    def emit(row):
                        predictions.write(canonical({"candidate": name, **row}) + "\n")
                    scope = dataset.fingerprint + ":" + run
                    report = replay(dataset.events(run), Candidate(name, config, scope), settings, emit=emit)
                    profiles = []
                    if profile:
                        for _ in range(config["profiling"]["repeats"]):
                            measured = replay(dataset.events(run), Candidate(name, config, scope), settings, profile=True)
                            if measured["cohorts"] != report["cohorts"]:
                                raise ValueError("profiling changed numeric replay")
                            profiles.append({**measured["resources"], "peak_pending_bytes": measured["peak_pending_bytes"],
                                             "feedback_identity_bytes": measured["feedback_identity_bytes"]})
                    reports.append({"candidate": name, "run": run, "case": case, "seed": seed, "repeat": repeat,
                                    "profiles": profiles, **report})
                    for delay in config["availability_delay_sensitivity_ms"]:
                        delayed = replay(dataset.events(run, delay * 1000000), Candidate(name, config, scope + f":delay-{delay}"), settings)
                        sensitivity.append({"candidate": name, "run": run, "extra_delay_ms": delay,
                                            "cohorts": delayed["cohorts"], "peak_pending": delayed["peak_pending"]})
                print(f"Compared {run}", flush=True)
        with (output / "runs.jsonl").open("w") as stream:
            for report in reports:
                stream.write(canonical(report) + "\n")
        with (output / "delay-sensitivity.jsonl").open("w") as stream:
            for report in sensitivity:
                stream.write(canonical(report) + "\n")
        decision = selection(reports, config)
        dump(output / "summary.json", summarize(reports))
        decision.update(config_sha256=manifest["config_sha256"], dataset_sha256=dataset.fingerprint,
                        schema_version=config["schema"]["version"], policy_version=config["policy"]["version"],
                        model_version=None,  # No production trained checkpoint is produced by this comparison.
                        model_configuration_version=Candidate(decision["selected_candidate"], config).configuration_version
                        if decision["selected_candidate"] else None,
                        parameters=config, peak_process_rss_kib=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
        dump(output / "decision.json", decision)
        return decision


def summarize(reports: list[dict]) -> dict:
    cells = defaultdict(list)
    for row in reports:
        cells[row["candidate"], row["case"]].append(row)
    summaries = []
    for (candidate, case), values in sorted(cells.items()):
        warm = [row["cohorts"].get("evaluation_warm", {}) for row in values]
        resources = [profile for row in values for profile in row["profiles"]]
        def mean_metric(metric):
            measured = [row[metric] for row in warm if row.get(metric) is not None]
            return statistics.mean(measured) if measured else None
        summaries.append({"candidate": candidate, "case": case, "runs": len(values),
            "eligible_warm_labels": sum(row.get("eligible_labels", 0) for row in warm),
            "minimum_prediction_coverage": min((row.get("prediction_coverage", 0) or 0 for row in warm), default=0),
            "mean_run_mae_ms": mean_metric("mae_ms"), "mean_run_rmse_ms": mean_metric("rmse_ms"),
            "mean_run_severe_underestimate_rate": mean_metric("severe_underestimate_rate"),
            "mean_run_bucket_accuracy": mean_metric("bucket_accuracy"),
            "maximum_profile_prediction_p99_ms": max((p["warm_prediction_ms"]["p99"] for p in resources
                if p["warm_prediction_ms"]["p99"] is not None), default=None),
            "maximum_accounted_model_bytes": max((p["peak_model_state"]["accounted_state_bytes"] for p in resources), default=None),
            "maximum_pending": max(row["peak_pending"] for row in values),
            "true_bucket_support": [sum(sum(row.get("bucket_confusion", [[0]])[i]) for row in warm)
                                    for i in range(len(warm[0].get("bucket_confusion", [])))]})
    return {"artifact_version": 1, "aggregation": "equal-weight mean of run metrics; repeats/seed pairing is in decision.json",
            "cells": summaries}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--no-profile", action="store_true", help="correctness smoke only; selection stays incomplete")
    args = parser.parse_args()
    result = compare(args.dataset, args.config, args.output, not args.no_profile)
    print(result["status"], result["selected_candidate"])


if __name__ == "__main__":
    main()
