# `analysis/`: offline Python tools

This folder is **not part of the running broker.** Harbinger predicts processing times with **per-key statistics**
inside the C++ broker: a decaying duration histogram per key, read at the median, mapped to a tier
([duration predictor](../docs/duration-predictor.md)). No machine-learning model is served, and no Python runs next to
the broker. The Python code here is the offline toolkit used to check the broker's data formats and to produce
published evidence. (Before October 2026 this folder was called `ml_engine/`; manifests of results published before
the rename record that path.)

| Part | Files | Status |
|---|---|---|
| Feature and feedback formats | `features.py`, `feedback.py` | **Live.** Python mirror of the broker's ingress-feature encoding and reader for its persistent feedback files; used by `benchmarks/export_feedback.py`. |
| Real-trace predictability | `trace_analysis.py`, `feature_signal.py` | **Reproduces published results:** `benchmarks/results/azure2021/` (`trace_analysis.py`) and `feature-signal-v1/` (#24, `feature_signal.py`). |
| C++ predictor cross-check | `cpp_replay.py` | **Live.** Scores `harbinger_predictor_replay` (the broker's own predictor) on a temporal export. |
| Online-model comparison (#5) | `dataset.py`, `models.py`, `policy.py`, `replay.py`, `metrics.py`, `compare.py`, `prepare.py`, `publish.py` | **Archived.** Compared learned models (River regressors and trees) with simple statistics (per-key means, EWMA). None of the eight candidates passed every gate (`benchmarks/results/issue5-v1/`). The broker uses the simplest option, per-key statistics. Kept to reproduce that result. |

Core tools need only the Python 3.10+ standard library. `requirements-models.txt` (River) is needed only to rerun
the archived comparison, and `requirements-analysis.txt` only for the #24 study.

## Enable capture in an embedded broker

```cpp
#include "harbinger_service.hpp"

harbinger::HarbingerConfig config;
config.ingress_features = harbinger::ml::IngressFeatureConfig{
    .schema = {
        .version = "features-v1-job-units",
        .headers = {
            {.name = "job_type", .type = harbinger::ml::FeatureType::Categorical,
             .vocabulary = {"resize", "convert", "thumbnail"}},
            {.name = "units", .type = harbinger::ml::FeatureType::Numeric,
             .minimum = 0, .maximum = 1000000},
        },
    },
    .routing_policy_version = "static-v1-three-levels-priority-1",
};
harbinger::HarbingerService broker{config};
```

`ingress_features=nullopt` is the default and allocates no ML context. An empty allowlist captures payload byte count only. Configuration is programmatic in this issue; the standalone server has no schema-file flag. Enabling capture does not enable feedback persistence or require Python running.

The broker copies configuration at construction. Schema/policy identifiers must be nonempty UTF-8 strings of at most 128 bytes. A schema identifier denotes an immutable specification; changes to fields, ranges, vocabularies, or encoding require a different version. The policy identifier is caller-declared: use a different identity for incompatible static configurations. It does not validate or enable predictive bucket routing.

## Data flow and privacy

`route_message()` resolves TTL, extracts only allowlisted metadata, attaches a `shared_ptr<const RoutingContext>`, and enqueues once. The immutable context carries schema/policy versions, features or explicit invalidity, disabled mode, and actual ingress priority. Model version, predictions, fallback reason, and inference time are absent. Queue placement, aging, cancellation restoration, in-flight delivery, retries, and DLQ copies preserve this context.

Payload content, unapproved headers, reserved `__*` keys, and arbitrary failure text never enter the feature snapshot. Existing payloads/application headers still reach consumers normally; capture does not redact the application's own messages. ML context is not serialized into Pull or InspectDlq responses or injected into headers.

Limits are 16 headers, 64 UTF-8 bytes per key, 256 bytes per value, 1,024 vocabulary entries per field, and an 8 KiB serialized feature object. Numeric headers use the [contract's strict decimal grammar](../docs/ml-contract.md#numeric-parsing-and-representation). Missing/invalid individual values become null with reasons; unknown valid categories remain strings for the encoder. A complete size overflow retains null features plus `FeatureLimit` without rejecting the submission or changing static priority.

Vocabularies are bounded configuration, not per-message data: the maximum raw category bytes are `16 × 1024 × 256 = 4 MiB` per schema, excluding container/key overhead. The broker configuration and extractor own their copies; extraction never copies or validates the vocabulary again. Recorded feature objects contain only observed allowlisted values.

## Python validation and encoding

Python 3.10+ and its standard library suffice:

```python
from analysis.features import FeatureSchema, HeaderFeature, encode

schema = FeatureSchema("features-v1-job-units", (
    HeaderFeature("job_type", "categorical", vocabulary=("resize", "convert", "thumbnail")),
    HeaderFeature("units", "numeric", minimum=0, maximum=1000000),
))
snapshot = {
    "payload_size_bytes": "4096",
    "headers": {"job_type": "resize", "units": 2.0},
    "missing_reasons": {},
}
features = encode(schema, schema.version, snapshot)
```

Sparse feature keys are four-tuples `(source, field, kind, index)`, for example:

```text
("payload", "size_bytes", "numeric", "")     → 4096.0
("header", "job_type", "category", "0")      → 1.0
("header", "units", "numeric", "")           → 2.0
```

Vocabulary categories are one-hot, not ordinal numeric values. Unseen values use `unknown`; missing/invalid values use `missing`. Optional `encoding="hash"` activates one of 1,024 bins using the versioned SHA-256 rule. Hashing has collision risk and preserves no semantic similarity. Meaningful operation-family and work-size metadata provide the context needed to predict costs.

`validate_snapshot()` rejects mismatched versions, unapproved/missing keys, invalid types/ranges, malformed UTF-8, and inconsistent missing reasons. Payload size stays exact through validation; `encode()` explicitly converts it to binary64 for model input. `extract_features()` is a reference extractor for fixtures; production ingress extraction is C++.

## Deterministic serialization and checks

`snapshot_json()` in both languages emits the same compact UTF-8 feature object: fixed top-level order, sorted header/reason keys, explicit ASCII control escapes, and 17-significant-digit scientific numeric values. The 8 KiB limit uses these bytes rather than language-specific default JSON formatting. Illustrative JSON objects in the contract are semantic examples, not necessarily this exact byte representation.

```bash
python3 -m unittest discover -s analysis/tests -v
cmake -S . -B build -DHARBINGER_WARNINGS_AS_ERRORS=ON -DHARBINGER_REQUIRE_PYTHON_TESTS=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Both suites consume `tests/fixtures/ml/features_v1.json`, including numeric bit patterns, fixed hash bins, UTF-8/escape examples, privacy exclusions, and exact size admission boundaries. Python tests are registered with CTest when Python 3.10+ is available; `HARBINGER_REQUIRE_PYTHON_TESTS=ON` makes its absence a configuration error and is required by the normal CI job.

Feedback event capture/persistence and delivery ordinals are implemented in #3 through embedded `HarbingerConfig::feedback`, separately from feature capture. Use the [feedback guide](../docs/feedback.md) and [ML contract](../docs/ml-contract.md) for their interfaces. Prediction and predictive routing are implemented in the broker's C++ [duration predictor](../docs/duration-predictor.md); the ingress features captured here are recorded for analysis and are not inputs to that predictor.

## Feedback validation and temporal datasets

Issue #4 adds `feedback.validate_record()` and the disk-backed `FeedbackIndex`
for sealed v1 records. Callers register `FeatureSchema` instances and supply the
recorded delivery lease to audit label precedence. Duplicate event identities
deduplicate only when contents match; conflicting records, malformed JSON,
unknown schemas, changed per-message routing context, and incomplete tails fail
explicitly. Active/suspect segments are not admitted. Actual disposition,
failed-attempt status, measurement validity/missingness, feature usability, and
TTL censorship remain distinct, overlapping dimensions.

The [benchmark exporter](../benchmarks/README.md#leakage-safe-static-feedback-export)
joins exact monotonic sidecars to production-static feedback, keeps all attempts
of each broker-instance/message together, and embargoes labels crossing the
training cutoff. Evaluation ingress and delayed eligible Ack labels are separate
ordered streams. Exported model inputs are immutable ingress features only;
oracle costs, retries, outcomes, and identities are audit metadata. Issue #5's
learners consume only causally available eligible labels. Legacy feedback-only export provides
audit views, not an invented precise temporal training split.

## Online model comparison (issue #5, archived)

Historical: this comparison ended with `no_qualifier` and is kept only so the published result can be reproduced.

The [predictor guide](../docs/online-predictor.md) explains the literature,
algorithms, sparse-indicator adapter, delayed evaluation, and selection limits.
The comparator implements global/per-job means, per-job EWMA, raw/log scaled
linear regression, raw/log ordinary Hoeffding trees and a raw adaptive tree.
Dependencies are isolated from the standard-library feature/feedback tools:

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r analysis/requirements-models.txt
.venv/bin/python -m pip check
python3 -m unittest discover -s analysis/tests -v
.venv/bin/python -m unittest discover -s analysis/model_tests -v
```

Use fresh output roots. Build the benchmark runner as described in
[`benchmarks/README.md`](../benchmarks/README.md). First exercise the small
development matrix (seeds 101/202), which cannot establish model selection:

```bash
python3 -m benchmarks.evaluate --config benchmarks/configs/predictor-development-v1.json \
  --runner build-issue5/benchmarks/harbinger_synthetic_benchmark --output /tmp/opencode/issue5-dev-runs
python3 -m benchmarks.export_feedback --run /tmp/opencode/issue5-dev-runs \
  --train-fraction 0.5 --output /tmp/opencode/issue5-dev-data
.venv/bin/python -m analysis.compare --dataset /tmp/opencode/issue5-dev-data \
  --config analysis/configs/online-comparison-v1.json --output /tmp/opencode/issue5-dev-report
```

The frozen full collection takes approximately 80–90 minutes on the recorded
host, plus replacement/export/comparison time. Preparation audits full coverage,
preserves invalid trials and admits only coverage-selected replacements:

```bash
python3 -m benchmarks.evaluate --config benchmarks/configs/predictor-datasets-v1.json \
  --runner build-issue5/benchmarks/harbinger_synthetic_benchmark --output /tmp/opencode/issue5-runs-v1
python3 -m analysis.prepare --source /tmp/opencode/issue5-runs-v1 \
  --runner build-issue5/benchmarks/harbinger_synthetic_benchmark \
  --config analysis/configs/online-comparison-v1.json --output /tmp/opencode/issue5-prepared-v1
.venv/bin/python -m analysis.compare --dataset /tmp/opencode/issue5-prepared-v1/dataset \
  --config analysis/configs/online-comparison-v1.json --output /tmp/opencode/issue5-comparison-v1
python3 -m analysis.publish --source /tmp/opencode/issue5-runs-v1 \
  --prepared /tmp/opencode/issue5-prepared-v1 --comparison /tmp/opencode/issue5-comparison-v1 \
  --output benchmarks/results/issue5-v1
```

`manifest.json` records input/config/schema/source fingerprints and dependencies.
`predictions.jsonl` retains original forecasts and score-before-learn observations;
`runs.jsonl` has quality/cohort/resource reports; `delay-sensitivity.jsonl` adds
10/100 ms label-availability delays; `decision.json` records `selected`,
`no_qualifier`, or `incomplete_evidence`. `--no-profile` is a deterministic smoke
option and cannot produce a complete resource-qualified selection. Invalid input
exports fail explicitly; they are not silently filtered into a favorable dataset.

For reproducibility, publication keeps compact reports and a SHA-256 inventory of
raw collection, temporal export and complete comparison archives (the archives
themselves are not tracked in git). Replay uses
publication-time availability, not production ingestion delay. Readiness and
synthetic bucket boundaries are experimental policy, not new broker defaults.
