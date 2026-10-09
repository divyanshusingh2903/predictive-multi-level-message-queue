"""Versioned duration buckets and explicit cold/incompatible/invalid fallback."""
from __future__ import annotations

from bisect import bisect_right
from dataclasses import dataclass
import math


@dataclass(frozen=True)
class Prediction:
    duration_ms: float | None
    bucket: int
    fallback: str | None
    updates: int
    backoff: bool = False


class DurationPolicy:
    def __init__(self, spec: dict):
        required = {"version", "num_levels", "default_priority", "boundaries_ms", "ready_labels"}
        if not isinstance(spec, dict) or set(spec) != required:
            raise ValueError("invalid policy fields")
        levels, default, ready = (spec[k] for k in ("num_levels", "default_priority", "ready_labels"))
        if (type(levels) is not int or not 1 <= levels <= 255 or type(default) is not int or
                not 0 <= default < levels or type(ready) is not int or ready <= 0):
            raise ValueError("invalid levels/default/readiness")
        boundaries = spec["boundaries_ms"]
        if (not isinstance(boundaries, list) or len(boundaries) != levels - 1 or
                any(type(x) not in (int, float) or not math.isfinite(x) or x <= 0 for x in boundaries) or
                any(a >= b for a, b in zip(boundaries, boundaries[1:])) or
                not isinstance(spec["version"], str) or not spec["version"]):
            raise ValueError("invalid version/boundaries")
        self.version, self.default, self.ready = spec["version"], default, ready
        self.levels, self.boundaries = levels, tuple(boundaries)

    def bucket(self, duration: float) -> int:
        if type(duration) not in (int, float) or not math.isfinite(duration) or duration < 0:
            raise ValueError("invalid duration")
        return bisect_right(self.boundaries, duration)

    def result(self, duration: object, updates: int, reason: str | None = None, backoff: bool = False) -> Prediction:
        if reason is None and updates < self.ready:
            reason = "unready"
        if reason is None and (type(duration) not in (int, float) or not math.isfinite(duration) or duration < 0):
            reason = "invalid_prediction"
        if reason:
            return Prediction(None, self.default, reason, updates, backoff)
        return Prediction(float(duration), self.bucket(duration), None, updates, backoff)
