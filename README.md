<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/brand/harbinger-lockup-dark.svg">
    <img alt="Harbinger" src="assets/brand/harbinger-lockup.svg" height="64">
  </picture>
</p>

# Harbinger — Predictive Multi-Level Message Queue

*Harbinger (formerly PMLMQ) — the queue that knows what's coming: it learns each kind of message's cost, then prioritizes.*

A research messaging system that learns **per-key processing-time statistics online** and uses them for **multi-level priority scheduling**, so operators do not have to classify heavy and light endpoints by hand or benchmark them in advance.

**Research hypothesis (revised):** Learning each key's duration distribution from observed handler times and routing short-expected work first reduces median and mean latency and head-of-line blocking relative to FIFO, and matches a well-configured static priority map without hand configuration. It is expected to stay robust where a static map is wrong or stale. Tail (P99) latency is *not* expected to improve when long jobs are common; see [Design direction](#design-direction).

---

## Status

**Phase 1 complete; Phase 2 implemented and evaluated.** The core gRPC broker includes multi-level priority queues, producer/consumer clients, DLQ, aging, retries, and in-flight management. Phase 2 adds an in-process [per-key duration predictor](docs/duration-predictor.md) wired into `route_message()` with shadow and opt-in predictive modes, learning from Ack settlement, snapshots, drift counters and a [standalone config file](docs/configuration.md), plus [versioned ingress features](ml_engine/README.md) and [bounded persistent feedback](docs/feedback.md). **Default routing stays static.**

[Phase 2 results](docs/phase2-report.md), against criteria [frozen before any run](docs/v2-preregistration.md):

- **Production-flow benchmark (gate passed).** On a realistic e-commerce job queue (real CPU work, flash-sale overload, mid-run cost shift; 5 seeds × 7 arms), predictive routing cut mean latency 28% and short-job median latency 88% versus FIFO, beat a hand-tuned static map by 11% and a misconfigured one by 34%, with all-message P99 +4%, no lost work and no throughput change. Without job labels (producer-only keys) it still cut mean latency 15%.
- **Azure Functions trace simulation (gate not passed).** With the broker's 5 s aging and hours-long backlogs from the trace's bursts, every policy, including the oracle, was within 1–3% of FIFO. Exploratory runs without aging separate the policies.
- **Aging under sustained overload (#40, opt-in).** Worker-time level weights with pausing aging keep priority meaningful in long backlogs. On the Azure trace they cut mean latency 14.6–27.3% versus FIFO, passing the frozen gate at load 0.8 but not at 0.5 or 0.95. In the production flow they cut mean latency a further 42% versus today's predictive routing, at the cost of all-message P99 +12% (just over its 10% guardrail). They stay off by default ([results](benchmarks/results/v3-aging/README.md)).
- **Real data:** on the same trace a per-key median predicts held-out durations with a median error factor of 1.3, versus 29 for a single global median.

The earlier offline comparison (issue #5) found no qualifying model under its frozen v1 gates; v2 replaced those budgets, as documented in the pre-registration.

---

## Design direction

Evidence so far, and the resulting Phase 2 plan:

- **Synthetic evidence only.** The workloads use one 3-value categorical header and a fixed 16-byte payload, so a per-key average is close to the best possible predictor there. These results do not show whether richer features help on real traffic.
- **Shortest-job-first trade-off.** With a non-preemptive scheduler, prioritizing short jobs lowers median latency (about 40–60% in the bimodal cells) and leaves P95 flat or makes P99 worse when long jobs are a large fraction of messages. Reordering cannot reduce a long job's own service time. Primary metrics are therefore per-class percentiles, slowdown, mean latency, and long-job starvation, with all-message P99 as a guardrail.
- **Predictor: per-key duration statistics, in C++.** Each message has a *key*: the broker-assigned producer id, optionally joined to a producer-supplied `job_type` header (there is no queue/topic name; the broker is one logical queue). The broker keeps a decaying duration histogram per key plus a global histogram, estimates a message's duration from its key at ingress inside `route_message()`, and maps it to a tier using boundaries derived from global quantiles. Cold or high-variance keys use the middle tier; aging is unchanged. Only successful, non-replayed handler durations are learned. Per-key state is bounded in memory.
- **Python is the offline evaluation harness**, not a serving component. River models remain optional offline challengers, and richer features are added only if real or numeric-feature workloads show the per-key model losing.
- **Not yet shown.** No predictive scheduling benefit, real-workload generalization, or activation is claimed. The v1 budgets cannot be met even by an oracle-like static map, so Phase 2 comparisons move to a pre-registered v2 matrix.

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
- **Phase 2 hook** — `route_message()` in `harbinger_service.cpp` is the single injection point for the planned per-key duration predictor; opt-in feedback persists accepted `processing_time_ms` observations and actual outcomes without changing delivery semantics. Offline learners consume validated temporal exports; the C++ [per-key predictor](docs/duration-predictor.md) exists but is not connected to the broker yet.
- **`harbinger_rpc` proto package** — kept distinct from the `harbinger` C++ namespace to avoid symbol collisions.

---

## Documentation

- [Documentation index](docs/README.md) — choose a guide by role.
- [Producer guide](docs/producers.md) — connect, send messages, set TTL, and understand Submit outcomes.
- [Consumer guide](docs/consumers.md) — handlers, delivery leases, acknowledgements, retries, and shutdown.
- [Broker and queue internals](docs/internals.md) — state transitions, priority ordering, aging, expiry, and recovery.
- [Ingress features and encoding](ml_engine/README.md) — implemented opt-in C++ capture and matching Python representation.
- [Persistent feedback](docs/feedback.md) — embedded static-mode collection, JSONL storage, retention, loss counters, and durability/shutdown limits.
- [Synthetic baselines and feedback export](benchmarks/README.md) — seeded open-loop gRPC replay, outcome/fairness accounting, paired uncertainty, and frozen experiment budgets.
- [Online predictors](docs/online-predictor.md) — cited literature, eight candidates, delayed validation, readiness/fallback, and the frozen v1 selection budgets (result: no qualifier).
- [ML contract](docs/ml-contract.md), [architecture decision](docs/adr/0001-phase2-ml-contract.md), and [validation plan](docs/phase2-validation.md) — Phase 2 boundaries; inference transport and activation remain planned.

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
├── ml_engine/                 # Features, feedback validation, offline evaluation harness
├── benchmarks/                # Opt-in cleanup + synthetic scheduler baselines and dataset export
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
| `aging` | `nullopt` (off) | `AgingConfig{threshold 5000 ms, interval 500 ms}` when enabled; `nullopt` = strict-priority with no aging thread (`server/main.cpp` and the demo enable aging explicitly). `pause_when_behind` (unset = on with `level_weights`) pauses promotion into a level that is behind ([details](docs/configuration.md#level-weights-and-pausing-aging-issue-40)) |
| `level_weights` | `nullopt` (strict priority) | Opt-in worker-time share per level, e.g. `{8, 3, 1}`; one weight in [1, 1000] per level. Not combinable with the round-robin benchmark policy |
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
| `ingress_features` | `nullopt` | Opt-in bounded static ingress snapshots; programmatic embedded configuration |

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

### Phase 2 — ML integration (implemented and evaluated)

The [Phase 2 tracker](https://github.com/divyanshusingh2903/predictive-multi-level-message-queue/issues/11) sequences implementation from the [ML contract](docs/ml-contract.md). Prediction begins in shadow mode; explicit predictive routing requires the [synthetic validation gate](docs/phase2-validation.md).

- [x] Versioned, bounded ingress features and immutable routing context with C++/Python fixtures
- [x] In-process C++ per-key duration predictor (decaying histograms, adaptive tier boundaries, bounded key state)
- [x] Shadow and predictive routing injected into `route_message()` in `harbinger_service.cpp`
- [x] `processing_time_ms` feedback from Ack (and censoring from lease expiry) updates the per-key statistics
- [x] Predictor snapshots (atomic, versioned, rollback copy), drift counters and a stats line
- [x] Standalone `--config` file, `--routing-mode` and `--stats-interval-ms` ([configuration](docs/configuration.md))
- [x] Seeded synthetic workloads, real-broker FIFO/static-priority/round-robin baselines, temporal feedback export, and frozen numerical budgets
- [x] Offline delayed online-predictor comparison, readiness/fallback checks, and versioned model-selection evidence (v1 result: no qualifier)
- [x] Pre-registered v2 matrix ([v2 pre-registration](docs/v2-preregistration.md)): production-flow benchmark with oracle, well-configured/misconfigured/stale static priority, per-class and slowdown metrics; Azure Functions trace simulation
- [x] Per-key predictability on a real trace (Azure Functions 2021)
- [x] Feature signal on self-generated data (`feature-signal-v1`) and opt-in log2 size-binned keys with cold-bin parent fallback ([size bins](docs/duration-predictor.md#size-bins-issue-35)); under load they did not pass their pre-registered gate ([v3-sizebins-1](benchmarks/results/v3-sizebins/README.md))
- [x] v2 activation gate: passed on the production-flow benchmark; not passed on the Azure trace simulation ([Phase 2 report](docs/phase2-report.md))

### Phase 3 — Benchmarking & validation
- [ ] Expand synthetic workload coverage beyond the Phase 2 activation gate
- [ ] Real-world data: public traces (e.g. Azure Functions invocation trace, BurstGPT) and self-generated cloud runs with real handlers
- [ ] Extend FIFO/static-priority/round-robin comparisons to real trace workloads
- [ ] Comparison against Kafka, RabbitMQ, Pulsar

### Phase 4 — Enhancements (optional)
- [ ] Streaming mode (persistent events, multi-consumer replay)
- [ ] Multi-tenancy
- [ ] Dynamic online classifier retraining

---

## Key metrics

| Metric | Description |
|---|---|
| Latency percentiles | P50/P95/P99 end-to-end latency overall and per job class, plus mean latency and slowdown (latency / service time) |
| Throughput (msg/s) | Sustained message processing rate |
| Prediction quality | Duration MAE/RMSE, bucket accuracy, severe underestimates, and coverage |
| Queue starvation rate | Rate at which lower-priority queues are starved |

---

## Tech stack

- **Core:** C++20 — concepts, `std::atomic`, lock-free where possible, header-only libraries preferred
- **IPC:** gRPC + Protocol Buffers
- **Predictor:** C++ per-key duration statistics inside the broker (planned)
- **Offline evaluation:** Python — delayed-feedback replay and metrics; pinned River 0.22.0 candidates and dependency-free baselines (implemented)
- **Tests:** GoogleTest (fetched via CMake `FetchContent`)
- **Metrics (planned):** Prometheus / StatsD
- **Tracing (planned):** Jaeger / Zipkin
