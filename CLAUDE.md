# PMLMQ — Predictive Multi-Level Message Queue

## Project Overview

PMLMQ is a novel distributed messaging system that combines ML-based processing time prediction with Multi-Level Feedback Queue (MLFQ) principles to optimize message routing and reduce latency. The core innovation: predict message processing times at ingress and route to appropriate priority queues, rather than relying on FIFO or static priority schemes.

**Research hypothesis:** Predictive classification at message arrival can measurably improve P50/P95/P99 latency over baseline algorithms (FIFO, static priority, round-robin).

---

## Architecture

Six core components:

1. **Proxy Layer** — Ingress point; invokes the ML classifier and routes messages to the appropriate queue tier
2. **ML Classifier** — Python-based online learning engine that predicts message processing time category (short / medium / long) from message content/metadata at arrival time; continuously updated as new processing time observations arrive
3. **Multi-Level Queue System** — Priority-based queue hierarchy; higher-priority queues for predicted short tasks
4. **Consumer Groups** — Competitive consumption model; message deleted after acknowledgment (queuing paradigm, not streaming)
5. **Metrics & Feedback System** — Tracks actual processing times per message; feeds back into classifier training
6. **Discrete Event Simulator** — Benchmarking harness for controlled workload replay and comparison

---

## Tech Stack

- **Language:** C++20 (core infrastructure), Python (ML engine)
- **ML Engine:** Python service with online/incremental learning; classifier is continuously retrained as actual processing times are reported back from consumers. Candidate algorithms: `river` (online ML library), or scikit-learn with incremental estimators (`SGDClassifier`, `PassiveAggressiveClassifier`)
- **C++/Python Bridge:** gRPC or a lightweight IPC mechanism between the C++ Proxy Layer and the Python classifier service
- **Style:** Zero-cost abstractions, template-based design, lock-free data structures where possible, header-only libraries preferred
- **Metrics:** Prometheus / StatsD integration
- **Tracing:** Jaeger / Zipkin for distributed tracing
- **Benchmarking frameworks:** OpenMessaging Benchmark; Yahoo Streaming Benchmark (reference)

---

## Development Phases

### Phase 1 — Core Queuing
- [ ] Basic message ingestion and acknowledgment
- [ ] Competitive consumption (single consumer processes each message)
- [ ] Dead letter queue (DLQ) for failed/expired messages
- [ ] Static multi-level priority queue (baseline)

### Phase 2 — ML Integration
- [ ] Feedback mechanism: consumers report actual processing times to metrics collector
- [ ] Feature extraction from message headers/payload metadata
- [ ] Python ML service with online learning (continuously updates classifier on each new observation)
- [ ] IPC/gRPC bridge between C++ Proxy Layer and Python classifier service
- [ ] Predictive routing integrated into Proxy Layer
- [ ] Model drift monitoring and accuracy tracking over time

### Phase 3 — Benchmarking & Validation
- [ ] Synthetic workload generators (uniform, bimodal, heavy-tailed distributions)
- [ ] Real-world dataset replay: Alibaba Microservices Trace (2021/2022), Azure Functions Trace
- [ ] Comparison against FIFO, static priority, round-robin baselines
- [ ] Comparison against industry systems: Kafka, RabbitMQ, Pulsar

### Phase 4 — Enhancements (Optional)
- [ ] Streaming mode (persistent events, multi-consumer replay)
- [ ] Multi-tenancy
- [ ] Dynamic classifier retraining online

---

## Key Metrics

| Metric | Description |
|--------|-------------|
| P50 / P95 / P99 latency | End-to-end message processing latency percentiles |
| Throughput (msg/s) | Sustained message processing rate |
| Prediction accuracy | Classifier accuracy across processing time buckets |
| Queue starvation rate | Rate at which lower-priority queues are starved |

---

## Baseline Comparisons

| System | Paradigm | Notes |
|--------|----------|-------|
| Kafka | Streaming (distributed commit log) | Pull-based, partitioned, persistent; not a task queue |
| RabbitMQ | Queuing | Push-based, sophisticated routing via exchanges |
| Pulsar | Hybrid streaming/queuing | Multi-tenancy, tiered storage |
| FIFO Queue | Baseline algorithm | No priority |
| Static Priority Queue | Baseline algorithm | Fixed priority, no prediction |
| Round-Robin | Baseline algorithm | No priority |

---

## Novelty Statement

Existing ML applications in message queuing focus on: CPU node selection, network congestion control, and broker parameter optimization. PMLMQ applies ML at the **message level** to predict per-message processing time and route accordingly — combining MLFQ principles (from OS scheduling, 1960s) with predictive classification in a distributed messaging context. This combination has not been addressed in prior work.

---

## Coding Conventions

- C++20 features encouraged: concepts, ranges, coroutines where appropriate
- Prefer `std::atomic` and lock-free structures over mutexes for hot paths
- All public APIs documented with Doxygen-style comments
- Unit tests alongside each component; integration tests in `/tests/integration`
- Benchmarks in `/benchmarks`; must be reproducible with a single script

---

## Repository Structure (Target)

```
pmlmq/
├── include/          # Public headers (C++)
│   ├── proxy/
│   ├── queue/
│   ├── consumer/
│   └── metrics/
├── src/              # C++ implementation
├── ml_engine/        # Python ML service
│   ├── classifier/   # Online learning models
│   ├── features/     # Feature extraction logic
│   ├── feedback/     # Ingests processing time observations
│   └── server.py     # gRPC / IPC server entry point
├── tests/
│   ├── unit/
│   └── integration/
├── benchmarks/       # Benchmark harness + workload generators
├── docs/             # Design docs, research notes
├── scripts/          # Build, run, and evaluation scripts
└── CLAUDE.md
```

---

## Running Benchmarks

*(To be filled in as benchmarking infrastructure is built)*

```bash
# Placeholder
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/benchmarks/run_all --workload bimodal --duration 60s
```