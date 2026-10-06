"""Build the real-input corpus for the #24 feature-signal collection (feature-signal-v1).

Inputs are real data; the only synthetic step is resampling real photos to camera resolutions:
- images: the OpenCV sample photos (JPEG/PNG, git commit pinned below) plus Lanczos-upscaled copies at 4, 8 and 12 MP;
- text: Python standard-library source files (real text of 1 KB to several hundred KB);
- json: JSON documents built from slices of the Azure Functions 2021 trace (real rows, varying length);
- rows: row slices of the same trace for the SQLite report job.

Usage: python prepare_corpus.py OPENCV_SAMPLES_DIR PYTHON_STDLIB_DIR AZURE_TRACE.txt OUT_DIR
Writes OUT_DIR/manifest.json with every file's kind, size, features and sha256.
"""
from __future__ import annotations

import hashlib
import json
import random
import sys
from pathlib import Path

from PIL import Image

OPENCV_COMMIT = "41ef839c7d03231dc40c026d28e1ba80494f506d"
UPSCALE_MEGAPIXELS = (4, 8, 12)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    opencv, stdlib, trace, out = (Path(a) for a in sys.argv[1:5])
    out.mkdir(parents=True, exist_ok=False)
    rng = random.Random(24)
    entries = []

    images = out / "images"
    images.mkdir()
    for source in sorted(opencv.iterdir()):
        try:
            with Image.open(source) as im:
                im.load()
                image = im.convert("RGB")
        except Exception:
            continue
        if image.width * image.height < 40_000:
            continue  # icons and tiny test patterns are not uploads
        original = images / (source.stem + ".jpg")
        image.save(original, "JPEG", quality=90)
        entries.append({"kind": "image", "path": str(original.relative_to(out)), "width": image.width,
                        "height": image.height, "upscaled": False})
        # Real photo content at camera resolutions; one random target per source keeps the corpus bounded.
        target = rng.choice(UPSCALE_MEGAPIXELS) * 1_000_000
        scale = (target / (image.width * image.height)) ** 0.5
        if scale > 1.2:
            big = image.resize((int(image.width * scale), int(image.height * scale)), Image.LANCZOS)
            path = images / f"{source.stem}-{target // 1_000_000}mp.jpg"
            big.save(path, "JPEG", quality=90)
            entries.append({"kind": "image", "path": str(path.relative_to(out)), "width": big.width,
                            "height": big.height, "upscaled": True})

    text = out / "text"
    text.mkdir()
    sources = sorted(p for p in stdlib.rglob("*.py") if p.is_file() and 1_000 <= p.stat().st_size <= 600_000)
    for i, source in enumerate(rng.sample(sources, min(400, len(sources)))):
        path = text / f"{i:04d}-{source.name}"
        path.write_bytes(source.read_bytes())
        entries.append({"kind": "text", "path": str(path.relative_to(out))})

    rows = trace.read_text().splitlines()[1:400_001]
    data = out / "data"
    data.mkdir()
    for i in range(300):
        count = int(10 ** rng.uniform(1.5, 4.7))  # 30 to 50,000 rows
        start = rng.randrange(0, len(rows) - count)
        chunk = [r.split(",") for r in rows[start:start + count]]
        doc = [{"app": a, "func": f, "end": float(e), "duration": float(d)} for a, f, e, d in chunk]
        path = data / f"{i:04d}-{count}.json"
        path.write_text(json.dumps({"invocations": doc}))
        entries.append({"kind": "json", "path": str(path.relative_to(out)), "rows": count})
        csv_path = data / f"{i:04d}-{count}.csv"
        csv_path.write_text("\n".join(rows[start:start + count]))
        entries.append({"kind": "rows", "path": str(csv_path.relative_to(out)), "rows": count})

    for entry in entries:
        path = out / entry["path"]
        entry["bytes"] = path.stat().st_size
        entry["sha256"] = sha256(path)
    manifest = {"opencv_commit": OPENCV_COMMIT, "entries": entries,
                "counts": {k: sum(e["kind"] == k for e in entries) for k in ("image", "text", "json", "rows")}}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    print(json.dumps(manifest["counts"]))


if __name__ == "__main__":
    main()
