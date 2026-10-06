"""Collect (timestamp, key, ingress features, duration) from real handlers on a real corpus (#24, feature-signal-v1).

Workers run real work in separate processes: image resize + JPEG re-encode (Pillow), zlib compression, JSON parsing,
text tokenization, SHA-256, and SQLite aggregation over real trace rows. One key (`notify/http_call`) is a simulated
network call whose latency does not depend on its payload, as a control. Halfway through the run the mix shifts:
catalog uploads become mostly high-resolution and storage receives larger files.

Only features that a producer could put in a header at ingress are recorded (sizes, dimensions, row counts); measured
outcomes such as compression ratio are not features.

Usage: python collect_jobs.py CORPUS_DIR OUT.csv --minutes M --seed S [--workers 3]
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import multiprocessing as mp
import os
import random
import re
import sqlite3
import time
import zlib
from collections import Counter
from pathlib import Path

FIELDS = ["run", "seq", "submit_unix", "start_unix", "end_unix", "service", "job_type", "payload_bytes",
          "width", "height", "pixels", "rows", "phase", "duration_ms", "cpu_ms", "worker"]

_CORPUS: Path | None = None


def _init(corpus: str) -> None:
    global _CORPUS
    _CORPUS = Path(corpus)


def _handle(job: dict) -> dict:
    """Run one job in a worker process and measure wall and CPU time around the handler only."""
    path = _CORPUS / job["path"] if job.get("path") else None
    started_wall, started_cpu = time.time(), time.process_time()
    t0 = time.perf_counter()
    kind = job["job_type"]
    if kind == "resize_image":
        from PIL import Image
        with Image.open(path) as im:
            im = im.convert("RGB")
            for size in (1600, 800, 200):  # listing, detail and thumbnail renditions
                copy = im.copy()
                copy.thumbnail((size, size), Image.LANCZOS)
                copy.save(io.BytesIO(), "JPEG", quality=82)
    elif kind == "compress_file":
        zlib.compress(path.read_bytes(), 6)
    elif kind == "parse_json":
        doc = json.loads(path.read_bytes())
        sum(len(x["app"]) for x in doc["invocations"])
    elif kind == "index_text":
        Counter(w.lower() for w in re.findall(r"[A-Za-z_][A-Za-z0-9_]{2,}", path.read_text(errors="replace")))
    elif kind == "checksum":
        hashlib.sha256(path.read_bytes()).hexdigest()
    elif kind == "sqlite_report":
        db = sqlite3.connect(":memory:")
        db.execute("CREATE TABLE t(app TEXT, func TEXT, end REAL, duration REAL)")
        with path.open() as stream:
            db.executemany("INSERT INTO t VALUES(?,?,?,?)", (line.rstrip("\n").split(",") for line in stream))
        db.execute("SELECT app, COUNT(*), AVG(duration), MAX(duration) FROM t GROUP BY app ORDER BY 3 DESC").fetchall()
        db.close()
    elif kind == "http_call":
        time.sleep(job["latency_ms"] / 1000)
    else:
        raise ValueError(kind)
    duration = (time.perf_counter() - t0) * 1000
    return {**job, "start_unix": started_wall, "end_unix": time.time(), "duration_ms": round(duration, 3),
            "cpu_ms": round((time.process_time() - started_cpu) * 1000, 3), "worker": os.getpid()}


def make_job(rng: random.Random, corpus: dict, phase: int) -> dict:
    by_kind = corpus["by_kind"]
    services = [("catalog", "resize_image", 3), ("storage", "compress_file", 3), ("api", "parse_json", 3),
                ("search", "index_text", 2), ("storage", "checksum", 2), ("reports", "sqlite_report", 1),
                ("notify", "http_call", 3)]
    service, kind, _ = rng.choices(services, weights=[w for *_, w in services])[0]
    job = {"service": service, "job_type": kind, "phase": phase, "payload_bytes": "", "width": "", "height": "",
           "pixels": "", "rows": "", "path": None}
    if kind == "resize_image":
        images = by_kind["image"]
        big = [e for e in images if e["width"] * e["height"] >= 3_000_000]
        small = [e for e in images if e["width"] * e["height"] < 3_000_000]
        entry = rng.choice(big if rng.random() < (0.6 if phase else 0.15) else small)
        job.update(width=entry["width"], height=entry["height"], pixels=entry["width"] * entry["height"])
    elif kind in ("compress_file", "checksum", "index_text"):
        files = sorted(by_kind["text"], key=lambda e: e["bytes"])
        # After the shift storage receives larger files (upper half of the size range).
        pool = files[len(files) // 2:] if phase and kind != "index_text" else files
        entry = rng.choice(pool)
    elif kind == "parse_json":
        entry = rng.choice(by_kind["json"])
        job["rows"] = entry["rows"]
    elif kind == "sqlite_report":
        entry = rng.choice(by_kind["rows"])
        job["rows"] = entry["rows"]
    else:
        entry = None
        job["payload_bytes"] = rng.randint(200, 2000)
        job["latency_ms"] = rng.lognormvariate(3.4, 0.5)  # median ~30 ms, unrelated to the payload
    if entry:
        job["path"] = entry["path"]
        job["payload_bytes"] = entry["bytes"]
    return job


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--minutes", type=float, required=True)
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--workers", type=int, default=3)
    args = parser.parse_args()
    if args.out.exists():
        raise SystemExit(f"refusing to overwrite {args.out}")
    manifest = json.loads((args.corpus / "manifest.json").read_text())
    corpus = {"by_kind": {}}
    for entry in manifest["entries"]:
        corpus["by_kind"].setdefault(entry["kind"], []).append(entry)
    rng = random.Random(args.seed)
    run = f"m{args.minutes:g}-s{args.seed}"
    deadline = time.time() + args.minutes * 60
    shift_at = time.time() + args.minutes * 60 * 0.5
    seq = 0
    with mp.get_context("spawn").Pool(args.workers, initializer=_init, initargs=(str(args.corpus),)) as pool, \
            args.out.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS, extrasaction="ignore")
        writer.writeheader()
        pending = []
        # Closed loop: keep two jobs queued per worker so workers never idle and contention stays realistic.
        while time.time() < deadline or pending:
            while time.time() < deadline and len(pending) < args.workers * 2:
                job = make_job(rng, corpus, 1 if time.time() >= shift_at else 0)
                job.update(run=run, seq=seq, submit_unix=time.time())
                seq += 1
                pending.append(pool.apply_async(_handle, (job,)))
            done = [p for p in pending if p.ready()]
            for p in done:
                pending.remove(p)
                writer.writerow(p.get())
            if not done:
                time.sleep(0.001)
    print(f"{run}: {seq} jobs -> {args.out}")


if __name__ == "__main__":
    main()
