"""Keep compact model evidence and checksummed, reproducible raw archives."""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import tarfile

from benchmarks.evaluate import publish_results
from .compare import dump


def archive(source: Path, destination: Path):
    def normalize(info):
        info.uid = info.gid = info.mtime = 0
        info.uname = info.gname = ""
        return info
    with destination.open("wb") as raw:
        with gzip.GzipFile(filename="", fileobj=raw, mode="wb", mtime=0) as zipped:
            with tarfile.open(fileobj=zipped, mode="w|", format=tarfile.PAX_FORMAT) as tar:
                tar.add(source, arcname=source.name, filter=normalize)


def publish(source: Path, prepared: Path, comparison: Path, output: Path, refresh: bool = False):
    if output.exists() and not refresh:
        raise ValueError("publication output must be fresh")
    decision = json.loads((comparison / "decision.json").read_text())
    manifest = json.loads((comparison / "manifest.json").read_text())
    preparation = json.loads((prepared / "preparation.json").read_text())
    if output.exists():
        previous = json.loads((output / "manifest.json").read_text())
        if any(previous[key] != manifest[key] for key in ("config_sha256", "dataset_sha256")):
            raise ValueError("refresh requires the identical frozen configuration and dataset")
    if (preparation["status"] != "complete" or preparation["dataset_sha256"] != decision["dataset_sha256"] or
            Path(preparation["initial_source"]).resolve() != source.resolve()):
        raise ValueError("preparation/comparison provenance mismatch")
    for name, expected in manifest["dataset_files"].items():
        if sha256(prepared / "dataset" / name) != expected:
            raise ValueError("dataset changed after comparison")
    provenance = {row["run"]: row for row in manifest["dataset_manifest"]["sources"]}
    for selected in preparation["selected"]:
        directory = Path(selected["directory"])
        expected = provenance[selected["run"]]
        for filename, key in (("trace.tsv", "trace_sha256"), ("observations.tsv", "sidecar_sha256"),
                              ("run.json", "run_metadata_sha256")):
            if sha256(directory / filename) != expected[key]:
                raise ValueError("selected raw trial changed after export")
        for segment in expected["sources"]:
            if sha256(directory / "feedback" / segment["file"]) != segment["sha256"]:
                raise ValueError("selected sealed feedback changed after export")
    output.mkdir(parents=True, exist_ok=refresh)
    if not (output / "collection").exists():
        publish_results(source, output / "collection")
    for name in ("manifest.json", "decision.json", "summary.json", "runs.jsonl", "delay-sensitivity.jsonl"):
        shutil.copyfile(comparison / name, output / name)
    shutil.copyfile(prepared / "preparation.json", output / "preparation.json")
    raw = output / "raw"; raw.mkdir(exist_ok=refresh)
    archive(source, raw / "collection-v1.tar.gz")
    archive(prepared / "dataset", raw / "dataset-v1.tar.gz")
    archive(comparison, raw / "comparison-v1.tar.gz")
    for replacement in sorted(prepared.glob("replacement-*")):
        if replacement.is_dir():
            archive(replacement, raw / f"{replacement.name}.tar.gz")
    inventory = []
    for path in sorted(raw.glob("*.tar.gz")):
        inventory.append({"file": path.name, "bytes": path.stat().st_size, "sha256": sha256(path)})
    dump(raw / "inventory.json", inventory)
    (raw / "SHA256SUMS").write_text("".join(f"{r['sha256']}  {r['file']}\n" for r in inventory))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("source", "prepared", "comparison", "output"):
        parser.add_argument(f"--{arg}", type=Path, required=True)
    parser.add_argument("--refresh", action="store_true", help="refresh only an identical frozen dataset/configuration")
    args = parser.parse_args()
    publish(args.source, args.prepared, args.comparison, args.output, args.refresh)


if __name__ == "__main__":
    main()
