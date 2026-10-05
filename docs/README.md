# Harbinger documentation

The operational guides below describe the core broker and implemented Phase 2 feature capture, persistent feedback, synthetic baselines and offline predictor comparison. Live inference remains planned. Start with the [project README](../README.md) for build and run instructions.

| Guide | Read this for |
|---|---|
| [Producers](producers.md) | Registering, sending bytes and headers, per-message TTL, and Submit failure semantics |
| [Consumers](consumers.md) | Handler callbacks, polling, Ack/Nack, delivery leases, retries, and shutdown |
| [Broker and queue internals](internals.md) | The full message state machine, scheduling, aging, TTL, leases, DLQ, and concurrency |
| [Ingress features and encoding](../ml_engine/README.md) | Implemented programmatic feature capture, schema validation, and Python sparse encoding |
| [Persistent feedback](feedback.md) | Embedded configuration, event labels, bounded JSONL, retention/recovery, and durability limits |

## Phase 2 design and validation

| Document | Read this for |
|---|---|
| [ML architecture decision](adr/0001-phase2-ml-contract.md) | Routing modes, Python service boundary, alternatives, and preserved invariants |
| [ML contract](ml-contract.md) | Versioned features, prediction/feedback records, proposed limits, and persistence semantics |
| [Online predictors](online-predictor.md) | Literature, incremental regressors, delayed evaluation, readiness/fallback and selection evidence |
| [Validation plan](phase2-validation.md) | Baselines, workloads, metrics, proposed gates, and activation/rollback criteria |

These documents implement the planning deliverables for [issue #1](https://github.com/divyanshusingh2903/predictive-multi-level-message-queue/issues/1); implementation work is tracked under [Phase 2 #11](https://github.com/divyanshusingh2903/predictive-multi-level-message-queue/issues/11).

The wire contract is [proto/harbinger.proto](../proto/harbinger.proto); the public C++ interfaces are under [`include/`](../include/). Configuration defaults and development invariants are also recorded in [AGENTS.md](../AGENTS.md).

Harbinger currently distributes work among **competing consumers of one logical queue**. It does not implement RabbitMQ-style exchanges, named subscriptions, or fan-out. [Synthetic scheduler baselines](../benchmarks/README.md) and [offline online-model comparisons](online-predictor.md) are implemented; live inference remains planned. Delivery state lives in memory and is lost on broker restart; optional feedback files survive independently.
