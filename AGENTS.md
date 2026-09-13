# AGENTS.md — PMLMQ

Predictive Multi-Level Message Queue: ML-predicted processing time + MLFQ routing to cut P50/P95/P99 vs FIFO/static-priority/round-robin.

## Architecture

`Producer::send → Submit → Proxy::accept → route_message → MultiLevelQueue (L0 HIGH … Ln LOW, aging promotes stale) → Pull (tier-blind) → Ack/Nack (carries processing_time_ms) → DLQ on max-retries/TTL`

- **Consumers are tier-blind.** Broker picks the message; levels never leak to clients.
- **Proxy is narrow:** ID + `arrival_time` + `__producer_id` header → forwards to sink. No queue/priority/TTL knowledge.
- **Phase 2 hooks (stubbed):** `route_message()` in `src/pmlmq_service.cpp` is the single ML-classifier injection point; `processing_time_ms` on every Ack/Nack feeds training.

## Invariants (don't break)

- Priority ∈ `[0, num_levels)`, 0 = highest. `default_priority` must be `< num_levels` (throws otherwise).
- `original_priority` = submit-time priority; Nack re-queue resets `priority = original_priority`.
- `enqueue_time` = last queue placement; reset on `enqueue()` and on aging promotion.
- TTL measured from `arrival_time`; `0` = off. Expired-at-Pull → DLQ (`TTL_EXPIRED`), keep scanning.
- In-flight: owned by one consumer (`Pull` → map, `Ack` deletes, `Nack` re-queues or DLQs). Wrong owner → `PERMISSION_DENIED`; unknown ID → `NOT_FOUND`/`PERMISSION_DENIED`.
- Aging: only levels `≥1` promote one step per scan; `enqueue_time` resets so threshold is per-entry. `shutdown()` unblocks `dequeue()` + joins thread.
- `Pull`: `wait = min(requested>0 ? requested : max_pull_wait, max_pull_wait)`; polls in 100 ms chunks checking `IsCancelled()`. No message → `timed_out=true`.
- Consumer poll deadline = `pull_timeout + 500 ms`; transient Pull errors back off 200 ms.
- ID format: `<ns-timestamp>-<counter>`. Producer key stored as `__producer_id` header.
- Proto package `pmlmq_rpc` ≠ C++ namespace `pmlmq` (avoids collisions).

## Config defaults (`PMLMQConfig`)

`num_levels=3`, `aging={5000 ms, 500 ms}` (`nullopt` = strict-priority), `default_max_retries=3`, `default_ttl=0`, `default_priority=1`, `max_pull_wait=5000 ms`.

## Build / Run / Test

```bash
# deps (Ubuntu): sudo apt install -y libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev
# deps (macOS):  brew install grpc protobuf
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
./build/pmlmq_server [addr]          # default 0.0.0.0:50051
./build/demo/pmlmq_demo              # embedded broker on 127.0.0.1:50099
./build/tests/pmlmq_unit_tests; ./build/tests/pmlmq_integration_tests
```

Targets: `pmlmq_server`, `pmlmq_unit_tests`, `pmlmq_integration_tests`, `demo/pmlmq_demo`. GTest via `FetchContent`.

## Layout

`proto/pmlmq.proto` · `include/{pmlmq_service,proxy,queue/{message,multi_level_queue,dead_letter_queue},producer,consumer}.hpp` · `src/` mirrors `include/` · `server/main.cpp` · `demo/main.cpp` · `tests/unit/` · `ml_engine/` (Phase 2) · `benchmarks/` (Phase 3)

## Conventions

C++20; `std::atomic`/lock-free on hot paths; keep public headers one-line `///` docs only (details live here); no decorative banner comments; Doxygen not required per-line.
