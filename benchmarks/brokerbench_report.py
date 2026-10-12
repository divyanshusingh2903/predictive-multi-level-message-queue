"""Broker-cost microbenchmark report (#37; E4 in #46). Overhead is reported, with no pass/fail.

Usage: python -m benchmarks.brokerbench_report CONFIG RESULTS_DIR [--out DIR]

RESULTS_DIR holds rep-<r>/<kind>/rate-<rate>.json from benchmarks/run_brokerbench.sh. Rate 0 is the as-fast-as-possible
run: its delivery rate is the peak sustained throughput and its latency only reflects the backlog, so it is not shown.
At a fixed rate a broker keeps up when it delivered every message and its producers ran on schedule; lateness shows
when publishing itself could not accept the rate.
"""
from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path


def load(results: Path) -> dict:
    """{kind: {rate: [run, ...]}} over every repetition."""
    table: dict = {}
    for path in sorted(results.glob("rep-*/*/rate-*.json")):
        run = json.loads(path.read_text())
        table.setdefault(run["kind"], {}).setdefault(float(run["rate"]), []).append(run)
    return table


def summarise(runs: list[dict]) -> dict:
    med = lambda values: statistics.median(values) if values else None
    pick = lambda f: [f(r) for r in runs]
    return {
        "repetitions": len(runs),
        "lost": sum(pick(lambda r: r["lost"])), "publish_errors": sum(pick(lambda r: r["publish_errors"])),
        "duplicates": sum(pick(lambda r: r["duplicates"])),
        "send_rate_per_s": med(pick(lambda r: r["send_rate_per_s"])),
        "delivery_rate_per_s": med(pick(lambda r: r["delivery_rate_per_s"])),
        "delivery_rate_range": [min(pick(lambda r: r["delivery_rate_per_s"])), max(pick(lambda r: r["delivery_rate_per_s"]))],
        "latency_ms": {q: med(pick(lambda r: r["latency_ms"][q])) for q in ("p50", "p90", "p99", "p999", "max")},
        "latency_p99_range": [min(pick(lambda r: r["latency_ms"]["p99"])), max(pick(lambda r: r["latency_ms"]["p99"]))],
        "lateness_p99_ms": med([r["lateness_ms"]["p99"] for r in runs if "lateness_ms" in r]),
    }


def report(config: dict, table: dict) -> tuple[str, dict]:
    summary = {kind: {str(rate): summarise(runs) for rate, runs in sorted(rates.items())} for kind, rates in table.items()}
    kinds = [k for k in config["kinds"] if k in summary]
    lines = [f"# Broker cost ({config['version']})", "",
             f"{config['producers']} producers, {config['consumers']} consumers with empty handlers, "
             f"{config['payload_bytes']}-byte payloads, {config['seconds']} s per run ({config['warmup_s']} s warm-up "
             f"excluded), {config['repetitions']} repetitions; medians across repetitions.", "",
             "## Peak sustained throughput (as fast as possible)", "",
             "| Broker | Delivered/s (median) | Range | Published/s | Lost |", "|---|---|---|---|---|"]
    for kind in kinds:
        s = summary[kind].get("0.0")
        if s:
            lines.append(f"| {kind} | {s['delivery_rate_per_s']:,.0f} | {s['delivery_rate_range'][0]:,.0f}–"
                         f"{s['delivery_rate_range'][1]:,.0f} | {s['send_rate_per_s']:,.0f} | {s['lost']} |")
    for rate in [r for r in config["rates"] if r > 0]:
        lines += ["", f"## Offered {rate:,} msg/s", "",
                  "| Broker | Delivered/s | P50 ms | P90 ms | P99 ms (range) | P99.9 ms | Max ms | Send lateness P99 ms | Lost |",
                  "|---|---|---|---|---|---|---|---|---|"]
        for kind in kinds:
            s = summary[kind].get(str(float(rate)))
            if not s:
                continue
            l = s["latency_ms"]
            lines.append(f"| {kind} | {s['delivery_rate_per_s']:,.0f} | {l['p50']:.3f} | {l['p90']:.3f} | {l['p99']:.3f} "
                         f"({s['latency_p99_range'][0]:.3f}–{s['latency_p99_range'][1]:.3f}) | {l['p999']:.3f} | "
                         f"{l['max']:.1f} | {s['lateness_p99_ms']:.3f} | {s['lost']} |")
    return "\n".join(lines) + "\n", summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("config", type=Path)
    parser.add_argument("results", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    text, summary = report(config, load(args.results))
    out = args.out or args.results
    (out / "report.md").write_text(text)
    (out / "summary.json").write_text(json.dumps({"config": config["version"], "brokers": summary}, indent=1) + "\n")
    print(text)


if __name__ == "__main__":
    main()
