# PMLMQ — Predictive Multi-Level Message Queue

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
 │  PMLMQService (Broker)                                          │
 │                                                                 │
 │  Proxy::accept()  →  route_message()  →  MultiLevelQueue        │
 │    stamp ID              ▲                  Level 0 (HIGH)      │
 │    arrival_time    Phase 2: ML              Level 1 (MED)       │
 │    producer_id     classifier               Level 2 (LOW)       │
 │                    goes here                     │              │
 │                                            aging thread         │
 │                                            (promotes stale msgs)│
 │                                                  │              │
 │  Consumer Pull() ◄───────────────────────────────┘             │
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
- **Phase 2 hook** — `route_message()` in `pmlmq_service.cpp` is the single injection point for the ML classifier; `processing_time_ms` in every `Ack`/`Nack` feeds the training loop.
- **`pmlmq_rpc` proto package** — kept distinct from the `pmlmq` C++ namespace to avoid symbol collisions.

---

## Repository layout

```
pmlmq/
├── proto/pmlmq.proto          # Broker service definition (package pmlmq_rpc)
├── include/
│   ├── pmlmq_service.hpp      # PMLMQService : pmlmq_rpc::Broker::Service
│   ├── proxy/proxy.hpp
│   ├── queue/
│   │   ├── message.hpp        # Message, AgingConfig, DLQEntry
│   │   ├── multi_level_queue.hpp
│   │   └── dead_letter_queue.hpp
│   ├── producer/producer.hpp  # gRPC client: connect() → send()
│   └── consumer/consumer.hpp  # gRPC client: connect() → start() / stop()
├── src/                       # C++ implementations
├── server/main.cpp            # pmlmq_server binary (SIGINT/SIGTERM graceful shutdown)
├── demo/main.cpp              # End-to-end demo (embedded broker + 2 producers + 2 consumers)
├── tests/
│   └── unit/                  # GoogleTest sources; built as pmlmq_unit_tests
│                              # (queue, DLQ, proxy — no gRPC) and
│                              # pmlmq_integration_tests (broker, client over gRPC)
├── ml_engine/                 # (Phase 2, planned — not yet present)
├── benchmarks/                # (Phase 3, planned — not yet present)
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

The build expects Protobuf and gRPC CMake CONFIG packages from a compatible
installation. For sanitizer verification, configure with
`-DPMLMQ_SANITIZER=address`, `undefined`, or `thread`.

**Targets**

| Binary | Description |
|---|---|
| `build/pmlmq_server` | Standalone broker process |
| `build/tests/pmlmq_unit_tests` | Internal tests (no gRPC required) |
| `build/tests/pmlmq_integration_tests` | Full gRPC round-trip tests |
| `build/demo/pmlmq_demo` | End-to-end demo |

---

## Running

**Standalone broker** (default `0.0.0.0:50051`, accepts an address override as `argv[1]`):

```bash
./build/pmlmq_server
# or
./build/pmlmq_server 127.0.0.1:50051
```

**End-to-end demo** (embedded broker + 2 producers + 2 consumers, demonstrates retries; the DLQ path is exercised in the integration tests):

```bash
./build/demo/pmlmq_demo
```

**Tests**

```bash
./build/tests/pmlmq_unit_tests
./build/tests/pmlmq_integration_tests
# or, when configured with CMake testing
ctest --test-dir build --output-on-failure
```

---

## Client API

```cpp
// Producer
auto producer = pmlmq::Producer::connect("127.0.0.1:50051");
std::string msg_id = producer->send(
    {0x01, 0x02},                          // payload bytes
    {{"job_type", "resize"}, {"src", "a"}} // headers (used for ML features in Phase 2)
);

// Consumer
auto consumer = pmlmq::Consumer::connect(
    "127.0.0.1:50051",
    [](const pmlmq::ReceivedMessage& msg) -> pmlmq::AckResult {
        // process msg.payload / msg.headers
        return pmlmq::AckResult::SUCCESS; // or FAILURE to trigger retry / DLQ
    }
);
consumer->start();
// ...
consumer->stop();
```

---

## Configuration (`PMLMQConfig`)

| Field | Default | Description |
|---|---|---|
| `num_levels` | `3` | Number of priority queue levels (0 = highest) |
| `aging` | `nullopt` (off) | `AgingConfig{threshold 5000 ms, interval 500 ms}` when enabled; `nullopt` = strict-priority with no aging thread (`server/main.cpp` and the demo enable aging explicitly) |
| `default_max_retries` | `3` | Nack attempts before the message moves to the DLQ |
| `default_ttl` | `0` (off) | TTL from arrival; `0` = no expiry |
| `default_priority` | `1` (medium) | Static priority for Phase 1; ML classifier overrides in Phase 2 |
| `max_pull_wait` | `5000 ms` | Server-side cap on consumer pull timeout |
| `ttl_sweep_interval` | `100 ms` | Background TTL sweep interval; `0` disables it |

Configuration is validated at broker construction. Levels and retry count must
be positive, durations must use their documented non-negative/off semantics,
and an enabled aging policy requires positive threshold and interval values.

### Current production boundary

The broker is currently an in-memory research prototype. Before deployment
beyond a trusted local network, add in-flight lease recovery, queue/DLQ/payload
limits and backpressure, TLS/authentication, idempotent mutation retries,
operator authorization, and metrics/tracing. A consumer stops its polling loop
when an Ack/Nack RPC fails so it does not report a successful local outcome for
an unconfirmed broker mutation; the message remains subject to future broker
lease recovery once that feature is implemented.

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
- [ ] Predictive routing injected into `route_message()` in `pmlmq_service.cpp`
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
