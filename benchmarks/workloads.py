"""Generate schedule-independent oracle traces and immutable ingress metadata."""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
import random

VERSION = "synthetic-v1"
SCHEMA = {"version": "synthetic-job-v1", "headers": [
    {"name": "job", "type": "categorical", "vocabulary": ["class0", "class1", "class2"]}]}


def generate(spec: dict, seed: int) -> list[dict]:
    count, rate = spec["count"], spec["rate_per_s"]
    low, cap = spec.get("min_cost_us", 1000), spec.get("cap_cost_us", 20000)
    if (type(count) is not int or not 1 <= count <= 1000000 or
            type(seed) is not int or not math.isfinite(rate) or rate <= 0 or
            type(low) is not int or type(cap) is not int or not 0 < low <= cap <= 60000000):
        raise ValueError("invalid workload dimensions")
    distribution = spec["distribution"]
    if distribution not in ("uniform", "bimodal", "heavy_tail"):
        raise ValueError("unsupported distribution")
    if distribution == "bimodal" and cap <= 2 * low:
        raise ValueError("bimodal long cap must exceed the short-mode upper bound")
    costs, metadata, failures = (random.Random(f"{VERSION}:{seed}:{tag}")
                                for tag in ("cost", "metadata", "failure"))
    shift = spec.get("shift_index", count)
    if type(shift) is not int or not 0 <= shift <= count:
        raise ValueError("invalid shift index")
    informative = spec.get("informative", True)
    probability = spec.get("failure_probability", 0)
    abandon = spec.get("abandon_probability", 0)
    if not 0 <= probability <= 1 or not 0 <= abandon <= 1:
        raise ValueError("invalid failure probability")
    burst = spec.get("burst_size", 1)
    if type(burst) is not int or burst < 1:
        raise ValueError("invalid burst size")
    result = []
    for sequence in range(count):
        if distribution == "uniform":
            # Pick a latent job context first, then draw cost independently within its band.
            # Weight bands by their integer widths to retain an exactly uniform marginal.
            width = cap - low + 1
            category = min(2, 3 * costs.randrange(width) // width)
            begin = low + (category * width + 2) // 3
            end = low + ((category + 1) * width + 2) // 3 - 1
            cost = costs.randint(begin, end)
        elif distribution == "bimodal":
            category = costs.randrange(2) if costs.random() < .8 else 2
            if category < 2:
                mid = low + low // 2
                cost = costs.randint(low, mid) if category == 0 else costs.randint(mid + 1, low * 2)
            else:
                cost = costs.randint(max(2 * low + 1, round(.8 * cap)), cap)
        else:
            category = costs.randrange(3)
            uniform = min(math.nextafter(1.0, 0.0), (category + costs.random()) / 3)
            cost = min(cap, round(low / (1 - uniform) ** (1 / 1.4)))
        # This is generated job context; measured duration never becomes an ingress field.
        if sequence >= shift:
            category = 2 - category
        if not informative:
            category = metadata.randrange(3)
        result.append({"sequence": sequence, "arrival_us": round((sequence // burst) * burst * 1e6 / rate),
                       "cost_us": cost, "job": f"class{category}",
                       "failures": int(failures.random() < probability),
                       "abandons": int(failures.random() < abandon)})
    return result


def write_trace(path: Path, rows: list[dict]) -> str:
    columns = ("sequence", "arrival_us", "cost_us", "job", "failures", "abandons")
    data = "trace-v1\n" + "\n".join("\t".join(str(row[key]) for key in columns) for row in rows) + "\n"
    path.write_text(data, encoding="ascii")
    return hashlib.sha256(data.encode("ascii")).hexdigest()


def canonical_hash(value: object) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                     allow_nan=False).encode()).hexdigest()
