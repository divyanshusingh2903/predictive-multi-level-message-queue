"""Bounded statistics and isolated River 0.22 adapters; no inference transport."""
from __future__ import annotations

import hashlib
import json
import math
from .features import FeatureSchema, FeatureError, encode
from .policy import DurationPolicy

CANDIDATES = ("global_mean", "job_mean", "job_ewma", "linear_raw", "linear_log",
              "tree_raw", "tree_log", "adaptive_tree_raw")


class FeatureAdapter:
    def __init__(self, schema: FeatureSchema, limit: int):
        self.schema = schema
        self.indicators = []
        for field in schema.headers:
            self.indicators.append(("header", field.name, "missing", ""))
            if field.type == "categorical":
                if field.encoding == "vocabulary":
                    self.indicators.extend(("header", field.name, "category", str(i)) for i in range(len(field.vocabulary)))
                    self.indicators.append(("header", field.name, "unknown", ""))
                else:
                    self.indicators.extend(("header", field.name, "bin", str(i)) for i in range(1024))
        if len(self.indicators) + len(schema.headers) + 1 > limit:
            raise ValueError("adapter dimension limit exceeded")

    def vector(self, version: str, snapshot: dict) -> dict:
        sparse = encode(self.schema, version, snapshot)
        vector = dict.fromkeys(self.indicators, 0.0)
        vector.update(sparse)
        for field in self.schema.headers:
            if field.type == "numeric":
                vector.setdefault(("header", field.name, "numeric", ""), 0.0)
        return vector

    def group(self, snapshot: dict, header: str) -> str:
        field = next((f for f in self.schema.headers if f.name == header), None)
        if field is None or field.type != "categorical" or field.encoding != "vocabulary":
            raise ValueError("statistics group requires a fixed-vocabulary header")
        value = snapshot["headers"][header]
        if value is None:
            return "missing"
        return f"category:{field.vocabulary.index(value)}" if value in field.vocabulary else "unknown"


class Candidate:
    def __init__(self, name: str, config: dict, run_scope: str = "independent-fixture"):
        if name not in CANDIDATES:
            raise ValueError("unknown candidate")
        if config["adapter_version"] != "numeric-scale-indicator-zero-v1":
            raise ValueError("incompatible model preprocessing version")
        self.name, self.config = name, config
        self.schema = FeatureSchema.from_dict(config["schema"])
        self.adapter = FeatureAdapter(self.schema, config["dimension_limit"])
        self.policy = DurationPolicy(config["policy"])
        digest = hashlib.sha256(json.dumps(config, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()).hexdigest()[:12]
        self.configuration_version = f"{config['version']}:{config['adapter_version']}:{name}:{digest}"
        self.run_scope = hashlib.sha256(run_scope.encode()).hexdigest()[:12]
        self.updates, self.mean, self.groups = 0, 0.0, {}
        self.scaler = self.estimator = None
        if name.startswith(("linear_", "tree_", "adaptive_tree_")):
            import river
            if river.__version__ != "0.22.0":
                raise ValueError("model requires River 0.22.0")
            from river import drift, linear_model, optim, preprocessing, tree
            if name.startswith("linear_"):
                self.scaler = preprocessing.StandardScaler()
                settings = config["linear"]
                self.estimator = linear_model.LinearRegression(
                    optimizer=optim.SGD(settings["learning_rate"]), intercept_lr=settings["intercept_lr"],
                    l2=settings["l2"], clip_gradient=settings["clip_gradient"],
                    initializer=optim.initializers.Zeros())
            elif name.startswith("adaptive_"):
                self.estimator = tree.HoeffdingAdaptiveTreeRegressor(
                    **config["tree"], **config["adaptive_tree"], drift_detector=drift.ADWIN(**config["adwin"]))
            else:
                self.estimator = tree.HoeffdingTreeRegressor(**config["tree"])

    @property
    def version(self):
        return f"{self.configuration_version}:{self.run_scope}:update-{self.updates}"

    def _scaled(self, vector: dict, learn: bool) -> dict:
        if self.scaler is None:
            return vector
        # Indicators are fixed 0/1 coordinates, never centered/scaled as sparse active-only values.
        numeric = {key: value for key, value in vector.items() if key[2] == "numeric"}
        if learn:
            self.scaler.learn_one(numeric)
        return {**vector, **self.scaler.transform_one(numeric)}

    def predict(self, snapshot: dict | None, version: str):
        if version != self.schema.version:
            return self.policy.result(None, self.updates, "incompatible_version")
        if snapshot is None:
            return self.policy.result(None, self.updates, "absent_features")
        try:
            vector = self.adapter.vector(version, snapshot)
        except FeatureError:
            return self.policy.result(None, self.updates, "invalid_features")
        backoff = False
        if self.estimator is None:
            duration = self.mean
            if self.name != "global_mean":
                group = self.adapter.group(snapshot, self.config["group_header"])
                count, mean, ewma = self.groups.get(group, (0, 0.0, 0.0))
                backoff = count < self.config["group_min_labels"]
                if not backoff:
                    duration = ewma if self.name == "job_ewma" else mean
        else:
            duration = self.estimator.predict_one(self._scaled(vector, False))
            if self.name.endswith("_log") and duration is not None:
                try:
                    duration = math.expm1(duration)
                except OverflowError:
                    duration = math.inf
        return self.policy.result(duration, self.updates, backoff=backoff)

    def learn(self, snapshot: dict, version: str, duration: int):
        if type(duration) is not int or not 0 <= duration <= self.config["lease_ms"]:
            raise ValueError("ineligible duration")
        vector = self.adapter.vector(version, snapshot)
        if self.estimator is not None:
            target = math.log1p(duration) if self.name.endswith("_log") else duration
            self.estimator.learn_one(self._scaled(vector, True), target)
        self.updates += 1
        self.mean += (duration - self.mean) / self.updates
        if self.name in ("job_mean", "job_ewma"):
            group = self.adapter.group(snapshot, self.config["group_header"])
            count, mean, ewma = self.groups.get(group, (0, 0.0, 0.0))
            count += 1
            mean += (duration - mean) / count
            ewma = duration if count == 1 else ewma + self.config["ewma_alpha"] * (duration - ewma)
            self.groups[group] = (count, mean, ewma)
