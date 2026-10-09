"""Audit completed collection and replace lossy trials by a frozen coverage rule."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import tempfile

from benchmarks.evaluate import run_experiment
from benchmarks.export_feedback import export_run
from .compare import dump, validate_config
from .dataset import TemporalDataset, fingerprint

HEALTH = ("feedback_drops", "feedback_pending_bytes", "feedback_pending_records", "feedback_recovery_failures",
          "feedback_retention_records", "feedback_sync_failures", "feedback_uncertain", "feedback_writer_failures",
          "observation_drops", "pull_errors", "in_flight_at_cutoff", "queued_at_cutoff")


def coverage(directory: Path, count: int, config: dict) -> list[str]:
    """No predictor quality enters this admission decision."""
    metadata = json.loads((directory / "run.json").read_text())
    metrics, summary = metadata["metrics"], metadata["summary"]
    failures = []
    expected_name = f"{metadata['case']}-s{metadata['seed']}-r{metadata['repeat']}-disabled"
    if metadata["directory"] != directory.name or directory.name != expected_name:
        failures.append("run identity differs from collection metadata")
    if metadata["policy"] != "disabled" or metrics["valid"] is not True or metrics["full_drain"] is not True:
        failures.append("not a valid fully drained static trial")
    counts = metrics["all_messages_counts"]
    if any(counts.get(key, 0) != count for key in ("offered", "accepted", "ack", "terminal")) or counts.get("unfinished", 0):
        failures.append("offered/accepted/successful/terminal coverage")
    if any(int(summary.get(key, -1)) != 0 for key in HEALTH):
        failures.append("writer/observation/consumer health")
    if int(summary["feedback_written"]) != 2 * count or int(summary["feedback_synced"]) != 2 * count:
        failures.append("incomplete sealed/synced feedback")
    if not failures:
        with tempfile.TemporaryDirectory(prefix="harbinger-trial-check-") as scratch:
            exported = Path(scratch) / "dataset"
            try:
                export_run(directory, exported, config["train_fraction"])
                with TemporalDataset(exported, config):
                    pass
            except ValueError as exc:
                failures.append(str(exc))
    return failures


def prepare(source: Path, runner: Path, config_path: Path, output: Path):
    config = json.loads(config_path.read_text()); validate_config(config)
    collection_path = Path(config["collection_config"])
    collection = json.loads(collection_path.read_text())
    source_manifest = json.loads((source / "manifest.json").read_text())
    if fingerprint(source_manifest["config"]) != fingerprint(collection):
        raise ValueError("source collection differs from frozen configuration")
    result = json.loads((source / "results.json").read_text())
    by = {(r["case"], r["seed"], r["repeat"]): r for r in result["runs"]}
    expected = {(c["name"], seed, repeat) for c in collection["cases"] for seed in collection["seeds"]
                for repeat in range(collection["repeats"])}
    if len(by) != len(result["runs"]) or set(by) != expected:
        raise ValueError("incomplete/duplicate collection matrix")
    if output.exists():
        raise ValueError("preparation output must be fresh")
    output.mkdir(parents=True)
    accepted = output / "accepted"; accepted.mkdir()
    dump(accepted / "manifest.json", source_manifest)
    audit = {"artifact_version": 1, "initial_source": str(source.resolve()), "config_sha256": fingerprint(config),
             "collection_config_sha256": fingerprint(collection), "trials": [], "selected": [], "status": "preparing"}
    for key in sorted(expected):
        case, seed, repeat = key
        spec = next(c for c in collection["cases"] if c["name"] == case)
        directory = source / by[key]["directory"]
        failure = coverage(directory, spec["count"], config)
        audit["trials"].append({"case": case, "seed": seed, "repeat": repeat, "attempt": 0,
                                "directory": str(directory.resolve()), "failures": failure})
        for attempt in range(1, config["maximum_replacements"] + 1):
            if not failure:
                break
            # Existing orchestration indexes repeats from zero. Retain the
            # additional unused repeat too; select only the matching repeat.
            replacement_config = {**collection, "cases": [spec], "seeds": [seed], "repeats": repeat + 1}
            replacement_root = output / f"replacement-{case}-s{seed}-r{repeat}-a{attempt}"
            replacement_path = output / f"{replacement_root.name}.json"
            dump(replacement_path, replacement_config)
            replacement = run_experiment(replacement_path, runner, replacement_root)
            selected = next(r for r in replacement["runs"] if r["repeat"] == repeat)
            directory = replacement_root / selected["directory"]
            failure = coverage(directory, spec["count"], config)
            audit["trials"].append({"case": case, "seed": seed, "repeat": repeat, "attempt": attempt,
                                    "directory": str(directory.resolve()), "failures": failure})
        if failure:
            audit["status"] = "incomplete_evidence"
            dump(output / "preparation.json", audit)
            raise ValueError(f"clean trial replacement budget exhausted: {key}")
        shutil.copytree(directory, accepted / directory.name)
        audit["selected"].append({"case": case, "seed": seed, "repeat": repeat,
                                 "directory": str(directory.resolve()), "run": directory.name})
        dump(output / "preparation.json", audit)
    audit["status"] = "complete"
    dump(output / "preparation.json", audit)
    export_run(accepted, output / "dataset", config["train_fraction"])
    with TemporalDataset(output / "dataset", config) as dataset:
        audit["dataset_sha256"] = dataset.fingerprint
    dump(output / "preparation.json", audit)
    return audit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.runner, args.config, args.output)


if __name__ == "__main__":
    main()
