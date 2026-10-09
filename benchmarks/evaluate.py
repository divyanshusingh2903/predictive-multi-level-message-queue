"""Paired open-loop real-broker trials, censoring-aware metrics, and uncertainty."""
from __future__ import annotations

import argparse
import hashlib
from collections import Counter, defaultdict
import csv
import json
import platform
import os
import re
from pathlib import Path
import random
import statistics
import subprocess
import sys

from .workloads import SCHEMA, canonical_hash, generate, write_trace

POLICIES = ("fifo", "static", "round_robin", "disabled")
DEFAULTS = {"submitters": 8, "drain_ms": 1500, "lease_ms": 1000, "ttl_ms": 0,
            "max_retries": 3, "aging_threshold_ms": 50, "aging_interval_ms": 5,
            "ttl_sweep_ms": 10, "lease_sweep_ms": 10, "levels": 3, "default_priority": 1,
            "tier0": 0, "tier1": 1, "tier2": 2}


def read_tsv(path: Path) -> list[dict]:
    with path.open(encoding="utf8", newline="") as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


def quantiles(values: list[float]) -> dict:
    ordered = sorted(values)
    return {"count": len(values), **{f"p{percent}": ordered[(percent * len(ordered) + 99) // 100 - 1]
            if ordered else None for percent in (50, 95, 99)}}


def metrics(jobs: list[dict], observations: list[dict], submissions: list[dict], runtimes: list[dict],
            summary: dict, warmup: int, starvation_ms: float) -> tuple[dict, list[dict]]:
    cutoff = int(summary["cutoff_ns"])
    start = jobs[warmup]["arrival_us"] * 1000
    window = (cutoff - start) / 1e9
    if window <= 0 or starvation_ms <= 0:
        raise ValueError("invalid measurement window or fairness threshold")
    by_sequence = defaultdict(list)
    for record in observations:
        item = {key: (int(value) if key in {"sequence", "attempt", "arrival_ns", "time_ns", "published_ns", "retries"} else value)
                for key, value in record.items()}
        if not 0 <= item["sequence"] < len(jobs) or item["time_ns"] > cutoff:
            raise ValueError("invalid observation sequence/time")
        by_sequence[item["sequence"]].append(item)
    submission_map = {int(row["sequence"]): row for row in submissions}
    outcome_counts, class_counts = Counter(), defaultdict(Counter)
    waits, terminal, success, ages, lateness, submit_latency = [], [], [], [], [], []
    records, inconsistencies = [], []
    for job in jobs[warmup:]:
        seq, job_class = job["sequence"], job["job"]
        events = sorted(by_sequence[seq], key=lambda row: (row["time_ns"], row["attempt"]))
        submitted = submission_map[seq]
        ingresses = [row for row in events if row["kind"] == "ingress"]
        dispatch = [row for row in events if row["kind"] == "dispatch"]
        terminals = [row for row in events if row["kind"] in {"ack", "ttl", "max_retries"}]
        if len(ingresses) > 1 or len(terminals) > 1:
            inconsistencies.append(seq)
        # An ingress proves acceptance even if the Submit response was lost/late.
        accepted = bool(ingresses) or (int(submitted["status"]) == 0 and int(submitted["end_ns"]) <= cutoff)
        offered_ns = job["arrival_us"] * 1000
        arrival = events[0]["arrival_ns"] if events else None
        outcome = terminals[0]["kind"] if terminals else "unfinished" if accepted else (
            "missed" if int(submitted["status"]) == -2 else "unconfirmed" if
            int(submitted["end_ns"]) > cutoff or int(submitted["status"]) in {0, 4, 14, -1} else "rejected")
        outcome_counts[outcome] += 1
        outcome_counts["accepted"] += accepted
        if int(submitted["start_ns"]) > 0:
            lateness.append(max(0, int(submitted["start_ns"]) - offered_ns) / 1e6)
            if int(submitted["end_ns"]) <= cutoff:
                submit_latency.append((int(submitted["end_ns"]) - int(submitted["start_ns"])) / 1e6)
        wait = (dispatch[0]["time_ns"] - arrival) / 1e6 if dispatch and arrival is not None else None
        age_end = terminals[0]["time_ns"] if terminals else cutoff
        age = max(0, age_end - arrival) / 1e6 if accepted and arrival is not None else None
        class_counts[job_class]["offered"] += 1
        if accepted:
            class_counts[job_class]["accepted"] += 1
            if wait is not None:
                waits.append(wait)
                class_counts[job_class]["dispatched"] += 1
                class_counts[job_class]["longest_wait_ms"] = max(wait, class_counts[job_class]["longest_wait_ms"])
            elif age is not None:
                class_counts[job_class]["never_dispatched"] += 1
                class_counts[job_class]["longest_censored_age_ms"] = max(age, class_counts[job_class]["longest_censored_age_ms"])
                class_counts[job_class]["younger_censored"] += age <= starvation_ms
            if (wait if wait is not None else age or 0) > starvation_ms:
                class_counts[job_class]["starved"] += 1
            if outcome == "unfinished" and age is not None:
                ages.append(age)
        latency = (terminals[0]["time_ns"] - arrival) / 1e6 if terminals and arrival is not None else None
        if latency is not None:
            terminal.append(latency)
            if outcome == "ack": success.append(latency)
        records.append({"sequence": seq, "message_id": submitted["message_id"] or
                        (events[0]["message_id"] if events else None), "job": job_class,
                        "accepted": accepted, "outcome": outcome, "arrival_ns": arrival,
                        "first_dispatch_wait_ms": wait, "terminal_latency_ms": latency,
                        "censored_age_ms": age if outcome == "unfinished" else None,
                        "attempts": len(dispatch), "events": events})
    for name in ("class0", "class1", "class2"):
        row = class_counts[name]
        for key in ("offered", "accepted", "dispatched", "never_dispatched", "younger_censored", "starved"):
            row.setdefault(key, 0)
        row["starvation_rate"] = row["starved"] / row["accepted"] if row["accepted"] else None
        row.setdefault("longest_wait_ms", None)
        row.setdefault("longest_censored_age_ms", None)
    offered = len(jobs) - warmup
    terminal_count = sum(outcome_counts[name] for name in ("ack", "ttl", "max_retries"))
    settled_times = [event["time_ns"] for row in records for event in row["events"]
                     if event["kind"] in {"ack", "ttl", "max_retries"}]
    end = max(settled_times, default=start)
    measured_runtimes = [(int(row["end_ns"]) - int(row["start_ns"])) / 1e6 for row in runtimes
                         if int(row["sequence"]) >= warmup and int(row["end_ns"]) <= cutoff]
    telemetry_loss = sum(int(summary.get(key, 0)) for key in ("feedback_drops", "feedback_uncertain", "feedback_retention_records"))
    all_counts = Counter()
    all_terminal_times = []
    for job in jobs:
        seq = job["sequence"]
        events, submitted = by_sequence[seq], submission_map[seq]
        is_accepted = any(row["kind"] == "ingress" for row in events) or (
            int(submitted["status"]) == 0 and int(submitted["end_ns"]) <= cutoff)
        completed = [row for row in events if row["kind"] in {"ack", "ttl", "max_retries"}]
        all_counts["accepted"] += is_accepted
        all_counts["terminal"] += bool(completed)
        all_counts["ack"] += bool(completed) and completed[0]["kind"] == "ack"
        if completed: all_terminal_times.append(completed[0]["time_ns"])
        if not is_accepted:
            all_counts["not_accepted"] += 1
            all_counts["missed"] += int(submitted["status"]) == -2
            all_counts["unconfirmed"] += int(submitted["status"]) in {0, -1, 4, 14} or int(submitted["end_ns"]) > cutoff
    full_drain = all_counts["accepted"] == all_counts["terminal"] and not all_counts["unconfirmed"]
    result = {"offered": offered, "counts": dict(outcome_counts),
              "rates_per_offered": {key: value / offered for key, value in outcome_counts.items()},
              "rates_per_accepted": {key: outcome_counts[key] / outcome_counts["accepted"] if outcome_counts["accepted"] else None
                                     for key in ("ack", "ttl", "max_retries", "unfinished")},
              "first_dispatch_wait_ms": quantiles(waits), "handler_runtime_ms": quantiles(measured_runtimes),
              "terminal_latency_ms": quantiles(terminal), "success_latency_ms": quantiles(success),
              "unfinished_age_ms": quantiles(ages), "submit_lateness_ms": quantiles(lateness),
              "submit_latency_ms": quantiles(submit_latency), "submission_late_over_1ms": sum(value > 1 for value in lateness),
              "maximum_submission_lateness_ms": max(lateness, default=None), "measurement_window_s": window,
              "success_per_s": outcome_counts["ack"] / window, "terminal_per_s": terminal_count / window,
              "full_drain": full_drain, "full_drain_time_s": (max(all_terminal_times, default=0) - jobs[0]["arrival_us"] * 1000) / 1e9 if full_drain else None,
              "measured_cohort_terminal_time_s": (end - start) / 1e9,
              "all_messages_counts": {"offered": len(jobs), **dict(all_counts),
                                      "unfinished": all_counts["accepted"] - all_counts["terminal"]},
              "classes": dict(class_counts), "starvation_threshold_ms": starvation_ms,
              "telemetry_loss": telemetry_loss, "observation_drops": int(summary["observation_drops"]),
              "inconsistent_sequences": inconsistencies, "queue_snapshot": int(summary["queued_at_cutoff"]),
              "in_flight_snapshot": int(summary["in_flight_at_cutoff"]),
        "valid": not (telemetry_loss or int(summary["observation_drops"]) or inconsistencies or
                            all_counts["not_accepted"] or int(summary["pull_errors"]))}
    return result, records


def paired_interval(deltas: list[float], samples: int, seed: int) -> dict:
    if not deltas:
        return {"mean": None, "ci95": None, "paired_seeds": 0, "evidence_status": "unevaluable"}
    rng = random.Random(seed)
    means = sorted(statistics.mean(rng.choices(deltas, k=len(deltas))) for _ in range(samples))
    return {"mean": statistics.mean(deltas), "ci95": [means[(samples + 39) // 40 - 1],
                                                         means[(39 * samples + 39) // 40 - 1]],
            "paired_seeds": len(deltas), "evidence_status": "minimum-five-pairs-met" if len(deltas) >= 5 else "insufficient-paired-seeds"}


def aggregate(runs: list[dict], samples: int, bootstrap_seed: int) -> list[dict]:
    cells = defaultdict(list)
    for run in runs: cells[run["case"]].append(run)
    output = []
    for case, trials in cells.items():
        for comparator in ("fifo", "round_robin", "disabled"):
            for metric in ("success_per_s", "terminal_latency_ms.p50", "terminal_latency_ms.p95", "terminal_latency_ms.p99",
                           *(f"classes.class{i}.{field}" for i in range(3) for field in ("longest_wait_ms", "starvation_rate"))):
                grouped = defaultdict(lambda: defaultdict(dict))
                for run in trials:
                    if run["policy"] not in ("static", comparator) or not run["metrics"]["valid"]:
                        continue
                    value = run["metrics"]
                    for key in metric.split("."): value = value.get(key) if isinstance(value, dict) else None
                    if value is not None: grouped[run["seed"]][run["policy"]][run.get("repeat", 0)] = value
                deltas, paired_ratios, zero_denominators = [], [], 0
                for values in grouped.values():
                    repeats = set(values["static"]) & set(values[comparator])
                    if repeats:
                        deltas.append(statistics.mean(values["static"][trial] - values[comparator][trial] for trial in repeats))
                        ratios = [values["static"][trial] / values[comparator][trial] - 1 for trial in repeats
                                  if values[comparator][trial] != 0]
                        zero_denominators += sum(values[comparator][trial] == 0 for trial in repeats)
                        if len(ratios) == len(repeats): paired_ratios.append(statistics.mean(ratios))
                output.append({"case": case, "comparison": f"static-minus-{comparator}", "metric": metric,
                               **paired_interval(deltas, samples, bootstrap_seed),
                               "relative_delta": paired_interval(paired_ratios, samples, bootstrap_seed),
                               "zero_denominator_trials": zero_denominators})
    return output


def dump(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf8")


def median_or_none(values) -> float | None:
    rows = list(values)
    return statistics.median(rows) if rows else None


def run_experiment(config_path: Path, runner: Path, output: Path) -> dict:
    config = json.loads(config_path.read_text())
    if config["version"] != "baseline-experiment-v1" or not config["seeds"] or not config["cases"]:
        raise ValueError("invalid experiment manifest")
    if (type(config.get("repeats", 1)) is not int or not 1 <= config.get("repeats", 1) <= 100 or
            not 100 <= config.get("bootstrap_samples", 2000) <= 100000 or
            len(set(config["seeds"])) != len(config["seeds"]) or
            not set(config.get("policies", POLICIES)) <= set(POLICIES)):
        raise ValueError("invalid trial/uncertainty settings")
    if output.exists(): raise ValueError("output must be a fresh directory")
    if not runner.is_file(): raise ValueError("runner executable not found")
    # Check every workload/config before producing artifacts or launching a broker.
    for case in config["cases"]:
        generate(case, config["seeds"][0])
        if (not re.fullmatch(r"[a-z0-9-]+", case["name"]) or
                not 0 <= case.get("warmup", 0) < case["count"] or type(case["workers"]) is not int or not 1 <= case["workers"] <= 256):
            raise ValueError("invalid warmup/workers")
        params = {**DEFAULTS, **config.get("broker", {}), **case.get("broker", {})}
        if set(params) != set(DEFAULTS) or any(type(value) is not int or value < 0 for value in params.values()):
            raise ValueError("unknown/invalid broker parameters")
        if (not params["drain_ms"] or not params["lease_ms"] or not params["max_retries"] or
                not 1 <= params["levels"] <= 255 or not params["submitters"] or
                params["default_priority"] >= params["levels"] or
                any(params[f"tier{i}"] >= params["levels"] for i in range(3)) or
                bool(params["aging_threshold_ms"]) != bool(params["aging_interval_ms"]) or
                (not params["aging_threshold_ms"] and not case.get("starvation_ms"))):
            raise ValueError("invalid broker/fairness configuration")
    if len({case["name"] for case in config["cases"]}) != len(config["cases"]):
        raise ValueError("duplicate case names")
    output.mkdir(parents=True)
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(["git", "status", "--porcelain"], text=True).strip()
    manifest = {"artifact_version": 1, "config": config, "config_sha256": canonical_hash(config),
                "schema": SCHEMA, "revision": revision, "dirty": bool(dirty),
                "python": sys.version, "platform": platform.platform(), "machine": platform.machine(),
                "processor": platform.processor(), "cpu_count": os.cpu_count(), "runtime_realization": "sleep_us",
                "runner_sha256": hashlib.sha256(runner.read_bytes()).hexdigest(),
                "build_cache": {line.split("=", 1)[0]: line.split("=", 1)[1]
                                for line in (runner.resolve().parents[1] / "CMakeCache.txt").read_text().splitlines()
                                if "=" in line and line.startswith(("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:",
                                     "CMAKE_CXX_FLAGS", "HARBINGER_", "gRPC_DIR:", "Protobuf_DIR:"))}}
    root = Path(__file__).resolve().parents[1]
    runtime_sources = [*root.glob("include/**/*.hpp"), *root.glob("src/**/*.cpp"),
                       *root.glob("benchmarks/*.cpp"), *root.glob("benchmarks/*.py"), *root.glob("analysis/*.py"),
                       root / "CMakeLists.txt", root / "benchmarks/CMakeLists.txt", root / "proto/harbinger.proto"]
    manifest["source_sha256"] = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                                 for path in sorted(runtime_sources)}
    manifest["fixed_broker_defaults"] = {"completion_retention_ms": 60000, "completion_cache_max_entries": 10000,
        "maintenance_batch_size": 256, "max_pull_wait_ms": 5000,
        "feedback": {"buffer_records": 4096, "buffer_bytes": 33554432, "max_event_bytes": 16384,
                     "segment_bytes": 67108864, "retention_bytes": 1073741824, "max_segments": 256,
                     "retention_age_days": 7, "sync_interval_ms": 1000, "shutdown_drain_ms": 1000}}
    if Path("/proc/cpuinfo").exists():
        with Path("/proc/cpuinfo").open() as stream:
            manifest["cpu_model"] = next((line.partition(":")[2].strip() for line in stream if line.startswith("model name")), "unknown")
    if hasattr(os, "sched_getaffinity"): manifest["cpu_affinity"] = sorted(os.sched_getaffinity(0))
    if "budgets" in config:
        budgets = json.loads(Path(config["budgets"]).read_text())
        manifest["budgets"] = budgets
        manifest["budgets_sha256"] = canonical_hash(budgets)
    dump(output / "manifest.json", manifest)
    runs = []
    for case in config["cases"]:
        for seed in config["seeds"]:
            jobs = generate(case, seed)
            for repeat in range(config.get("repeats", 1)):
                policies = list(config.get("policies", POLICIES))
                if config.get("telemetry_off_control", True): policies.append("disabled_off")
                # Declared seeded policy-order randomization limits order/host drift confounding.
                random.Random(f"policy-order-v1:{seed}:{repeat}:{case['name']}").shuffle(policies)
                for policy in policies:
                    directory = output / f"{case['name']}-s{seed}-r{repeat}-{policy}"
                    directory.mkdir()
                    feedback = directory / "feedback"; feedback.mkdir()
                    trace_hash = write_trace(directory / "trace.tsv", jobs)
                    params = {**DEFAULTS, **config.get("broker", {}), **case.get("broker", {}),
                              "policy": policy.removesuffix("_off"), "workers": case["workers"],
                              "feedback": "off" if policy.endswith("_off") else str(feedback.resolve()),
                              "observation_capacity": case.get("observation_capacity", case["count"] * 16)}
                    # Manifest includes every runner parameter; no policy-specific rate/settings.
                    (directory / "runner.conf").write_text("".join(f"{key} {value}\n" for key, value in params.items()))
                    subprocess.run([str(runner.resolve()), str(directory / "trace.tsv"),
                                    str(directory / "runner.conf"), str(directory)], check=True,
                                   timeout=jobs[-1]["arrival_us"] / 1e6 + params["drain_ms"] / 1000 + 90)
                    summary = dict(line.split("\t", 1) for line in (directory / "summary.tsv").read_text().splitlines())
                    threshold = case.get("starvation_ms", 2 * params["levels"] * params["aging_threshold_ms"])
                    measured, records = metrics(jobs, read_tsv(directory / "observations.tsv"),
                        read_tsv(directory / "submissions.tsv"), read_tsv(directory / "runtimes.tsv"),
                        summary, case.get("warmup", 0), threshold)
                    run = {"case": case["name"], "seed": seed, "repeat": repeat, "policy": policy,
                           "directory": directory.name, "trace_sha256": trace_hash, "parameters": params,
                           "summary": summary, "metrics": measured}
                    dump(directory / "run.json", run)
                    with (directory / "messages.jsonl").open("w") as stream:
                        for row in records: stream.write(json.dumps(row, allow_nan=False) + "\n")
                    runs.append(run)
                    print(f"{directory.name}: valid={measured['valid']} success={measured['counts'].get('ack', 0)} "
                          f"unfinished={measured['counts'].get('unfinished', 0)}", flush=True)
    results = {"artifact_version": 1, "config_sha256": manifest["config_sha256"], "runs": runs,
               "uncertainty": aggregate(runs, config.get("bootstrap_samples", 2000), config.get("bootstrap_seed", 42))}
    dump(output / "results.json", results)
    return results


def publish_results(source: Path, output: Path) -> None:
    """Keep every raw trial summary and interval, without retaining unbounded feedback logs."""
    if output.exists(): raise ValueError("publication directory must be fresh")
    output.mkdir(parents=True)
    results = json.loads((source / "results.json").read_text())
    manifest = json.loads((source / "manifest.json").read_text())
    dump(output / "manifest.json", manifest)
    dump(output / "uncertainty.json", results["uncertainty"])
    with (output / "trials.jsonl").open("w") as stream:
        for run in results["runs"]:
            row = {key: value for key, value in run.items() if key not in ("parameters", "directory")}
            row["parameters"] = {**run["parameters"], "feedback": "off" if run["policy"].endswith("_off") else "dedicated-per-run"}
            directory = source / run["directory"]
            row["raw_sha256"] = {name: hashlib.sha256((directory / name).read_bytes()).hexdigest()
                                  for name in ("trace.tsv", "observations.tsv", "submissions.tsv", "runtimes.tsv")}
            stream.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")
    cells = defaultdict(list)
    for run in results["runs"]: cells[(run["case"], run["policy"])].append(run)
    summaries = []
    for (case, policy), runs in sorted(cells.items()):
        valid = [run["metrics"] for run in runs if run["metrics"]["valid"]]
        summaries.append({"case": case, "policy": policy, "trials": len(runs), "valid_trials": len(valid),
            "fully_drained_trials": sum(run["metrics"]["full_drain"] for run in runs),
            "telemetry_loss": sum(run["metrics"]["telemetry_loss"] for run in runs),
            "median_p95_terminal_ms": median_or_none(row["terminal_latency_ms"]["p95"] for row in valid
                                                       if row["terminal_latency_ms"]["p95"] is not None),
            "median_success_per_s": median_or_none(row["success_per_s"] for row in valid),
            "median_full_drain_capacity_per_s": median_or_none(row.get("all_messages_counts", row["counts"]).get("ack", 0) / row["full_drain_time_s"]
                for row in valid if row["full_drain"] and row["full_drain_time_s"])})
    dump(output / "summary.json", {"artifact_version": 1, "cells": summaries,
         "total_trials": len(results["runs"]), "valid_trials": sum(run["metrics"]["valid"] for run in results["runs"]),
         "configuration_sha256": manifest["config_sha256"]})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--runner", type=Path)
    parser.add_argument("--publish-from", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.publish_from:
        if args.config or args.runner: parser.error("publication accepts only --publish-from and --output")
        publish_results(args.publish_from, args.output)
    else:
        if not args.config or not args.runner: parser.error("experiments require --config and --runner")
        run_experiment(args.config, args.runner, args.output)


if __name__ == "__main__": main()
