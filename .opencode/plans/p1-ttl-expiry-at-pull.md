# P1 — TTL-Expiry-at-Pull: Tests + Hardening

## Goal
Make TTL expiry correct, observable, and covered. Today expiry is lazy-only
(checked solely in `Pull`), per-message TTL is impossible (`Submit` has no TTL
field), never-pulled expired messages leak, and zero end-to-end tests exist.

## Scope (decided: full hardening)
Tests + behavior in one slice. If you'd rather split it, the plan is ordered
so steps 1–2 (tests for current behavior) can land alone.

## Background (verified against code)
- `Message::is_expired()` (`include/queue/message.hpp`): `ttl == 0` means off;
  otherwise `steady_clock::now() - arrival_time >= ttl`. The TTL clock is
  `arrival_time`, stamped once by `Proxy::accept` and never reset (not on
  requeue, not on aging promotion). The aging clock is the separate
  `enqueue_time`.
- `Proxy::accept` (`src/proxy/proxy.cpp`) is narrow by contract: stamps ID +
  `arrival_time`, passes through to `route_message`, knows nothing of TTL.
- `route_message` (`src/pmlmq_service.cpp`) is the single routing point: sets
  `priority`/`original_priority`/`max_retries`, applies `default_ttl` when the
  message carries none, enqueues.
- `Pull` (`src/pmlmq_service.cpp`) is the ONLY expiry checkpoint: polls the
  queue in 100 ms chunks until the deadline, DLQs expired messages as
  `TTL_EXPIRED`, keeps scanning. Consequences:
  1. Expired messages nobody pulls sit in the queue forever, and the aging
     thread keeps promoting them toward L0 (wasted work, live-message delay).
  2. A `Nack` after TTL requeues a dead message (extra round trip), or worse,
     the DLQ reason becomes `MAX_RETRIES_EXCEEDED` instead of `TTL_EXPIRED`.
  3. `ttl == 0` conflates "unset" with "explicitly off", so a non-zero
     `default_ttl` cannot be overridden per message.
- No DLQ read path exists (`queue_size`/`dlq_size`/`in_flight_count` only), so
  the DLQ reason is unverifiable from outside.
- Coverage: queue tests (14) have no TTL; DLQ tests (8) use `TTL_EXPIRED` as an
  opaque enum; broker (11) + client (13) tests assert DLQ size only for
  max-retries. No TTL-expiry-at-Pull test exists.

## Design decisions
1. **Per-message TTL via `optional ttl_ms` on `SubmitRequest`** (field 4, free).
   Absent = use `default_ttl`; present `0` = explicitly off; present `>0` = TTL;
   present `<0` = `INVALID_ARGUMENT`. Internally a `kTtlUnset{-1}` sentinel
   flows Proxy → `route_message` only (never stored), so `Message` keeps its
   shape and `is_expired()` treats `<= 0` as off — a leaked sentinel can never
   falsely expire.
2. **Proxy stays narrow.** It gains an optional TTL passthrough parameter and
   applies no defaults and no config reads; `route_message` remains the only
   place defaults (and later the ML classifier) apply.
3. **Reclamation without layering violations.** New `MultiLevelQueue::
   sweep_expired()` primitive returns expired messages (highest→lowest level);
   the queue never touches the DLQ. The service drains it from two places: a
   small sweeper thread (backstop for idle consumers, default 100 ms,
   `nullopt` disables) and the top of each `Pull` poll iteration (keeps tests
   deterministic, no sweeper-tick sleeps).
4. **Aging and expiry stay on separate clocks in one locked pass.**
   `run_aging` is refactored to expire-by-`arrival_time` and
   promote-by-`enqueue_time` in a single scan, so expired messages stop
   polluting L0.
5. **Nack checks expiry BEFORE retry accounting** (ownership checks still
   first — expiry must never mask auth bugs). An expired in-flight message
   DLQs as `TTL_EXPIRED` without inflating `retry_count`. Ack-after-expiry
   stays `OK` with no DLQ move: the work completed and the consumer holds the
   only copy; burying it would double-count.
6. **Read-only `InspectDlq` RPC** (paginated, capped) so tests and operators
   can assert DLQ reasons. No replay in P1 — the DLQ never redelivers.
   `dlq_time` is steady-clock, so the RPC exposes age, not a fake wall time.
   No-auth operator endpoint (documented; revisit if you want gating).

## Files affected
- `proto/pmlmq.proto` — `ttl_ms`, `DlqReason`/`DlqEntry`/`InspectDlq*`, RPC
- `include/queue/message.hpp` — `kTtlUnset`, `is_expired() <= 0`
- `include/proxy/proxy.hpp`, `src/proxy/proxy.cpp` — optional TTL passthrough
- `include/producer/producer.hpp`, `src/producer/producer.cpp` — `send()`
  overload (defaulted, source-compatible), client-side `< 0` reject
- `src/pmlmq_service.cpp`, `include/pmlmq_service.hpp` — `Submit` validation,
  `route_message` sentinel resolution, `Nack` expiry-first, sweeper thread +
  config, `Pull`-path sweep, `InspectDlq` handler
- `include/queue/multi_level_queue.hpp`, `src/queue/multi_level_queue.cpp` —
  `sweep_expired()`, single-pass aging
- `include/queue/dead_letter_queue.hpp`, `src/queue/dead_letter_queue.cpp` —
  `snapshot(offset, limit)`
- `tests/unit/test_broker.cpp` — ~14 new tests (below)
- `AGENTS.md`, `demo/main.cpp` — invariant notes + TTL example

## Steps (each independently reviewable, main stays green)
1. **Proto + `Message` + DLQ snapshot + `InspectDlq` skeleton.** Regenerate via
   existing CMake codegen. Tests: presence bits, snapshot pagination/cap.
2. **Proxy + Producer overloads** (defaulted args, old signatures keep
   working). Tests: proxy unit (unset→sentinel, value passthrough), producer
   sets `has_ttl_ms`.
3. **`Submit` validation + `route_message` resolution.** Tests: unset→default,
   explicit-0 beats default, per-message beats off-default, negative→
   `INVALID_ARGUMENT` with `queue_size == 0`.
4. **`sweep_expired()` + aging single-pass.** Unit tests directly on the queue:
   mixed TTLs across levels, only-expired returned in level order, size
   accounting, expired never promoted.
5. **Sweeper thread + `Pull`-path sweep.** Tests: 5 never-pulled messages →
   `queue 0, dlq 5` with no `Pull`; aging-on + TTL → expired never reaches L0.
6. **Nack-expiry-first + Ack-still-OK.** Tests via `InspectDlq`: Nack-after-TTL
   → `TTL_EXPIRED` (not `MAX_RETRIES`); Nack-before-TTL requeues and delivers;
   Ack-after-TTL → `OK`, `dlq 0`; `retry_count` untouched on expiry path.
7. **Core expiry-at-Pull cases** (local-service pattern from
   `NackExceedingMaxRetriesSendsToDLQ`; `ttl = 100 ms`, sleep 300–400 ms,
   `Pull` 500–1000 ms, client deadline 3 s):
   - expired → `timed_out`, `dlq 1`, `queue 0`, `in_flight 0`;
   - `TTL = 0` control still delivers after 300 ms;
   - expired head skipped, fresh message delivered, second `Pull` times out;
   - 3 expired drained in a single `Pull` (`dlq 3`).
8. **Docs**: `AGENTS.md` invariants (sentinel lifetime, clock discipline, Nack
   precedence, `InspectDlq` cap) + TTL example in demo.

## Explicitly out of scope
Per-message priority API; ML hook shape; consumer tier-blindness (no
level/TTL in `PullResponse`); `arrival_time` immutability; DLQ replay;
`processing_time_ms` feedback (Phase 2).

## Risks
- Conda `protoc` 29.3 shadows `/usr/local` 31.1 on default PATH — configure
  and build with sanitized PATH (`/usr/local/bin` first). `optional` fields
  need protoc ≥ 3.15; 31.1 is fine.
- Timing flake: assert `timed_out` + counters only, never elapsed time; rerun
  new tests 3×. `ttl < 50 ms` or sleep `< 3× TTL` is flaky territory.
- Sweeper lifecycle: join before queue shutdown in dtor; `nullopt` disables.
- Build with sanitized PATH and `-DCMAKE_PREFIX_PATH=/usr/local` (per P0).

## Verification
- Clean rebuild exits 0 (proto regen included), no new warnings.
- `./build/tests/pmlmq_integration_tests --gtest_filter='BrokerTest.*'`:
  11 existing + ~14 new pass, 3 consecutive runs.
- `ctest --test-dir build` fully green; `./build/demo/pmlmq_demo` ends with
  queue 0 / in-flight 0.
- `InspectDlq` reason assertions distinguish `TTL_EXPIRED` from
  `MAX_RETRIES_EXCEEDED` on the Nack-path tests.
