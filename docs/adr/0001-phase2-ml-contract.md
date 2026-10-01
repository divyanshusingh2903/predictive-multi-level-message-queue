# ADR 0001: Phase 2 ML routing and feedback

Status: Phase 2 design with [feature capture/encoding](../../ml_engine/README.md) implemented. Prediction transport, persistent feedback, and predictive activation remain planned. Resource defaults and performance gates marked proposed must be validated before rollout.

Tracking: [contract issue #1](https://github.com/divyanshusingh2903/predictive-multi-level-message-queue/issues/1), under [Phase 2 tracker #11](https://github.com/divyanshusingh2903/predictive-multi-level-message-queue/issues/11).

## Context

Harbinger currently assigns `default_priority` in `HarbingerService::route_message()`. Consumers measure callback runtime with `steady_clock` and include integer `processing_time_ms` in Ack/Nack, but the broker discards that field. Queue, in-flight, completion, and DLQ state are process-local.

The hypothesis is that prioritizing messages with shorter predicted handler runtimes improves completion latency. Predictions can be wrong, observations arrive after routing, failures censor runtimes, and classifier calls add ingress overhead. The implementation must make those effects measurable while retaining existing delivery semantics.

## Decisions

### Predict duration, then assign a bucket

Predict successful handler duration in milliseconds, excluding queue wait and settlement overhead. Compare statistical, online linear, and online tree regressors in #5 before selecting a model/framework. A log-transformed target is an experiment option; service responses always use milliseconds.

C++ maps the predicted duration to ordered, versioned boundaries. Shorter work receives a numerically lower priority. Exactly `num_levels - 1` boundaries are required; equality enters the next bucket. The current `uint8_t` configuration supports 1–255 levels. A one-level queue requires `default_priority=0` and no boundaries.

Boundaries do not adapt silently as the model learns. Changing features, model artifacts, or bucket policy has separate version semantics defined in the [ML contract](../ml-contract.md).

### Use a separate Python service

A separate gRPC service serves predictions and ingests persisted feedback asynchronously. C++ owns response validation, final priority, and fallback. Actual protobufs and code generation belong to #6; public Broker RPCs and the `harbinger_rpc` package remain unchanged by this ADR.

`route_message()` remains the only ingress classification and TTL-resolution point. No classifier or filesystem I/O runs under queue/settlement locks. Proposed inference defaults are a 5 ms RPC deadline, 32 concurrent calls, immediate fallback on saturation, and no inference retries. Admission consumes no unbounded waiting queue. Local feature extraction/response validation must also be bounded and measured; an RPC deadline alone is not a total Submit latency bound.

### Keep modes explicit

| Mode | Inference | Ingress priority |
|---|---|---|
| `disabled` (default) | None | `default_priority` |
| `shadow` | Predict and record | `default_priority` |
| `predictive` | Predict and record | Valid predicted bucket, otherwise `default_priority` |

Feedback collection is separately configured and off by default. Enable it in disabled mode to collect baseline training data before starting Python. Evaluation in shadow/predictive modes requires collection to be enabled; metrics alone cannot supply delayed training labels.

Start ML experiments in shadow mode. Enable predictive mode explicitly only after the [validation gate](../phase2-validation.md) passes. A classifier failure never rejects an otherwise valid submission merely because inference failed.

An unavailable, slow, unready, incompatible, overloaded, or invalid classifier falls back to `default_priority`. Cold-start readiness requires a declared training-sample threshold and compatible feature/policy versions. The ML service serializes updates or swaps complete snapshots so concurrent prediction never sees partially updated state.

### Retain an immutable ingress snapshot

Use payload byte count and typed, explicitly allowlisted application headers. Raw payloads, unapproved headers, free-text errors, producer/consumer identities, and settlement tokens are excluded by default. Header allowlists and encoding limits are part of a versioned feature specification.

C++ validates a typed snapshot; Python encodes it for prediction and delayed learning. Numeric headers use a strict locale-independent decimal grammar and binary64 values; payload byte count remains exact until model conversion. Prefer fixed, bounded vocabularies with sparse one-hot encoding for small categorical sets. Optional hashing also uses sparse one-hot bins, with explicit collision tradeoffs; never interpret category indices as ordered numeric values. Encoding/vocabulary changes require a new schema version. Shared C++/Python fixtures are part of #2.

Useful context comes from declared workload metadata, such as operation family and pixel/item counts, rather than arbitrary payload inspection. Encoding cannot recover omitted context: identical feature snapshots cannot reliably distinguish different underlying costs. Category hashing preserves identity only approximately and does not preserve semantic similarity. Text embeddings are outside the initial representation.

Store ingress features, prediction or its absence, selected priority, versions, mode, and fallback reason internally. This snapshot survives aging, retries, in-flight placement, and DLQ. Consumers remain tier-blind; ML context is not placed in their headers or Pull responses.

### Learn from accepted outcomes

Feedback is an event history keyed by broker instance and message ID, with separate delivery-attempt and event identities. A message can have several retries followed by Ack or DLQ. Do not overwrite that history with one latest row.

Allocate the internal delivery ordinal only as part of successful ownership installation after the final Pull cancellation check, under the settlement mutex. Cancelled Pull restoration does not consume an ordinal or emit a delivery event. The ordinal is independent of retry count and remains associated with that delivery's outcome.

Record actual outcome separately from requested operation. Ack-after-TTL can return OK while producing a TTL DLQ. Lease/queued expiry has no measured handler duration. Replayed accepted settlements emit no new event. Rejected owners/tokens supply no handler labels; a valid lease-expiry transition caused by a late, correctly owned settlement still emits the expiry event.

Initially train only on valid successful-handler Ack observations. Retain failed/censored events for analysis. Zero milliseconds is valid timer truncation; negative or implausible measurements are unusable labels and do not change existing settlement statuses. Evaluate the original stored prediction before applying its delayed label. Document that consumer hardware and success-only sampling can bias the target.

Use the ordered label-status rules in the [ML contract](../ml-contract.md): missing observation, negative/out-of-range measurement, unusable features, actual TTL DLQ, Nack failure, then successful Ack eligibility. TTL DLQ takes precedence over Nack failure when the measurement/features are valid; keep actual outcome and feature validity independently observable.

### Persist bounded telemetry

The first proposed backend is rotated append-only JSONL with one asynchronous writer. Each outcome includes its bounded ingress context, so it remains interpretable if its ingress event is missing. The detailed capacity/retention/flush policy is in the [ML contract](../ml-contract.md).

Capture a bounded event at the successful state-transition boundary before moving the message, then attempt nonblocking admission to telemetry. Disk/network work happens outside broker locks. On overflow or disk failure preserve broker settlement, drop telemetry as necessary, and expose counters. There is no transaction between in-memory settlement and log append.

Persisted feedback does not make message delivery durable or side effects exactly once. Crash loss includes unadmitted, buffered, and unsynced records. The healthy sync interval is a target, not a hard loss bound during filesystem failure. Retention may remove unconsumed data and is reported. #8 must checkpoint the model with its consumed-log cursor/deduplication state to avoid learning twice after replay.

Shutdown stops telemetry admission only after broker handlers and maintenance can no longer emit events. The proposed drain target is one second within the shutdown workflow. Drop remaining buffered work on drain-budget exhaustion, but never detach a writer that references destroyed objects. A blocking filesystem syscall cannot be interrupted by an ordinary thread deadline: an in-process writer may exceed the target while joining. #3 must document this limitation or isolate storage if a hard shutdown bound is required; the existing server's five-second RPC grace is not a guarantee that arbitrary storage I/O completes.

### Gate activation on scheduling evidence

The small synthetic evaluation harness is required in Phase 2. Compare FIFO, static priority, and round-robin with identical workloads and explicit aging/TTL/lease configurations. Prediction accuracy alone does not establish latency benefit. Measure latency, throughput, inference overhead, drops, incomplete messages, and per-class starvation. Gate per-class longest first-dispatch wait separately so a small set of delayed long jobs cannot hide behind an aggregate starvation rate; numerical allowances remain proposed until baseline feasibility checks.

Freeze the final numerical gates and experimental configuration after baseline feasibility checks and before predictive evaluation. A failed gate keeps experiments in shadow/static mode with follow-up work. Real trace replay and Kafka/RabbitMQ/Pulsar comparisons remain Phase 3.

## Alternatives considered

| Alternative | Tradeoff and decision |
|---|---|
| Predict only a class | Simple routing, but loses duration error information and couples learning to a particular bucket policy; prefer duration plus mapping |
| Embedded C++ inference | Avoids RPC overhead, but increases initial model/toolchain coupling; reconsider if measured IPC overhead fails the gate |
| Custom IPC | Could reduce overhead, but adds protocol/lifecycle work; reuse gRPC for the first experiment |
| SQLite/transactional outbox | Better query/replay facilities, but cannot atomically persist existing in-memory broker state by itself; consider if JSONL loss/query limits become inadequate |
| Immediate predictive routing | Cannot separate instrumentation overhead from scheduling benefit; collect static feedback and run shadow first |
| Automatic boundary changes/retraining | Makes comparisons and rollback harder; explicit versioned policy/model changes first |

## Preserved invariants

- Proxy stamps ID, arrival time, and producer header only; clients never know queue levels.
- TTL is resolved solely at ingress and measured from original arrival, including time spent predicting. Never store `kTtlUnset` in queue/in-flight/DLQ.
- Both priority fields start at the actual ingress choice. Nack/lease requeue restores `original_priority`; aging changes current placement only.
- Keep ownership checks, attempt-token fencing, completion replay, failure budgets, and TTL precedence unchanged. Ack-after-TTL is OK only while its lease is valid.
- Keep settlement → queue/DLQ lock ordering. Queue/deadline/completion maintenance remains bounded as it is today.
- Do not train on a replayed request, a rejected request, queue wait, or invented runtime after lease expiry.

## Implementation handoff

#2 owns feature extraction/context; #3 feedback capture/storage; #4 datasets/baselines; #5 model selection; #6 transport; #7 routing; #8 checkpoint/replay/monitoring; #9 cross-component tests; #10 comparative results and operating documentation. These documents introduce no runtime behavior or dependencies.
