# Ingress features and model encoding

Issue #2 implements a bounded C++ feature extractor, immutable broker-only routing context, and matching Python validation/sparse encoding. Issue #3 adds optional [persistent broker feedback](../docs/feedback.md). There is no classifier service or learner yet. Routing remains static.

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
from ml_engine.features import FeatureSchema, HeaderFeature, encode

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
python3 -m unittest discover -s ml_engine/tests -v
cmake -S . -B build -DHARBINGER_WARNINGS_AS_ERRORS=ON -DHARBINGER_REQUIRE_PYTHON_TESTS=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Both suites consume `tests/fixtures/ml/features_v1.json`, including numeric bit patterns, fixed hash bins, UTF-8/escape examples, privacy exclusions, and exact size admission boundaries. Python tests are registered with CTest when Python 3.10+ is available; `HARBINGER_REQUIRE_PYTHON_TESTS=ON` makes its absence a configuration error and is required by the normal CI job.

Feedback event capture/persistence and delivery ordinals are implemented in #3 through embedded `HarbingerConfig::feedback`, separately from feature capture. Serving follows in #6 and routing activation in #7. Use the [feedback guide](../docs/feedback.md) and [ML contract](../docs/ml-contract.md) for their interfaces.

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
oracle costs, retries, outcomes, and identities are audit metadata. No learner or
future-label preprocessing is implemented. Legacy feedback-only export provides
audit views, not an invented precise temporal training split.
