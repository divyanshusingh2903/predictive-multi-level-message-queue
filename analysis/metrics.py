"""Duration-quality populations, paired-seed intervals, and resource estimates."""
from __future__ import annotations

from collections import Counter
import math
import pickle
import random
import statistics
import sys
import types


def quantiles(values) -> dict:
    ordered = sorted(values)
    return {"count": len(ordered), **{f"p{p}": ordered[max(0, math.ceil(len(ordered) * p / 100) - 1)]
            if ordered else None for p in (50, 95, 99)}}


class Quality:
    def __init__(self, levels: int):
        self.ingress = self.labels = self.predicted = self.positive = self.severe = 0
        self.eligible_positive = 0
        self.absolute = self.squared = 0.0
        self.fallback = Counter()
        self.backoff = 0
        self.confusion = [[0] * levels for _ in range(levels)]

    def predict(self, prediction):
        self.ingress += 1
        if prediction.fallback:
            self.fallback[prediction.fallback] += 1
        self.backoff += prediction.backoff

    def score(self, prediction, actual: int, actual_bucket: int):
        self.labels += 1
        self.eligible_positive += actual > 0
        self.confusion[actual_bucket][prediction.bucket] += 1
        if prediction.duration_ms is None:
            return
        self.predicted += 1
        difference = prediction.duration_ms - actual
        self.absolute += abs(difference)
        self.squared += difference * difference
        if actual > 0:
            self.positive += 1
            self.severe += prediction.duration_ms < actual / 2

    def report(self) -> dict:
        return {"ingress": self.ingress, "eligible_labels": self.labels, "scored_predictions": self.predicted,
                "prediction_coverage": self.predicted / self.labels if self.labels else None,
                "fallback_ingress": dict(self.fallback), "category_backoff_ingress": self.backoff,
                "mae_ms": self.absolute / self.predicted if self.predicted else None,
                "rmse_ms": math.sqrt(self.squared / self.predicted) if self.predicted else None,
                "positive_scored_labels": self.positive, "severe_underestimates": self.severe,
                "eligible_positive_labels": self.eligible_positive,
                "severe_underestimate_rate": self.severe / self.positive if self.positive else None,
                "bucket_confusion": self.confusion,
                "bucket_accuracy": sum(self.confusion[i][i] for i in range(len(self.confusion))) / self.labels
                if self.labels else None}


def paired_interval(seed_deltas: list[float], samples: int, seed: int) -> dict:
    if len(seed_deltas) < 5:
        return {"paired_seeds": len(seed_deltas), "status": "insufficient_seeds", "mean_delta": None, "ci95": None}
    rng = random.Random(seed)
    bootstrap = sorted(statistics.mean(rng.choices(seed_deltas, k=len(seed_deltas))) for _ in range(samples))
    return {"paired_seeds": len(seed_deltas), "status": "measured", "mean_delta": statistics.mean(seed_deltas),
            "ci95": [bootstrap[max(0, math.ceil(samples * .025) - 1)], bootstrap[math.ceil(samples * .975) - 1]]}


def deep_size(value: object) -> int:
    """Cycle-safe Python ownership estimate; explicitly not whole-process RSS."""
    seen = set()
    def walk(item):
        if id(item) in seen or isinstance(item, (types.ModuleType, type, types.FunctionType, types.MethodType)):
            return 0
        seen.add(id(item))
        size = sys.getsizeof(item)
        if isinstance(item, dict):
            size += sum(walk(k) + walk(v) for k, v in item.items())
        elif isinstance(item, (tuple, list, set, frozenset)):
            size += sum(walk(x) for x in item)
        elif hasattr(item, "__dict__"):
            size += walk(vars(item))
        return size
    return walk(value)


def model_memory(candidate) -> dict:
    # Serialized native state supplements hidden Cython buffers (e.g. ADWIN).
    # This is an accounting proxy, not a hard allocator measurement or RSS bound.
    python_bytes = deep_size(candidate)
    serialized_bytes = len(pickle.dumps(candidate, protocol=5))
    return {"python_owned_bytes": python_bytes, "serialized_state_bytes": serialized_bytes,
            "accounted_state_bytes": python_bytes + serialized_bytes}
