# Production queues: pre-registration (issue #37, `v3-brokers`)

**Frozen 2026-10-12, together with [`v3-cloud`](v3-cloud-preregistration.md), after the AWS debug pass and before any
registered run.**

| | |
|---|---|
| Configurations | [`v3-brokers-prodflow.json`](../benchmarks/configs/v3-brokers-prodflow.json) (E3), [`v3-brokers-cost.json`](../benchmarks/configs/v3-brokers-cost.json) (E4) |
| Analysis | [`prodflow_report.py`](../benchmarks/prodflow_report.py) (E3) and [`brokerbench_report.py`](../benchmarks/brokerbench_report.py) (E4) |
| Plan | #46 |

Changing anything below after results exist requires a new version, and those results stay published.

## Questions

Two questions, kept apart as #37 requires.

1. **Scheduling outcome (E3).** On the production-flow workload, with identical producers, handlers, worker count,
   arrival schedule and seeds, does Harbinger's predictive routing beat the work queues people run today, RabbitMQ
   and Redis? Only the broker and its policy change. Does the scheduling idea also work on RabbitMQ when a predictor
   sits beside it?
2. **Broker cost (E4).** With empty handlers, how much throughput and latency does each broker itself cost? RabbitMQ
   and Redis are mature and heavily optimised and are expected to beat Harbinger here. This is reported whichever way
   it goes.

## Environment

- **AWS us-east-1**, one availability zone, one cluster placement group, Ubuntu 24.04.
- **Broker host:** `c8i.xlarge` (4 vCPU). **Client host:** `c8i.2xlarge` (8 vCPU), running producers, workers and all
  latency timestamps.
- **Image:** both hosts run the `harbinger-bench` image built from the freeze commit (`benchmarks/docker/Dockerfile`),
  with host networking. The image gives RabbitMQ 3.12.1 (Erlang 25.3), Redis 7.0.15, rabbitmq-c 0.11.0 and hiredis
  1.2.0, Ubuntu 24.04's own packages. Each run records the package versions.
- **Fresh broker per run:** a new container for every arm, seed and rate.
- **Non-persistent on every broker.** Harbinger is in-memory; RabbitMQ uses transient messages on a non-durable
  classic queue with no publisher confirms; Redis has no RDB or AOF.

## E3: scheduling outcome (`v3-brokers-prodflow-1`)

**Workload:** `v2-prodflow-1` unchanged. 300 s of arrivals, 4 workers, rate scale 1.8, 180 s drain, seeds 11, 22, 33,
44 and 55. Arm order is rotated per seed.

| Arm | Broker | Policy |
|---|---|---|
| `fifo` | Harbinger | one level |
| `static_tuned` | Harbinger | the well-configured static map |
| **`predictive`** | Harbinger | per-key predictor, strict priority, today's aging (**primary**) |
| `predictive_weighted` | Harbinger | predictor + weights `[8, 3, 1]` + pausing aging (opt-in, #40) |
| `rabbitmq_fifo` | RabbitMQ | classic queue, manual ack, prefetch 1 |
| `rabbitmq_priority` | RabbitMQ | `x-max-priority=3`, the same static map as `static_tuned`, no aging |
| `rabbitmq_predictive` | RabbitMQ | priorities from a sidecar running the broker's own predictor (below) |
| `redis_fifo` | Redis | reliable list queue (`LPUSH`, `BLMOVE` to a per-worker list, `LREM`) |

**How the arms run:**
- **Harbinger arms** run the broker as `harbinger_prodflow --role broker` on the broker host and the clients with
  `--role client`. `benchmarks/merge_split_run.py` joins the two halves.
- **RabbitMQ and Redis arms** connect the same client binary to the broker host's server.

**The sidecar** in `rabbitmq_predictive`:
- Runs the broker's `PerKeyPredictor`, with its default settings and key rule (service + job type), in the client
  process.
- Predicts at publish and learns only from successful attempts, in whole milliseconds, as the broker does.
- RabbitMQ has no lease, so nothing is censored.

**Known differences**, kept because they are how each broker works:
- **Retries.** A failed attempt (about 2% of webhooks fail once) goes back near the head of RabbitMQ's queue, to the
  tail of Redis's, and to its original priority on Harbinger.
- **Prefetch.** RabbitMQ prefetch 1 means priorities reorder only undelivered messages.

**Gate (primary arm `predictive`; paired by seed, bootstrap 95% intervals, 10,000 resamples, seed 2026):**

| Criterion | Pass if |
|---|---|
| P1 mean latency vs `rabbitmq_fifo` | point ≤ −15% and interval below 0 |
| P2 short-class P50 vs `rabbitmq_fifo` | point ≤ −30% and interval below 0 |
| P3 mean latency vs `redis_fifo` | point ≤ −15% and interval below 0 |
| P4 short-class P50 vs `redis_fifo` | point ≤ −30% and interval below 0 |
| G1 all-message P99 ratio vs `rabbitmq_fifo` | interval upper bound ≤ 1.25 |
| G2 export (long class) max wait ratio vs `rabbitmq_priority` | interval upper bound ≤ 1.0 |
| G3 no lost work | every message completed, 0 DLQ, 0 submit errors |
| G4 throughput ratio vs `rabbitmq_fifo` | interval lower bound ≥ 0.98 |

- **P1–P4 and G1** take v2's bars, with RabbitMQ and Redis FIFO in place of Harbinger FIFO.
- **G2** asks whether Harbinger's aging bounds long-job waits at least as well as a static RabbitMQ priority queue
  without aging. An operator could configure that queue today.

**Secondary (reported, not gated):**

| Criterion | Comparison | Pass if |
|---|---|---|
| S1 | Harbinger `fifo` vs `rabbitmq_fifo`, mean | within ±10% (FIFO brokers equivalent) |
| S2 | Harbinger `fifo` vs `redis_fifo`, mean | within ±10% |
| S3 | `rabbitmq_predictive` vs `rabbitmq_fifo`, mean | ≤ −15% (the idea transfers to RabbitMQ) |
| S4 | `rabbitmq_predictive` vs `rabbitmq_priority`, mean | ≤ +10% (learned vs hand-tuned, on RabbitMQ) |
| S5 | `predictive` vs `static_tuned`, mean | ≤ +10% (v2 replication on cloud hardware) |
| S6 | `predictive_weighted` vs `rabbitmq_fifo`, mean | ≤ −15% |
| S7 | `predictive_weighted` vs `rabbitmq_fifo`, P99 | ≤ 1.25× |
| S8 | `rabbitmq_priority` vs `rabbitmq_fifo`, export max wait | ≤ 2× (expected to fail: no aging) |
| G3b–G3e | no lost work on each RabbitMQ and Redis arm | as G3 |

## E4: broker cost (`v3-brokers-cost-1`)

`harbinger_brokerbench`, run by `benchmarks/run_brokerbench.sh` with `BROKER_SSH`/`BROKER_HOST` pointing at the broker
host:
- **Brokers:** `harbinger_server` (production server, default settings), RabbitMQ and Redis.
- **Clients:** 4 producers and 4 consumers with empty handlers, 64-byte payloads.
- **Runs:** 30 s per run with the first 5 s excluded, 60 s drain, 5 repetitions, broker order rotated per repetition.

**Rates:**
- **0 (as fast as possible):** the delivery rate is the peak sustained throughput.
- **1,000, 5,000 and 20,000 msg/s:** an open-loop schedule; latency is publish start to delivery start, P50 to max.

**Kept up.** A run keeps up at a fixed rate when it lost nothing, delivered at least 99% of the offered rate, and its
producers' send lateness stayed at or under 10 ms at P99.

**No pass/fail (#37).** Reported: medians across repetitions, with ranges and the kept-up count.

## What was seen before freezing

All local, on one 16-core laptop, and none of it registered:

- **Production flow, 30-second smoke runs, seed 11** (not a registered seed), every new arm and Harbinger `fifo`:
  - The three FIFO brokers agreed on mean latency within 4%.
  - `rabbitmq_priority`'s longest export wait was 14 s.
  - `rabbitmq_predictive` halved mean latency against `rabbitmq_fifo`.
- **Microbenchmark, 6–8-second smoke matrices:**
  - Harbinger kept up to 5,000 msg/s with sub-millisecond P99, but delivered about 5,200 msg/s at most with 4
    consumers.
  - RabbitMQ delivered about 17,000/s at most, and Redis about 22,000/s.
  - At 20,000 msg/s RabbitMQ and Redis kept up; Harbinger did not.
- **AWS debug pass** (2026-10-12, 22:25–23:00 UTC; the registered hosts; seed 7, debug durations; commit
  `f42f21d`):
  - **Hosts and image:** client `c8i.2xlarge`, broker `c8i.xlarge`, both Xeon 6975P-C, kernel 7.0.0-1014-aws, us-east-1a,
    one placement group. Identical image `sha256:65821c67…` and package versions on both.
  - **Completeness:** all 12 production-flow runs (E1 2, E2 2, E3 8) completed every message, with no worker errors
    and every Harbinger run merged. All 12 E4 runs lost nothing.
  - **CPU speed:** the client's kernel calibration (resize 6.6 ms/megapixel, export 17.5 ms/chunk) is about 2× faster
    than the local quiet re-run and about 20% faster than the original v2 run, so the same workload is a lighter load
    here.
  - **Seen while watching the debug log:** E4 RabbitMQ lines at 5,000 and 20,000 msg/s (P50 0.1–0.2 ms, P99 0.3–0.8 ms,
    all delivered). No E3 latency was examined.

## Expectations, stated before running

- **E3 gate.** `predictive` should beat both FIFO brokers about as much as it beat Harbinger FIFO in `v2-prodflow-1`:
  mean about −25 to −28%, short-class P50 about −80% or better. FIFO broker overhead is small next to the handlers'
  milliseconds (S1, S2).
- **G2.** RabbitMQ's static priorities have no aging, so exports can wait through the flash sale. G2 should pass and
  S8 fail.
- **S3 and S4.** The sidecar should carry most of the gain to RabbitMQ, and match the hand-tuned map within 10%.
- **S6 and S7.** `predictive_weighted` should cut mean latency the most, as in `v3-aging-prodflow-1`, where P99 rose
  12% against `predictive`.
- **E4.** Harbinger has lower peak throughput than both, and does not keep up at 20,000 msg/s. Its latency at 1,000
  and 5,000 msg/s is within the same sub-millisecond range.

A failed gate will be published as such.

## Limits

- One synthetic scenario and one cloud environment.
- Default broker settings, except those listed.
- RabbitMQ prefetch 1 also limits its throughput, and is noted wherever E4 is cited.
- Harbinger's E3 broker is the benchmark build: the same broker code with observation hooks. E4 uses the production
  `harbinger_server`.
- Kafka is out of scope (a log, with no per-message priority), as are managed cloud queues and durable modes.

## Decisions settled before freezing (2026-10-12)

1. **`predictive_weighted`** stays in E3 as a secondary arm.
2. **G2** requires Harbinger's export max wait to be at most `rabbitmq_priority`'s (upper bound ≤ 1.0).
3. **E4** uses 4 consumers only; there is no consumer sweep.
4. **20,000 msg/s** stays in E4, to show each broker's ceiling.
5. **E3 on two hosts** runs with `benchmarks/run_prodflow_split.sh`. It was smoke-tested on 2026-10-12:
   - all 8 arms, seed 7 (not a registered seed), 15 s of arrivals, every message completed, with the Harbinger halves
     merged with their broker's counters;
   - its SSH path, through a stand-in that runs the commands locally.
   The smoke latencies were not used for anything above.
