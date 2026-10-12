"""Join a split production-flow run (#47) into the run.json that an embedded run writes.

Usage: python -m benchmarks.merge_split_run RUN_DIR [RUN_DIR...]

A split run has the broker on one host (`harbinger_prodflow --role broker`, which writes broker.json) and the
producers and workers on another (`--role client`, which writes client.json and messages.tsv). The client cannot read
the broker's counters, so this adds them, with the broker's final DLQ count, which is authoritative. run.json is
written last, so a run directory without it is incomplete, as for an embedded run.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path


def merge(client: dict, broker: dict) -> dict:
    if client.get("role") != "client":
        raise ValueError("client.json was not written by --role client")
    if client["arm"] != broker["arm"]:
        raise ValueError(f"arm mismatch: client {client['arm']!r}, broker {broker['arm']!r}")
    run = dict(client)
    run["role"] = "split"
    run["dlq"] = broker["dlq"]
    run["broker"] = broker["broker"]
    return run


def merge_dir(directory: Path) -> Path:
    target = directory / "run.json"
    if target.exists():
        raise FileExistsError(f"refusing to overwrite {target}")
    client = json.loads((directory / "client.json").read_text())
    broker = json.loads((directory / "broker.json").read_text())
    target.write_text(json.dumps(merge(client, broker), separators=(",", ":")) + "\n")
    return target


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("runs", nargs="+", type=Path)
    for directory in parser.parse_args().runs:
        print(merge_dir(directory))


if __name__ == "__main__":
    main()
