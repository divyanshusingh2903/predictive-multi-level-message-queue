# AGENTS.md — Harbinger

Predictive Multi-Level Message Queue: ML-predicted processing time + MLFQ routing to cut P50/P95/P99 vs FIFO/static-priority/round-robin.

## Architecture

`Producer::send → Submit → Proxy::accept → route_message → MultiLevelQueue (L0 HIGH … Ln LOW, aging promotes stale) → Pull (tier-blind) → Ack/Nack (carries processing_time_ms) → DLQ on max-retries/TTL`

- **Consumers are tier-blind.** Broker picks the message; levels never leak to clients.
- **Proxy is narrow:** ID + `arrival_time` + `__producer_id` header → forwards to sink. No queue/priority/TTL knowledge.
- **Phase 2:** `route_message()` is the sole feature/classifier hook. Opt-in static ingress capture, Python encoding, and embedded persistent feedback are implemented; classifier calls remain planned.

## Invariants (don't break)

- Priority ∈ `[0, num_levels)`, 0 = highest. `default_priority` must be `< num_levels` (throws otherwise).
- `original_priority` = submit-time priority; Nack re-queue resets `priority = original_priority`.
- `enqueue_time` = last queue placement; reset on `enqueue()` and on aging promotion. `requeue_front()` rolls back an uncommitted dequeue and preserves this timestamp and all message metadata.
- TTL measured from `arrival_time`; `0` = off. Expired-at-Pull → DLQ (`TTL_EXPIRED`), keep scanning.
- Per-message TTL: `SubmitRequest.ttl_ms` unset → `default_ttl`, `0` = explicitly off, `>0` = TTL, `<0` → `INVALID_ARGUMENT`. `kTtlUnset` flows Proxy → `route_message` only, never stored; `route_message` is the sole resolver.
- Expiry checkpoints: selected-message `Pull` check, `sweep_expired_batch()` (Pull-path + `ttl_sweep_interval` sweeper, `0` = off), `Nack`/`Ack` after ownership checks. The full `sweep_expired()` API is diagnostic/test-oriented. `Nack`-after-expiry DLQs `TTL_EXPIRED` without touching `retry_count`; `Ack`-after-expiry DLQs but still returns OK. Aging never promotes expired messages.
- DLQ: `snapshot(offset, limit)` is non-destructive; `InspectDlq` RPC is read-only, paginated (limit `<=0` → 10, cap 100), no-auth operator endpoint; `dlq_age_ms` (steady clock, no wall-time form). DLQ never redelivers.
- In-flight: each Pull issues a new opaque `attempt_token` and fixed lease. Ack/Nack must echo that token; no legacy-client compatibility. Wrong owner → `PERMISSION_DENIED`, stale/conflicting attempt → `FAILED_PRECONDITION`, unknown delivery → `NOT_FOUND`, invalid token → `INVALID_ARGUMENT`.
- Lease expiry reclaims abandoned deliveries even with the queue TTL sweeper off. Expired TTL wins without incrementing retries; otherwise expiry consumes the same failure budget as Nack and requeues at original priority or DLQs. Ack-after-TTL is OK only while its lease remains valid. No lease renewal; size leases for handler duration plus settlement overhead.
- Settlement is serialized under the in-flight mutex (lock order: settlement → queue/DLQ). A bounded completion map/list replays identical accepted Ack/Nack as OK, including after redelivery. Opposite operations fail. Replay lasts until retention expiry or capacity eviction; tokens still fence stale attempts after eviction. This is at-least-once delivery, not exactly-once side effects; no state survives broker restart.
- TTL index contains only queued positive-TTL messages; stable FIFO nodes retain index handles through aging. Pull/background cleanup uses `sweep_expired_batch(maintenance_batch_size)` in deadline order; the full `sweep_expired()` API preserves level/FIFO order. Live/TTL-off messages are not scanned on Pull. Aging caches conservative per-level promotion deadlines and skips wholly young levels; due levels remain full FIFO scans. Dequeue can leave an earlier stale deadline; placement/restoration/promotion must never cache a later deadline than eligible live work. Unrepresentable aging deadlines are unreachable, not wrapped.
- Aging: only levels `≥1` promote one step per scan; `enqueue_time` resets so threshold is per-entry. `shutdown()` unblocks `dequeue()` + joins thread.
- `Pull`: `wait = min(requested>0 ? requested : max_pull_wait, max_pull_wait)`; polls in 100 ms chunks checking `IsCancelled()`. No message → `timed_out=true`.
- Pull rechecks cancellation under the in-flight mutex immediately before ownership installation. A live selected message for an already-cancelled call is restored to the front of its current level without creating a lease/history entry or consuming retries; TTL is checked first. Front restoration cannot undo concurrent deliveries or guarantee global FIFO across concurrent restorations. Cancellation/response loss after the final check still uses normal lease recovery and its failure budget.
- Consumer poll deadline = `pull_timeout + 500 ms`; UNAVAILABLE/DEADLINE_EXCEEDED Pull errors back off 200 ms (interruptible), permanent errors stop polling. Producer and consumer registration have a 5-second deadline.
- Consumer destructor stops/joins, restart joins an exited worker, concurrent stop has one join owner. Handler-thread stop only requests stop; never destroy the consumer from its own handler. External stop cancels pending Pull but waits for a running handler and its settlement. Arbitrary handlers cannot be interrupted.
- Ack/Nack retries echo the identical attempt/request, at most three calls with 5-second deadlines and 100/200 ms backoff on UNAVAILABLE/DEADLINE_EXCEEDED. Settlement FAILED_PRECONDITION counts once in `leases_lost()` and resumes polling without a confirmed outcome; other errors remain terminal. Counters record one confirmed outcome; `rpc_failures` counts failed settlement calls; `last_rpc_status()` exposes only the terminal error. No handler rerun merely to retry settlement.
- Recovery boundary: stale tokens remain fenced after completion-history eviction, but late settlement may then return terminal NOT_FOUND or PERMISSION_DENIED. Retention starts at recorded settlement/reclamation and capacity can shorten it; no guaranteed replay window, automatic consumer restart, or re-registration. User-facing expectations and responsibilities live in `docs/consumers.md` under "System boundary: delivery safety and consumer recovery".
- Server blocks SIGINT/SIGTERM before spawning threads, waits with sigwait, and shuts down with a 5-second grace deadline outside signal context.
- ID format: `<ns-timestamp>-<counter>`. Producer key stored as `__producer_id` header.
- Proto package `harbinger_rpc` ≠ C++ namespace `harbinger` (avoids collisions).

## Config defaults (`HarbingerConfig`)

`num_levels=3`, `aging=nullopt` (strict-priority; use `{5000 ms, 500 ms}` to enable aging), `default_max_retries=3`, `default_ttl=0`, `default_priority=1`, `max_pull_wait=5000 ms`, `ttl_sweep_interval=100 ms` (`0` = off).

`ingress_features=nullopt` disables capture. Embedded brokers may supply `ml::IngressFeatureConfig` with a schema and caller-declared static policy version; this alone enables no persistence/inference. `Message::routing_context` is `shared_ptr<const ml::RoutingContext>` and must survive all message moves/restoration/aging/retry/DLQ copies without leaking to RPCs. Disabled context has absent model/prediction/fallback/timing fields; oversized complete snapshots have null features plus `FeatureLimit`.

`feedback=nullopt` disables persistence. Enabled `ml::FeedbackConfig` requires explicit `ingress_features` and an existing dedicated directory: 4096 records/32 MiB pending (including writer-owned work), 16 KiB event JSON plus newline accounting, 64 MiB segments, 1 GiB/7-day sealed retention, 256 total segments, 1-second sync/drain targets. POSIX writer owns the directory exclusively; no standalone schema/feedback flags. Capture before moves and publish only after successful transitions, outside broker locks. Nonblocking admission drops on contention/overflow; runtime disk failures isolate telemetry and count loss. Stale/damaged active segments are preserved as suspect; never append/retry ambiguous records into another segment. Quiesce callers before destruction; maintenance joins before writer close/drain/join. Blocked syscalls can exceed the drain target. Details: `docs/feedback.md`.

Recovery defaults: `delivery_lease=30000 ms`, `lease_sweep_interval=100 ms`, `completion_retention=60000 ms`, `completion_cache_max_entries=10000`, `maintenance_batch_size=256`. All must be positive; extreme maintenance durations are rejected before clock conversion. Lease, queue TTL, and completion-history maintenance use bounded batches; reclamation delay grows with backlog.

Standalone server startup overrides: `--delivery-lease-ms`, `--lease-sweep-interval-ms`, `--completion-retention-ms`, `--completion-cache-max-entries`, `--maintenance-batch-size`, `--ttl-sweep-interval-ms` (unsigned decimal values, TTL sweep alone permits 0). Omitted options use `HarbingerConfig` defaults; server still enables aging explicitly. `--help` lists defaults; invalid configuration exits nonzero before listening. No runtime reload.

## Phase 2 contract

- Start in shadow mode: calculate and record a prediction, but keep static-priority routing until benchmarked against the baseline.
- Version the feature schema and exclude raw payloads by default; headers must be explicitly allowlisted before they are recorded or sent to a classifier.
- Persist feedback keyed by message ID with the routing version, predicted bucket, measured `processing_time_ms`, and terminal outcome (Ack, retry, or DLQ reason).
- Bound classifier calls with a short deadline. Any timeout, unavailable classifier, invalid prediction, or priority outside `[0, num_levels)` falls back to `default_priority`.
- Compare prediction error, P50/P95/P99, throughput, and starvation against FIFO, static-priority, and round-robin workloads before enabling predictive routing.

Design details live in [ADR 0001](docs/adr/0001-phase2-ml-contract.md), the [ML contract](docs/ml-contract.md), and the [validation plan](docs/phase2-validation.md). [Feature capture/encoding](ml_engine/README.md) and [feedback persistence](docs/feedback.md) are implemented; inference and performance validation remain planned.

- Modes: disabled/static by default, shadow for initial prediction experiments, predictive only by explicit opt-in after the gate. Feedback collection is separately configured so static-mode collection works without Python.
- Predict successful handler duration, then let C++ map it to fixed versioned boundaries. Current `uint8_t` levels support 1–255; exactly `num_levels - 1` positive increasing boundaries, equality enters the next bucket. One level requires default priority 0 and no boundaries.
- Store immutable ingress features and routing context internally; never put predictions/priorities into client headers. Disabled/shadow assign both priority fields to `default_priority`; predictive assigns both to the validated bucket or fallback. Retry/aging do not reclassify.
- Separate protocol/feature-schema/model/routing-policy versions. Changing allowlists or encodings changes the schema; changing boundaries changes the policy. No silent compatibility migration.
- Numeric headers use strict locale-independent decimal grammar and binary64; payload size stays an exact integer until model conversion. Prefer bounded fixed vocabularies for small categorical sets; optional hashing uses sparse one-hot bins. Missing and unknown are distinct, category indices are never numeric features, and encoding/vocabulary changes require a new schema version. Workload metadata supplies context; hashing does not preserve semantic meaning.
- Feature-limit admission uses identical bounded JSON in C++/Python: fixed object order, sorted map keys, explicit control escapes, and 17-significant-digit scientific binary64 values. Shared fixtures cover conversion bits, encoding, privacy, and exact 8 KiB boundaries. Python 3.10+ tests run via CTest when available; CI requires `HARBINGER_REQUIRE_PYTHON_TESTS=ON`.
- Capture actual outcome events, keyed by broker instance/message/event and internal attempt identity, before moving message state. Never export opaque settlement tokens or arbitrary failure text. Accepted replay emits no second training event; rejected requests supply no labels. Actual lease reclamation still emits its own event.
- Internal delivery ordinals increment only with successful ownership installation under the settlement mutex after the final cancellation check. Cancelled Pull restoration emits no delivery event and consumes no ordinal; attempt identity is independent of retry count.
- Zero duration is valid; missing duration is null. Train initially on valid successful Ack labels only, evaluating the stored prediction before learning. Ack-after-TTL is DLQ feedback; lease/queued expiry has no handler runtime.
- Label precedence: missing observation, negative measurement, out-of-range measurement, unusable features, TTL DLQ censorship, Nack failure, then successful Ack eligibility. Keep actual outcome and feature validity separate; valid Nack-after-TTL is censored.
- Telemetry admission and retention are bounded. No classifier/filesystem I/O under broker locks. Overflow/disk failure preserves settlement and reports telemetry loss. Persistent feedback is neither durable broker delivery nor an atomic settlement/log transaction.
- The Phase 2 synthetic gate precedes predictive activation; Phase 3 expands to real traces/external brokers. Freeze final numerical budgets and experiment configuration before predictive comparisons, including per-class longest-wait and starvation-rate guardrails.

## Build / Run / Test

```bash
# deps (Ubuntu): sudo apt install -y libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev
# deps (macOS):  brew install grpc protobuf
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
./build/harbinger_server [addr]          # default 0.0.0.0:50051
./build/demo/harbinger_demo              # embedded broker on 127.0.0.1:50099
./build/tests/harbinger_unit_tests; ./build/tests/harbinger_integration_tests
```

Targets: `harbinger_server`, `harbinger_unit_tests`, `harbinger_integration_tests`, `demo/harbinger_demo`. GTest via `FetchContent`.

`HARBINGER_WARNINGS_AS_ERRORS` defaults OFF; CI enables it for first-party targets only, excluding generated protobuf and fetched dependencies. `HARBINGER_BUILD_BENCHMARKS` defaults OFF; enables `benchmarks/harbinger_cleanup_benchmark`, documented in `benchmarks/README.md`. Benchmarks report measured outcomes without timing gates in tests.

## Layout

`proto/harbinger.proto` · `include/{harbinger_service,proxy,queue/{message,multi_level_queue,dead_letter_queue},producer,consumer}.hpp` · `include/ml/` · `src/` mirrors `include/` · `server/main.cpp` · `demo/main.cpp` · `tests/unit/` · `tests/fixtures/ml/` · `docs/adr/` · `ml_engine/` (features implemented; model/service planned) · `benchmarks/` (Phase 2 gate + Phase 3 expansion, planned)

## Conventions

C++20; `std::atomic`/lock-free on hot paths; keep public headers one-line `///` docs only (details live here); no decorative banner comments; Doxygen not required per-line.
