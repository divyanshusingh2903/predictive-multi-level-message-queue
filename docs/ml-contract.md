# Phase 2 ML contract

Status: the [issue #2 feature boundary](../ml_engine/README.md) is implemented: opt-in static ingress capture, internal immutable context, and matching Python validation/encoding. Prediction transport, feedback persistence, and activation remain planned under [issue #1's contract](https://github.com/divyanshusingh2903/predictive-multi-level-message-queue/issues/1). Persistent/transport field names below remain language-neutral design names rather than existing protobuf fields. See the [ADR](adr/0001-phase2-ml-contract.md) for rationale and the [validation plan](phase2-validation.md) for activation criteria.

Embedded brokers enable capture through optional `HarbingerConfig::ingress_features` (`ml::IngressFeatureConfig`), with a schema and caller-declared static routing-policy version. Absent configuration allocates no ML context. Capture is distinct from future feedback persistence: it does not write events or enable inference, and the standalone server currently has no feature-schema flag. Schema versions identify immutable field specifications; static policy identities must not be reused across incompatible configurations. Both priorities remain `default_priority` and prediction/model/fallback/timing fields remain absent, including when the complete snapshot exceeds its limit.

## Versions and identifiers

| Field | Meaning |
|---|---|
| `record_version` | Persistent event format; initially `1` |
| `protocol_version` | Classifier transport semantics; initially `1` |
| `feature_schema_version` | Immutable field names/types, allowlist, numeric ranges, and encoding specification |
| `model_version` | Immutable model artifact/update snapshot identifier; null when there is no usable response |
| `routing_policy_version` | Immutable mode-independent specification of level count, boundaries, and mapping rules |
| `broker_instance_id` | Unique telemetry namespace per broker process; separate from secret/fencing token material |
| `message_id` | Existing broker message ID |
| `attempt_id` | Internal per-message successful ownership-installation ordinal starting at 1; null for ingress/queued expiry; never the opaque attempt token |
| `event_id` | Broker instance plus monotonically allocated event sequence, unique across record types |

Version/instance identifiers are bounded strings (at most 128 UTF-8 bytes); counters use decimal strings in JSON to avoid cross-language integer truncation. Message IDs retain their existing format. A schema version refers to an immutable specification registered on both sides: changing an allowlist, type, range, hashing rule, or missing-value policy requires a new version. A model snapshot declares its compatible feature schema. Model updates change `model_version`; an ingress record retains the exact version used for its prediction.

Sequence allocation identifies an event, not a wall-clock ordering guarantee across concurrent threads. Log replay uses file generation/offset and explicit event identity. Broker completion-cache retention and learner deduplication are independent mechanisms.

Maintain a per-message delivery counter starting at zero, separately from `retry_count`. Increment it as part of successful in-flight ownership installation under the settlement mutex, after the final Pull cancellation check, and capture that ordinal in the installed delivery. A cancelled Pull restored to the queue, token generation, response preparation, aging, settlement replay, or reclamation does not increment it. The next successful delivery after Nack/lease requeue does. In-flight outcome events use their installed ordinal; queued expiry uses null even after earlier deliveries. Thus a cancelled Pull followed by Nack, lease expiry, and Ack on three successful deliveries produces attempts 1, 2, and 3, with no event for the cancelled Pull or replayed Ack.

## Proposed configuration

These settings are design starting values. They are not currently accepted by `HarbingerConfig` or server flags. Implementation issues must provide validation and configuration wiring.

| Setting | Proposed default/rule |
|---|---|
| `routing_mode` | `disabled`; accepted values `disabled`, `shadow`, `predictive` |
| `classifier_endpoint` | `127.0.0.1:50052`; local service for the initial experiment |
| `inference_deadline_ms` | `5`, positive and safely convertible to a clock deadline |
| `max_concurrent_predictions` | `32`, positive; reject excess prediction work to fallback immediately |
| `inference_retries` | `0` |
| `bucket_boundaries_ms` | `[10, 100]` for three levels; explicit boundaries required for other counts; empty for one level |
| `min_success_samples` | `100` valid unique successful labels before model readiness, subject to #5 evaluation |
| `feature_header_allowlist` | Empty; at most 16 typed entries, each with a declared range/encoding |
| `feedback_enabled` | `false`; static-mode collection can be enabled independently |
| `feedback_path` | No implicit path; required writable dedicated directory when collection is enabled |
| `feedback_buffer_records` / `feedback_buffer_bytes` | `4096` / `32 MiB`, whichever limit is reached first |
| `max_event_bytes` | `16 KiB` UTF-8 JSON encoding, including full context |
| `feedback_segment_bytes` | `64 MiB`; do not split a record; seal before crossing the cap |
| `feedback_retention_bytes` / `feedback_retention_age` | `1 GiB` / `7 days`, evict when either cap requires it |
| `feedback_sync_interval` | `1 second` healthy-operation target |
| `feedback_shutdown_drain` | `1 second` target; see storage/shutdown limitations below |

Existing level/default-priority validation remains authoritative: levels 1–255, priority in `[0, num_levels)`. Validate feature/policy specifications at startup. Enabled inference requires explicit compatible versions and exactly `num_levels - 1` finite positive increasing boundaries. Invalid configuration fails construction; invalid runtime prediction falls back. Do not silently resize three-level boundaries for another level count.

## Feature schema v1

| Field | Type and validation |
|---|---|
| `payload_size_bytes` | Unsigned 64-bit byte count, represented as a decimal string in JSON; Python/C++ preserve exact integer value before model conversion |
| `headers` | Map containing only configured application-header keys; each value is a finite binary64 number, UTF-8 category string, or null |
| `missing_reasons` | Map of configured missing/invalid keys to `absent`, `invalid_type`, `out_of_range`, `invalid_encoding`, or `oversized` |

Key lookup is exact and case-sensitive; no implicit normalization. Configuration declares each key's numeric or categorical type. Missing/invalid values become null with an explicit reason and missing indicator. Zero/empty-string categories remain distinct from missing. Do not truncate oversized values into another valid category.

Use at most 16 configured headers, 64 UTF-8 bytes per key, 256 UTF-8 bytes per value, and an 8 KiB encoded feature record. Retain only bounded data: never copy unapproved values while extracting. Exclude raw payloads, unapproved headers, free-text Nack/exception details, reserved `__*` headers (including `__producer_id`), consumer IDs, and attempt tokens. Oversized complete records disable prediction with `feature_limit`; if collection is enabled, record null features and a feature-validity reason, not raw rejected content.

The implemented size-admission rule measures the feature object alone, excluding enclosing schema/context metadata. `snapshot_json()` emits fixed top-level order (`payload_size_bytes`, `headers`, `missing_reasons`), sorted UTF-8 map keys, no whitespace, quote/backslash escapes, six-byte lowercase `\u00xx` escapes for all ASCII control bytes, and unescaped valid non-ASCII UTF-8. Binary64 values use scientific notation with 17 significant digits (16 after the decimal point), lowercase `e`, an explicit exponent sign, at least two exponent digits, and positive-zero normalization. C++ and Python use this same bounded representation; their ordinary default JSON formatting is not the admission rule. Illustrated objects below are semantic examples. Shared fixtures verify exact admission at 8,191/8,192/8,193 bytes.

### Numeric parsing and representation

Numeric headers use IEEE-754 binary64 (`double`) in C++, classifier transport, and Python. Accept only complete strings matching this locale-independent decimal grammar:

```text
-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
```

`0`, `-2`, `2.5`, and `1e3` are syntactically valid; leading/trailing whitespace, a leading `+`, leading zeros such as `01`, `.5`, `2.`, hexadecimal notation, `NaN`, infinity, and unit suffixes are invalid. Syntax failures use `invalid_type`. Convert using round-to-nearest, ties-to-even; reject overflow/nonfinite results, nonzero values rounded to zero, and converted values outside the schema's inclusive binary64 range as `out_of_range`. Finite nonzero subnormal values are allowed. Normalize negative zero to positive zero. Serialize JSON numbers with sufficient precision to round-trip the binary64 value; Python validates the typed snapshot without reinterpreting the original header string.

Numeric headers represent approximate real-valued features, not lossless integer identifiers. Preserve `payload_size_bytes` as an exact unsigned integer in extraction/transport and a decimal string in JSON; Python performs any model-facing conversion. Model-specific scaling/transforms are versioned with the model artifact and fitted without future-data leakage.

### Categorical encoding

Declare the encoding separately for each categorical header in the immutable schema. Prefer a fixed vocabulary for small known sets such as operation names, formats, and algorithms; hashing is an explicit option for open-ended categories, not a universal requirement.

| Encoding | Rule |
|---|---|
| `vocabulary` | Declare an ordered list of at most 1,024 unique exact UTF-8 values per field, subject to the value-size limit. Encode a known value as a sparse one-hot feature. An unseen valid value activates a dedicated unknown indicator. |
| `hash` | Use 1,024 bins per field: SHA-256 of the exact UTF-8 value, first eight digest bytes interpreted as unsigned big-endian, modulo 1,024. Activate that bin as a sparse one-hot feature. |

Each field has a separate feature namespace and missing indicator. Missing/invalid values activate only the missing indicator, with no category/bin or unknown indicator. An empty string is a valid category: it is known if listed in a vocabulary, otherwise unknown, or hashed normally. Never use a vocabulary/bin index as a numerical feature, which would imply an artificial ordering. Use structured feature identities (field plus category/bin/indicator) to prevent namespace collisions. Vocabulary additions/reordering, encoding changes, and hash-rule changes require a new schema version; do not grow a vocabulary silently while learning.

Hash collisions can merge distinct operations; hashing and one-hot encoding do not capture semantic similarity between operation names. Hashing also does not make sensitive data anonymous. Allowlisting remains necessary, and request IDs or other nearly unique categories generally offer little reusable cost information. Collision effects and unseen-category behavior must be evaluated in #5.

### Message-to-model ingestion and useful context

The data path is `message → C++ allowlist extraction/validation → versioned typed gRPC features → Python encoding → duration regressor → C++ bucket mapping`. Persist the original typed snapshot for delayed feedback and apply the same schema encoding during prediction and learning. The learner evaluates the stored prediction against its accepted successful runtime before updating.

Feature extraction determines which context is available; encoding only represents that context. Prefer application-supplied workload descriptors such as `operation`, `operation_family`, `input_format`, pixel counts, or item counts. For example, allowlisted `operation=image_resize`, `operation_family=image_processing`, `input_pixels=8847360`, and `output_pixels=230400` describe both the kind and scale of work without exposing image bytes. The broker remains a generic metadata validator, not a payload parser. If materially different requests have identical extracted features, the model cannot reliably distinguish their costs; include an uninformative-feature control in evaluation. Semantic text embeddings are outside the initial Phase 2 representation and would require a separately evaluated schema/model decision.

#2 must provide shared C++/Python fixtures for numeric syntax/conversion/ranges, exact payload sizes, Unicode/empty categories, vocabulary known/unknown/missing behavior, expected hash bins, sparse feature identities, and schema mismatch.

Example allowlist: `job_type` categorical with vocabulary `["resize", "convert", "thumbnail"]` and `units` numeric in `[0, 1000000]`. An incoming payload of 4096 bytes with `job_type=resize`, `units=2`, `authorization=secret`, and `__producer_id=producer-0` yields this typed snapshot (Python subsequently one-hot encodes `resize`):

```json
{
  "feature_schema_version": "features-v1-job-type-units",
  "features": {
    "payload_size_bytes": "4096",
    "headers": {"job_type": "resize", "units": 2},
    "missing_reasons": {}
  }
}
```

With `job_type` absent and an oversized `units` value, the same record contains:

```json
{
  "feature_schema_version": "features-v1-job-type-units",
  "features": {
    "payload_size_bytes": "4096",
    "headers": {"job_type": null, "units": null},
    "missing_reasons": {"job_type": "absent", "units": "oversized"}
  }
}
```

Forbidden/unapproved headers appear in neither features nor missing-reason maps. An unknown schema version is rejected as incompatible; its arbitrary values are not learned. A null configured value can be handled by the trained missing-value policy and does not automatically make the entire prediction invalid.

## Prediction request, response, and routing context

A request contains `protocol_version`, `feature_schema_version`, `routing_policy_version`, bounded `features`, `num_levels`, and `bucket_boundaries_ms`. The service receives no payload, message identity, or settlement token. C++ retains the message association locally.

A response echoes the three versions and contains `ready`, `model_version`, `predicted_processing_time_ms`, and `predicted_bucket`. Ready responses require a compatible nonempty model version, finite nonnegative duration, and an integer bucket matching the request policy. An unready response has null prediction/bucket. Unknown protocol/schema/policy versions fail compatibility checks even when a numeric prediction looks valid.

```json
{
  "protocol_version": 1,
  "feature_schema_version": "features-v1-job-type-units",
  "routing_policy_version": "routing-v1-3-levels-10-100",
  "ready": true,
  "model_version": "online-duration-v1-update-100",
  "predicted_processing_time_ms": 25.0,
  "predicted_bucket": 1
}
```

C++ computes the bucket independently: bucket index equals the number of boundaries less than or equal to the prediction. With `[10,100]`, predictions `0`, `9.9`, `10`, `99.9`, and `100` map to `0`, `0`, `1`, `1`, and `2`. Reject a mismatching echoed bucket. NaN, infinity, negative duration, a fractional bucket, or a bucket outside `[0,num_levels)` is invalid.

| Fallback reason | Condition |
|---|---|
| `timeout` | RPC deadline exceeded |
| `unavailable` | Transport/service unavailable |
| `overloaded` | No prediction concurrency slot or service resource exhaustion |
| `unready` | Cold start/incompatible or missing model reported unready |
| `incompatible_version` | Protocol, schema, or routing policy does not match |
| `invalid_prediction` | Malformed, negative, nonfinite, missing, or inconsistent prediction/bucket |
| `feature_limit` | Complete feature snapshot exceeds the limit |
| `client_error` | Other RPC/client failure |

For a failed prediction, retain null prediction/bucket and the applicable reason; bounded diagnostics may identify a returned model version but must not persist arbitrary error text. For valid predictions the fallback reason is null, even in shadow mode. Disabled mode makes no inference call and has null prediction/model/fallback.

Immutable routing context contains feature schema and feature snapshot/validity, policy and model versions, mode, predicted duration/bucket, actual ingress priority, fallback reason, and local inference elapsed time. Disabled mode with collection enabled still extracts features. Disabled mode without collection needs no ML context allocation. Shadow assigns `default_priority`; predictive assigns the validated bucket or fallback. Both priority fields equal the ingress choice initially. Context remains unchanged through aging/retry; `enqueue_time` retains its existing mutable placement meaning.

## Feedback event format

Each record has the following fields. Unknown values are JSON null; missing duration is never encoded as zero.

| Field | Rule |
|---|---|
| `record_version`, `event_id`, `broker_instance_id`, `message_id` | Required identity/version fields |
| `event_type` | `ingress` or `outcome` |
| `attempt_id` | Delivery ordinal for in-flight outcomes, null for ingress/queued expiry |
| `collected_at_utc` | UTC timestamp for operations; not a TTL/lease clock |
| `routing` | Full immutable ingress context, including features or explicit invalidity |
| `settlement_operation` | `ack`, `nack`, or null for maintenance/Pull expiry |
| `trigger` | `submit`, `settlement`, `lease_expiry`, `pull_expiry`, or `ttl_sweep` |
| `outcome` | `ack`, `retry`, `dlq`, or null for ingress |
| `dlq_reason` | `TTL_EXPIRED`, `MAX_RETRIES_EXCEEDED`, `PROCESSING_ERROR`, or null |
| `retry_count` | Count after the recorded transition; TTL never increments it |
| `processing_time_ms` | Consumer observation for an accepted Ack/Nack, otherwise null |
| `label_status` | `eligible`, `failure`, `censored`, `missing`, `negative`, `out_of_range`, or `invalid_features` |
| `elapsed_since_arrival_ms` | Local steady-clock duration at capture for analysis; no persisted absolute steady-clock timestamp |

Ingress events use null settlement/outcome/duration and `label_status=missing`. Full routing context in every outcome avoids a mandatory join with an ingress record that might have been lost. Keep records within 16 KiB; violations drop the record with an observable reason. No raw reason/details strings or opaque attempt tokens are exported. `PROCESSING_ERROR` is reserved for an actual transition to that DLQ reason; current handler failures use Nack and the normal failure budget.

For accepted requests, durations in `[0, delivery_lease_ms]` are plausibly usable; negative/larger durations get an invalid status. Values are consumer-provided, not trusted ground truth. The existing proto scalar does not distinguish an omitted duration from zero; this contract treats zero as reported zero and does not infer missing presence. Unaccepted late requests do not supply measurements, even when they cause reclamation.

Classify labels using the first matching rule below. Keep feature validity separately in the routing context so simultaneous measurement/feature problems remain observable. A null individual configured header is a valid missing-value input, not an unusable complete snapshot.

| Precedence | Condition | `label_status` |
|---|---|---|
| 1 | No accepted handler measurement | `missing` |
| 2 | Measurement is negative | `negative` |
| 3 | Measurement exceeds `delivery_lease_ms` | `out_of_range` |
| 4 | Complete feature snapshot is unusable | `invalid_features` |
| 5 | Actual outcome is TTL DLQ | `censored` |
| 6 | Accepted operation is Nack | `failure` |
| 7 | Actual outcome is successful Ack | `eligible` |

A valid Nack-after-TTL is therefore censored, not failure. A negative runtime on Ack-after-TTL is negative while the actual outcome remains TTL DLQ. Lease expiry has a missing measurement even if a rejected late request reports a duration. Successful Ack with usable features and zero runtime is eligible. Classification never changes settlement status, actual outcome, or retry budget. Only eligible events train initially. A later label-policy change requires a versioned model/evaluation decision, not reinterpretation of old outcome fields.

| Path | Operation / actual outcome | Observation and emission |
|---|---|---|
| Ack, valid ownership/token/lease, live TTL | `ack` / `ack` | One eligible event if measurement/features valid |
| Nack with remaining budget | `nack` / `retry` | One failure event if measurement/features valid, otherwise invalid; restore ingress priority |
| Nack exhausts budget | `nack` / `dlq`, `MAX_RETRIES_EXCEEDED` | One failure event if measurement/features valid, otherwise invalid |
| Ack or Nack after TTL, valid lease | Requested op / `dlq`, `TTL_EXPIRED` | One censored/invalid event; no retry increment; Ack remains OK |
| Lease expires, TTL live | null / `retry` or max-retry `dlq` | One event with null measurement; consume failure budget |
| Lease expires, TTL expired | null / TTL `dlq` | One event with null measurement; no retry increment |
| Queue expires at Pull or sweep | null / TTL `dlq` | One event with null attempt/measurement |
| Identical accepted settlement replay | No new transition | No event/update, including after redelivery |
| Invalid/unknown/stale/conflicting/wrong-owner request | No accepted measurement | No event unless an actual valid lease reclamation occurs |

Replay does not replace the first accepted measurement with changed request data. After completion eviction, existing token fencing still prevents settling a newer attempt; telemetry deduplication does not extend broker RPC replay retention.

Example: a shadow-mode message successfully finishes its first attempt. Here the illustrative configured static priority is 1, predicted bucket is 0, and the prediction remains unchanged by settlement:

```json
{
  "record_version": 1,
  "event_id": "instance-example:2",
  "broker_instance_id": "instance-example",
  "message_id": "123456789-0",
  "event_type": "outcome",
  "attempt_id": "1",
  "collected_at_utc": "2026-09-29T12:00:00Z",
  "routing": {
    "feature_schema_version": "features-v1-job-type-units",
    "routing_policy_version": "routing-v1-3-levels-10-100",
    "model_version": "online-duration-v1-update-100",
    "mode": "shadow",
    "features": {
      "payload_size_bytes": "4096",
      "headers": {"job_type": "resize", "units": 2},
      "missing_reasons": {}
    },
    "feature_validity": "valid",
    "predicted_processing_time_ms": 8.0,
    "predicted_bucket": 0,
    "ingress_priority": 1,
    "fallback_reason": null,
    "inference_elapsed_ms": 0.8
  },
  "settlement_operation": "ack",
  "trigger": "settlement",
  "outcome": "ack",
  "dlq_reason": null,
  "retry_count": 0,
  "processing_time_ms": 9,
  "label_status": "eligible",
  "elapsed_since_arrival_ms": 21
}
```

For the same ingress context, an accepted Ack after TTL changes `outcome` to `dlq`, `dlq_reason` to `TTL_EXPIRED`, and `label_status` to `censored` for a valid reported duration. A lease reclamation sets operation to null, trigger to `lease_expiry`, runtime to null, and status to `missing`; TTL wins over the failure budget. A queue-expiry event also has null attempt ID. Each actual transition receives its own event ID; an identical replay produces no second record.

## Transition capture and storage lifecycle

| Current function/path | Future capture point |
|---|---|
| `route_message()` | Capture ingress decision after TTL resolution and successful enqueue, retaining local context across the move |
| Ack/Nack → `settle()` → `finish_locked()` | After ownership/token/replay/lease checks; derive actual outcome and snapshot context before moving/erasing the message |
| `settle()` finds an expired valid lease | `finish_locked()` reclamation with no measurement from the rejected late request |
| `maintain_deliveries()` | Same reclamation path; no separate duplicate event |
| `Pull()` expired dequeued message | Capture TTL DLQ transition and context before moving to DLQ |
| Pull-path/background sweep → `dlq_swept()` | Capture each actual swept TTL DLQ before moving it |

Capturing an event must not reclassify, change settlement status, or add a throwing telemetry failure to an already accepted broker mutation. Use bounded, nonblocking admission; never take broker locks from the telemetry writer. Preserve settlement → queue/DLQ ordering. An outcome may reach storage before its ingress record under concurrency; full context makes this safe.

The single writer appends complete JSONL records and performs periodic flush/sync. On a partial write, stop using the damaged tail, report the failure, and recover into a new segment with bounded retry/backoff. Replay rejects/quarantines malformed records and an incomplete final line; it never trains twice while trying to parse a tail. Retention acts on sealed segments only, enforces both age and byte caps, and reports deletion of unconsumed records. If the active segment prevents satisfying a cap, stop/drop new telemetry until rotation/recovery permits bounded storage. Wall-clock retention must tolerate clock changes; wall time does not affect delivery.

Full admission buffer drops newest events with a reason counter. Disk failures do not block settlement and cannot cause unlimited buffer growth. Observe accepted/persisted/dropped events by bounded reason, buffer bytes/depth, writer failures, sync age, retention loss, learner backlog, and prediction fallbacks. Never put message IDs or raw headers into metric labels.

After broker handlers/maintenance stop emitting, stop telemetry admission, attempt the proposed one-second drain, discard remaining queued records on budget exhaustion, and join resources safely. The one-second drain is a target for healthy storage, not a way to interrupt blocked syscalls. Do not detach a thread that owns broker references. Any hard deadline requirement needs storage isolation and a revised ADR; document measured shutdown behavior in #3/#9.

For persisted events, #6/#8 define at-least-once log delivery, acknowledged ingest, and model replay: learning and its deduplication/cursor state must be checkpointed together. Store-and-forward never waits on a classifier during settlement. On log retention gaps, surface lost ranges and reset/reconcile the cursor explicitly. Broker crashes can still lose telemetry and all broker messages; no atomic settlement/log or exactly-once side-effect guarantee is introduced.
