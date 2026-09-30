# AGENTS.md — Harbinger

Predictive Multi-Level Message Queue: ML-predicted processing time + MLFQ routing to cut P50/P95/P99 vs FIFO/static-priority/round-robin.

## Architecture

`Producer::send → Submit → Proxy::accept → route_message → MultiLevelQueue (L0 HIGH … Ln LOW, aging promotes stale) → Pull (tier-blind) → Ack/Nack (carries processing_time_ms) → DLQ on max-retries/TTL`

- **Consumers are tier-blind.** Broker picks the message; levels never leak to clients.
- **Proxy is narrow:** ID + `arrival_time` + `__producer_id` header → forwards to sink. No queue/priority/TTL knowledge.
- **Phase 2 hooks (stubbed):** `route_message()` in `src/harbinger_service.cpp` is the single ML-classifier injection point; `processing_time_ms` on every Ack/Nack feeds training.

## Invariants (don't break)

- Priority ∈ `[0, num_levels)`, 0 = highest. `default_priority` must be `< num_levels` (throws otherwise).
- `original_priority` = submit-time priority; Nack re-queue resets `priority = original_priority`.
- `enqueue_time` = last queue placement; reset on `enqueue()` and on aging promotion.
- TTL measured from `arrival_time`; `0` = off. Expired-at-Pull → DLQ (`TTL_EXPIRED`), keep scanning.
- Per-message TTL: `SubmitRequest.ttl_ms` unset → `default_ttl`, `0` = explicitly off, `>0` = TTL, `<0` → `INVALID_ARGUMENT`. `kTtlUnset` flows Proxy → `route_message` only, never stored; `route_message` is the sole resolver.
- Expiry checkpoints: `Pull` scan, `sweep_expired()` (Pull-path + `ttl_sweep_interval` sweeper, `0` = off), `Nack`/`Ack` after ownership checks. `Nack`-after-expiry DLQs `TTL_EXPIRED` without touching `retry_count`; `Ack`-after-expiry DLQs but still returns OK. Aging never promotes expired messages.
- DLQ: `snapshot(offset, limit)` is non-destructive; `InspectDlq` RPC is read-only, paginated (limit `<=0` → 10, cap 100), no-auth operator endpoint; `dlq_age_ms` (steady clock, no wall-time form). DLQ never redelivers.
- In-flight: each Pull issues a new opaque `attempt_token` and fixed lease. Ack/Nack must echo that token; no legacy-client compatibility. Wrong owner → `PERMISSION_DENIED`, stale/conflicting attempt → `FAILED_PRECONDITION`, unknown delivery → `NOT_FOUND`, invalid token → `INVALID_ARGUMENT`.
- Lease expiry reclaims abandoned deliveries even with the queue TTL sweeper off. Expired TTL wins without incrementing retries; otherwise expiry consumes the same failure budget as Nack and requeues at original priority or DLQs. Ack-after-TTL is OK only while its lease remains valid. No lease renewal; size leases for handler duration plus settlement overhead.
- Settlement is serialized under the in-flight mutex (lock order: settlement → queue/DLQ). A bounded completion map/list replays identical accepted Ack/Nack as OK, including after redelivery. Opposite operations fail. Replay lasts until retention expiry or capacity eviction; tokens still fence stale attempts after eviction. This is at-least-once delivery, not exactly-once side effects; no state survives broker restart.
- TTL index contains only queued positive-TTL messages; stable FIFO nodes retain index handles through aging. Pull/background cleanup uses `sweep_expired_batch(maintenance_batch_size)` in deadline order; the full `sweep_expired()` API preserves level/FIFO order. Live/TTL-off messages are not scanned on Pull. Aging remains a full scan.
- Aging: only levels `≥1` promote one step per scan; `enqueue_time` resets so threshold is per-entry. `shutdown()` unblocks `dequeue()` + joins thread.
- `Pull`: `wait = min(requested>0 ? requested : max_pull_wait, max_pull_wait)`; polls in 100 ms chunks checking `IsCancelled()`. No message → `timed_out=true`.
- Consumer poll deadline = `pull_timeout + 500 ms`; UNAVAILABLE/DEADLINE_EXCEEDED Pull errors back off 200 ms (interruptible), permanent errors stop polling. Registration has a 5-second deadline.
- Consumer destructor stops/joins, restart joins an exited worker, concurrent stop has one join owner. Handler-thread stop only requests stop; never destroy the consumer from its own handler. External stop cancels pending Pull but waits for a running handler and its settlement. Arbitrary handlers cannot be interrupted.
- Ack/Nack retries echo the identical attempt/request, at most three calls with 5-second deadlines and 100/200 ms backoff on UNAVAILABLE/DEADLINE_EXCEEDED. Settlement FAILED_PRECONDITION counts once in `leases_lost()` and resumes polling without a confirmed outcome; other errors remain terminal. Counters record one confirmed outcome; `rpc_failures` counts failed settlement calls; `last_rpc_status()` exposes only the terminal error. No handler rerun merely to retry settlement.
- Recovery boundary: stale tokens remain fenced after completion-history eviction, but late settlement may then return terminal NOT_FOUND or PERMISSION_DENIED. Retention starts at recorded settlement/reclamation and capacity can shorten it; no guaranteed replay window, automatic consumer restart, or re-registration. User-facing expectations and responsibilities live in `docs/consumers.md` under "System boundary: delivery safety and consumer recovery".
- Server blocks SIGINT/SIGTERM before spawning threads, waits with sigwait, and shuts down with a 5-second grace deadline outside signal context.
- ID format: `<ns-timestamp>-<counter>`. Producer key stored as `__producer_id` header.
- Proto package `harbinger_rpc` ≠ C++ namespace `harbinger` (avoids collisions).

## Config defaults (`HarbingerConfig`)

`num_levels=3`, `aging=nullopt` (strict-priority; use `{5000 ms, 500 ms}` to enable aging), `default_max_retries=3`, `default_ttl=0`, `default_priority=1`, `max_pull_wait=5000 ms`, `ttl_sweep_interval=100 ms` (`0` = off).

Recovery defaults: `delivery_lease=30000 ms`, `lease_sweep_interval=100 ms`, `completion_retention=60000 ms`, `completion_cache_max_entries=10000`, `maintenance_batch_size=256`. All must be positive; extreme maintenance durations are rejected before clock conversion. Lease, queue TTL, and completion-history maintenance use bounded batches; reclamation delay grows with backlog.

Standalone server startup overrides: `--delivery-lease-ms`, `--lease-sweep-interval-ms`, `--completion-retention-ms`, `--completion-cache-max-entries`, `--maintenance-batch-size`, `--ttl-sweep-interval-ms` (unsigned decimal values, TTL sweep alone permits 0). Omitted options use `HarbingerConfig` defaults; server still enables aging explicitly. `--help` lists defaults; invalid configuration exits nonzero before listening. No runtime reload.

## Phase 2 contract

- Start in shadow mode: calculate and record a prediction, but keep static-priority routing until benchmarked against the baseline.
- Version the feature schema and exclude raw payloads by default; headers must be explicitly allowlisted before they are recorded or sent to a classifier.
- Persist feedback keyed by message ID with the routing version, predicted bucket, measured `processing_time_ms`, and terminal outcome (Ack, retry, or DLQ reason).
- Bound classifier calls with a short deadline. Any timeout, unavailable classifier, invalid prediction, or priority outside `[0, num_levels)` falls back to `default_priority`.
- Compare prediction error, P50/P95/P99, throughput, and starvation against FIFO, static-priority, and round-robin workloads before enabling predictive routing.

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

## Layout

`proto/harbinger.proto` · `include/{harbinger_service,proxy,queue/{message,multi_level_queue,dead_letter_queue},producer,consumer}.hpp` · `src/` mirrors `include/` · `server/main.cpp` · `demo/main.cpp` · `tests/unit/` · `ml_engine/` (Phase 2) · `benchmarks/` (Phase 3)

## Conventions

C++20; `std::atomic`/lock-free on hot paths; keep public headers one-line `///` docs only (details live here); no decorative banner comments; Doxygen not required per-line.
