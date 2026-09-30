<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/brand/harbinger-lockup-dark.svg">
    <img alt="Harbinger" src="assets/brand/harbinger-lockup.svg" height="64">
  </picture>
</p>

# Harbinger — Predictive Multi-Level Message Queue

*Harbinger (formerly PMLMQ) — the queue that knows what's coming: it predicts each message's cost, then prioritizes.*

A research messaging system that combines **online ML-based processing time prediction** with **Multi-Level Feedback Queue (MLFQ)** scheduling to reduce end-to-end message latency.

**Research hypothesis:** Predicting message processing time at ingress and routing accordingly (rather than using FIFO or static priority) measurably improves P50/P95/P99 latency.

---

## Status

**Phase 1 complete.** The core gRPC broker is fully implemented: multi-level priority queues, producer/consumer clients, DLQ, aging-based starvation prevention, retry tracking, and in-flight message management. The Phase 2 ML feedback hook (`processing_time_ms` on every Ack/Nack, `route_message()` in the service) is stubbed and ready.

---

## Architecture

```
Producer::send()
      │
      ▼  gRPC Submit
 ┌─────────────────────────────────────────────────────────────────┐
 │  HarbingerService (Broker)                                      │
 │                                                                 │
 │  Proxy::accept()  →  route_message()  →  MultiLevelQueue        │
 │    stamp ID              ▲                  Level 0 (HIGH)      │
 │    arrival_time    Phase 2: ML              Level 1 (MED)       │
 │    producer_id     classifier               Level 2 (LOW)       │
 │                    goes here                     │              │
 │                                            aging thread         │
 │                                            (promotes stale msgs)│
 │                                                  │              │
 │  Consumer Pull() ◄───────────────────────────────┘              │
 │  (tier-blind)                                                   │
 │       │                                                         │
 │  Ack / Nack  ──  processing_time_ms  ──►  (Phase 2 ML feedback) │
 │       │                                                         │
 │  DeadLetterQueue (max retries / TTL expired)                    │
 └─────────────────────────────────────────────────────────────────┘
```

### Key design decisions

- **Consumers are tier-blind** — the broker picks which message each `Pull()` receives; consumers never see queue levels.
- **Proxy is narrowly scoped** — stamps ID and `arrival_time`, stores `producer_id` in headers, then hands off to `route_message()`.
- **In-flight tracking** — messages are held in an `unordered_map` between `Pull` and `Ack/Nack`; on `Nack` they are re-queued or DLQ'd.
- **Phase 2 hook** — `route_message()` in `harbinger_service.cpp` is the single injection point for the future ML classifier; `processing_time_ms` is transmitted on every `Ack`/`Nack` but is not yet stored or used for training.
- **`harbinger_rpc` proto package** — kept distinct from the `harbinger` C++ namespace to avoid symbol collisions.

---

## Documentation

- [Documentation index](docs/README.md) — choose a guide by role.
- [Producer guide](docs/producers.md) — connect, send messages, set TTL, and understand Submit outcomes.
- [Consumer guide](docs/consumers.md) — handlers, delivery leases, acknowledgements, retries, and shutdown.
- [Broker and queue internals](docs/internals.md) — state transitions, priority ordering, aging, expiry, and recovery.

---

## Repository layout

```
harbinger/
├── assets/brand/              # Logo, icon, social preview, usage guide
├── docs/                      # Producer, consumer, and broker internals guides
├── proto/harbinger.proto          # Broker service definition (package harbinger_rpc)
├── include/
│   ├── harbinger_service.hpp      # HarbingerService : harbinger_rpc::Broker::Service
│   ├── proxy/proxy.hpp
│   ├── queue/
│   │   ├── message.hpp        # Message, AgingConfig, DLQEntry
│   │   ├── multi_level_queue.hpp
│   │   └── dead_letter_queue.hpp
│   ├── producer/producer.hpp  # gRPC client: connect() → send()
│   └── consumer/consumer.hpp  # gRPC client: connect() → start() / stop()
├── src/                       # C++ implementations
├── server/main.cpp            # harbinger_server binary (SIGINT/SIGTERM graceful shutdown)
├── demo/main.cpp              # End-to-end demo (embedded broker + 2 producers + 2 consumers)
├── tests/
│   └── unit/                  # GoogleTest sources; built as harbinger_unit_tests
│                              # (queue, DLQ, proxy — no gRPC) and
│                              # harbinger_integration_tests (broker, client over gRPC)
├── ml_engine/                 # (Phase 2, planned — not yet present)
├── benchmarks/                # Opt-in Phase 1 cleanup measurements; scheduler gate planned
└── CMakeLists.txt
```

---

## Building

**Dependencies**

```bash
# Ubuntu
sudo apt install -y libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev

# macOS
brew install grpc protobuf
```

**Build**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The build prefers Protobuf and gRPC CMake CONFIG packages from a compatible
installation, with FindProtobuf fallback for the matching Ubuntu packages.
Do not mix dependency installations. For sanitizer verification, configure with
`-DHARBINGER_SANITIZER=address`, `undefined`, or `thread`.

CI runs the full suite under ASan/UBSan and queue tests under TSan. Transport
tests under TSan require a compatible, TSan-instrumented gRPC/Protobuf stack;
prebuilt libraries can hide synchronization and produce unusable reports.

`-DHARBINGER_WARNINGS_AS_ERRORS=ON` makes warnings fail first-party builds;
it defaults to OFF locally and is enabled in CI. Generated protobuf sources and
fetched dependencies are excluded from this policy. For reproducible queue and
loopback delivery measurements, enable `-DHARBINGER_BUILD_BENCHMARKS=ON` (default
OFF) and follow [the benchmark guide](benchmarks/README.md).

**Targets**

| Binary | Description |
|---|---|
| `build/harbinger_server` | Standalone broker process |
| `build/tests/harbinger_unit_tests` | Internal tests (no gRPC required) |
| `build/tests/harbinger_integration_tests` | Full gRPC round-trip tests |
| `build/demo/harbinger_demo` | End-to-end demo |

---

## Running

**Standalone broker** (default `0.0.0.0:50051`, accepts an address override as `argv[1]`):

```bash
./build/harbinger_server
# or
./build/harbinger_server 127.0.0.1:50051
```

**End-to-end demo** (embedded broker + 2 producers + 2 consumers, demonstrates retries; the DLQ path is exercised in the integration tests):

```bash
./build/demo/harbinger_demo
```

**Tests**

```bash
./build/tests/harbinger_unit_tests
./build/tests/harbinger_integration_tests
# or, when configured with CMake testing
ctest --test-dir build --output-on-failure
```

---

## Client API

```cpp
// Producer
auto producer = harbinger::Producer::connect("127.0.0.1:50051");
std::string msg_id = producer->send(
    {0x01, 0x02},                          // payload bytes
    {{"job_type", "resize"}, {"src", "a"}} // headers (used for ML features in Phase 2)
);

// Consumer
auto consumer = harbinger::Consumer::connect(
    "127.0.0.1:50051",
    [](const harbinger::ReceivedMessage& msg) -> harbinger::AckResult {
        // process msg.payload / msg.headers
        return harbinger::AckResult::SUCCESS; // or FAILURE to trigger retry / DLQ
    }
);
consumer->start();
// ...
consumer->stop();
```

---

## Configuration (`HarbingerConfig`)

| Field | Default | Description |
|---|---|---|
| `num_levels` | `3` | Number of priority queue levels (0 = highest) |
| `aging` | `nullopt` (off) | `AgingConfig{threshold 5000 ms, interval 500 ms}` when enabled; `nullopt` = strict-priority with no aging thread (`server/main.cpp` and the demo enable aging explicitly) |
| `default_max_retries` | `3` | Nack attempts before the message moves to the DLQ |
| `default_ttl` | `0` (off) | TTL from arrival; `0` = no expiry |
| `default_priority` | `1` (medium) | Static priority for Phase 1; ML classifier overrides in Phase 2 |
| `max_pull_wait` | `5000 ms` | Server-side cap on consumer pull timeout |
| `ttl_sweep_interval` | `100 ms` | Background TTL sweep interval; `0` disables it |
| `delivery_lease` | `30000 ms` | Fixed ownership lease per delivery; no renewal |
| `lease_sweep_interval` | `100 ms` | Reclaims abandoned deliveries, independent of queue TTL sweeping |
| `completion_retention` | `60000 ms` | Maximum retention for replayable Ack/Nack outcomes |
| `completion_cache_max_entries` | `10000` | Hard cap on retained delivery outcomes |
| `maintenance_batch_size` | `256` | Maximum messages reclaimed per TTL/lease batch |

Configuration is validated at broker construction. Levels and retry count must
be positive, durations must use their documented non-negative/off semantics,
and an enabled aging policy requires positive threshold and interval values.

### Standalone server recovery options

Embedded brokers can set these fields in `HarbingerConfig` before constructing
`HarbingerService`. The standalone server accepts the corresponding options:

| Option | Default |
|---|---:|
| `--delivery-lease-ms` | 30000 |
| `--lease-sweep-interval-ms` | 100 |
| `--completion-retention-ms` | 60000 |
| `--completion-cache-max-entries` | 10000 |
| `--maintenance-batch-size` | 256 |
| `--ttl-sweep-interval-ms` | 100 |

```bash
./build/harbinger_server 0.0.0.0:50051 \
  --delivery-lease-ms 120000 \
  --completion-retention-ms 300000 \
  --completion-cache-max-entries 100000
./build/harbinger_server --help
```

Omitted options retain their defaults. Values are unsigned decimal integers;
durations are milliseconds. All six settings must be positive except
`--ttl-sweep-interval-ms`, which accepts `0` to disable background queued-TTL
cleanup. Invalid, missing, out-of-range, or unknown options cause a nonzero exit
before listening. Options use `--name value` syntax and may appear before or
after the optional address; repeated options use the last value. Settings apply
at startup, and effective values are printed before the readiness message.

The example is illustrative, not a recommended production profile. Size both
retention and capacity for peak attempt-completion volume and expected settlement
delays; increasing retention alone cannot prevent capacity eviction. See the
[consumer system boundary](docs/consumers.md#system-boundary-delivery-safety-and-consumer-recovery).

### Delivery and shutdown semantics

Every Pull returns a fresh `attempt_token`; Ack/Nack must echo it. Upgrade the
broker and clients together. The bundled consumer handles tokens internally.
Expired leases recover messages abandoned by crashes or lost Pull responses.
Lease expiry counts as a failed delivery, just like Nack; an expired TTL instead
sends the message to the DLQ without incrementing its retry count. With
`default_max_retries=1`, the first failure goes to the DLQ.

Duplicate accepted Ack/Nack requests replay OK without repeating the mutation.
This guarantee lasts until completion retention expires or the cache evicts the
record, whichever happens first. Stale attempt tokens cannot settle a newer
delivery, even after cache eviction. Known conflicting or expired attempts return
`FAILED_PRECONDITION`. After history is gone, a late settlement can instead return
`NOT_FOUND` or `PERMISSION_DENIED`, depending on current delivery ownership.

The consumer retries transient settlement errors up to three times, using
5-second RPC deadlines and 100/200 ms backoff, without rerunning the handler.
Settlement `FAILED_PRECONDITION` counts once in `leases_lost()` and polling
continues without a confirmed outcome. Other final settlement errors, including
retry exhaustion, stop the worker; `last_rpc_status()` exposes the terminal error
and `rpc_failures()` counts failed settlement calls. Ack/Nack counters count only
confirmed outcomes. Pull retries use interruptible 200 ms backoff.

**Stale-token safety does not guarantee automatic consumer recovery.** Completion
history is bounded by both time and capacity; late `NOT_FOUND` or
`PERMISSION_DENIED` results remain terminal. There is no automatic worker restart.
See the [consumer system boundary](docs/consumers.md#system-boundary-delivery-safety-and-consumer-recovery)
for expected results, history sizing, and application/operator responsibilities.

Consumer destruction joins the worker; restart after a stopped worker is safe.
`stop()` cancels pending Pull, but waits for an active handler and settlement.
A handler may call `stop()` to request shutdown, but must not destroy its own
consumer. Handlers cannot be forcibly interrupted. Set the delivery lease above
the expected handler duration plus settlement overhead; lease renewal is not
implemented. Handlers must tolerate duplicate processing after lease expiry.

TTL reclamation uses a deadline index and bounded batches, avoiding full-backlog
scans on delivery. Large expired backlogs take multiple passes to reclaim;
aging skips wholly not-yet-due levels but still scans due levels fully. The standalone POSIX server handles SIGINT/SIGTERM
using `sigwait` and shuts down with a five-second grace deadline.

### Current production boundary

The broker is currently an in-memory research prototype. Before deployment
beyond a trusted local network, add queue/DLQ/payload
limits and backpressure, TLS/authentication, idempotent Submit retries,
operator authorization, and metrics/tracing. All queue, lease, and completion
state is in memory and lost on restart. Delivery recovery provides at-least-once
processing while the broker runs, not exactly-once application effects. Submit
idempotency and durable storage are not implemented.

---

## Development phases

### Phase 1 — Core queuing (complete)
- [x] gRPC broker with `Submit / Pull / Ack / Nack`
- [x] Multi-level priority queue (strict priority + optional aging)
- [x] Proxy: ID generation, arrival-time stamping
- [x] Competitive consumption (single consumer processes each message)
- [x] In-flight tracking; retry on `Nack`
- [x] Dead Letter Queue (max retries exceeded, TTL expired)
- [x] Producer and Consumer gRPC clients
- [x] Unit + integration test suite

### Phase 2 — ML integration (next)
- [ ] Feature extraction from message headers/payload metadata
- [ ] Python ML service with online learning (`river` or scikit-learn incremental estimators)
- [ ] gRPC/IPC bridge between C++ broker and Python classifier
- [ ] Predictive routing injected into `route_message()` in `harbinger_service.cpp`
- [ ] `processing_time_ms` feedback from Ack/Nack fed to classifier
- [ ] Model drift monitoring and accuracy tracking

### Phase 3 — Benchmarking & validation
- [ ] Synthetic workload generators (uniform, bimodal, heavy-tailed)
- [ ] Real-world dataset replay: Alibaba Microservices Trace (2021/2022), Azure Functions Trace
- [ ] Comparison against FIFO, static priority, round-robin baselines
- [ ] Comparison against Kafka, RabbitMQ, Pulsar

### Phase 4 — Enhancements (optional)
- [ ] Streaming mode (persistent events, multi-consumer replay)
- [ ] Multi-tenancy
- [ ] Dynamic online classifier retraining

---

## Key metrics

| Metric | Description |
|---|---|
| P50 / P95 / P99 latency | End-to-end message processing latency percentiles |
| Throughput (msg/s) | Sustained message processing rate |
| Prediction accuracy | Classifier accuracy across processing-time buckets (short / medium / long) |
| Queue starvation rate | Rate at which lower-priority queues are starved |

---

## Tech stack

- **Core:** C++20 — concepts, `std::atomic`, lock-free where possible, header-only libraries preferred
- **IPC:** gRPC + Protocol Buffers
- **ML engine:** Python — `river` (online ML) or scikit-learn with `SGDClassifier` / `PassiveAggressiveClassifier`
- **Tests:** GoogleTest (fetched via CMake `FetchContent`)
- **Metrics (planned):** Prometheus / StatsD
- **Tracing (planned):** Jaeger / Zipkin
