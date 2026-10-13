# Cloud runs on AWS (`v3-cloud` and `v3-brokers`, issue #46)

Pre-registered in [docs/v3-cloud-preregistration.md](../../../docs/v3-cloud-preregistration.md) (E1, E2) and
[docs/v3-brokers-preregistration.md](../../../docs/v3-brokers-preregistration.md) (E3, E4). Both were frozen in commit
`2da0e3a` after the AWS debug pass and before any registered run. [Amendment 1](../../../docs/v3-cloud-preregistration.md#amendment-1-2026-10-13-0330-utc-e1-on-its-own-host)
moved E1 to its own host during the runs; nothing else changed.

| | |
|---|---|
| Code | commit `2da0e3a` on every host (`manifest-*.txt`) |
| Pair | client `c8i.2xlarge` (8 vCPU) and broker `c8i.xlarge` (4 vCPU): E4, E3, then E2 |
| E1 host | a second `c8i.2xlarge`: E1 alone, with the broker embedded |
| All hosts | AWS us-east-1a, one cluster placement group, Ubuntu 24.04, kernel 7.0.0-1014-aws, Intel Xeon 6975P-C, Docker 29.1.3 (`host-*.txt`) |
| Image | `harbinger-bench`, built from `2da0e3a`: `sha256:8952ed0d…` on both pair hosts, `sha256:2b02939e…` on the E1 host (rebuilt there, see Amendment 1) |
| Brokers | RabbitMQ 3.12.1 (Erlang 25.3), Redis 7.0.15, rabbitmq-c 0.11.0, hiredis 1.2.0 (`*/client-versions.txt`, `*/broker-versions.txt`) |
| Times (UTC) | E4 2026-10-12 23:06–23:56, E3 23:56–03:20, E2 03:20–07:21; E1 2026-10-13 03:38–06:34 |

**CPU speed.** The kernel calibration is the same on the E1 host and the pair's client: resize about 6.6 ms per
megapixel and export about 17.55 ms per chunk in every run. That is about 20% faster than the original `v2-prodflow-1`
run and about 2× faster than the local quiet re-run, so the same workload is a lighter load here.

**Raw data.** The per-message `messages.tsv` files (465 MB with the rest of the raw tree) are not committed. Each
experiment's `SHA256SUMS` hashes every raw file. `runs/` holds every run's `run.json` (E1–E3) or result file (E4).
`report.md` and `summary.json` were written by the pre-registered analysis on the client host, and re-running it
locally gives the same reports.

## Results

| Experiment | Question | Outcome |
|---|---|---|
| **E1** `v3-cloud-e1-1` | Does `v2-prodflow-1` replicate on cloud hardware? | **gate passed**: `predictive` −30.8% mean vs FIFO |
| **E3** `v3-brokers-prodflow-1` | Does Harbinger beat RabbitMQ and Redis on the same workload? | **gate passed**: −21.7% mean vs RabbitMQ FIFO, −25.5% vs Redis FIFO |
| **E2** `v3-cloud-e2-1` | Does the gain hold over 30 minutes with the broker on its own host? | descriptive, no gate: **3 of 8 checks failed** (gain −6% to −7% mean) |
| **E4** `v3-brokers-cost-1` | What does each broker cost with empty handlers? | no pass/fail: Harbinger has the lowest throughput, and its tail rises near capacity |

Every production-flow run (E1 35, E2 8, E3 40) completed every message, with no DLQ and no submit errors.

## E1: v2 replicated on cloud hardware ([report](e1/report.md))

**Gate passed.** Against the published `v2-prodflow-1`:

| Criterion | `v2-prodflow-1` | E1 (AWS) | Result |
|---|---|---|---|
| P1 mean vs FIFO (≤ −15%) | −27.9% [−31.9, −24.7] | **−30.8%** [−35.3, −27.1] | pass |
| P2 short-class P50 vs FIFO (≤ −30%) | −88.4% [−99.0, −72.6] | **−94.8%** [−99.0, −86.5] | pass |
| G1 all-message P99 vs FIFO (≤ 1.25) | 1.04× [1.02, 1.06] | 1.03× [1.02, 1.05] | pass |
| G2 export max wait vs FIFO (≤ 2.0, no run > 60 s) | 1.02× [0.96, 1.05] | 0.99× [0.90, 1.05] | pass |
| G3 no lost work | pass | pass | pass |
| G4 throughput vs FIFO (≥ 0.98) | 1.00× | 1.00× [1.00, 1.00] | pass |
| S1 vs `static_tuned` (≤ +10%) | −10.9% | −14.4% | pass |
| S2 vs `static_misconfigured` | −33.8% | −36.8% | pass |
| S3 shadow overhead (±5%) | −0.6% | −0.1% | pass |
| S4 producer-only keys vs FIFO (≤ −5%) | −15.4% | −15.7% | pass |

Every v2 verdict holds, and P1 and P2 fall inside the v2 intervals. By the arms' medians the oracle is 32% below FIFO
and `predictive` 29%, so the predictor is close to its ceiling. Prediction lookups take 1.9 µs on average.

## E3: Harbinger against RabbitMQ and Redis ([report](e3/report.md))

**Gate passed.** Identical producers, handlers, workers, arrivals and seeds; only the broker and its policy change.

| Criterion | Point | 95% CI | Result |
|---|---|---|---|
| P1 `predictive` mean vs `rabbitmq_fifo` (≤ −15%) | −21.7% | [−26.2, −18.4] | pass |
| P2 short-class P50 vs `rabbitmq_fifo` (≤ −30%) | −89.5% | [−98.8, −75.5] | pass |
| P3 mean vs `redis_fifo` (≤ −15%) | −25.5% | [−29.5, −22.6] | pass |
| P4 short-class P50 vs `redis_fifo` (≤ −30%) | −90.7% | [−99.0, −78.0] | pass |
| G1 all-message P99 vs `rabbitmq_fifo` (≤ 1.25) | 1.10× | [1.07, 1.12] | pass |
| G2 export max wait vs `rabbitmq_priority` (≤ 1.0) | 0.20× | [0.18, 0.23] | pass |
| G3 no lost work | – | – | pass |
| G4 throughput vs `rabbitmq_fifo` (≥ 0.98) | 1.00× | [0.99, 1.00] | pass |

**Secondary:**

| Criterion | Point | Result |
|---|---|---|
| S1 Harbinger `fifo` vs `rabbitmq_fifo`, mean (±10%) | **+11.0%** [+10.4, +11.6] | **fail** |
| S2 Harbinger `fifo` vs `redis_fifo`, mean (±10%) | +5.6% [+4.9, +6.2] | pass |
| S3 `rabbitmq_predictive` vs `rabbitmq_fifo`, mean (≤ −15%) | **−57.6%** [−58.6, −56.8] | pass |
| S4 `rabbitmq_predictive` vs `rabbitmq_priority`, mean (≤ +10%) | −26.3% [−31.5, −21.1] | pass |
| S5 `predictive` vs `static_tuned`, mean (≤ +10%) | −13.2% [−17.1, −10.7] | pass |
| S6 `predictive_weighted` vs `rabbitmq_fifo`, mean (≤ −15%) | −52.4% [−53.4, −51.0] | pass |
| S7 `predictive_weighted` vs `rabbitmq_fifo`, P99 (≤ 1.25×) | 1.21× [1.20, 1.24] | pass |
| S8 `rabbitmq_priority` vs `rabbitmq_fifo`, export max wait (≤ 2×) | 5.23× [4.79, 5.55] | fail (expected: no aging) |
| G3b–G3e no lost work on RabbitMQ and Redis | – | pass |

Mean latency, median across seeds (ms): Harbinger `fifo` 7,319, `rabbitmq_fifo` 6,643, `redis_fifo` 7,008;
`predictive` 5,296, `predictive_weighted` 3,141, `rabbitmq_priority` 4,153, `rabbitmq_predictive` 2,839.

**What the results show:**
- **The gate passes, by less than expected.** The pre-registration expected −25% to −28% against both FIFO brokers;
  against RabbitMQ it is −21.7%. Part of the gap is S1: Harbinger's own FIFO arm is 11% slower than RabbitMQ's, just
  outside the ±10% equivalence bar. Harbinger's broker adds latency of its own under this load (see E4).
- **The prediction carries over to RabbitMQ, and works best there.** The broker's predictor running beside RabbitMQ
  as a sidecar (`rabbitmq_predictive`) cuts mean latency by 57.6%, far more than Harbinger's own `predictive` arm. It
  matches `predictive_weighted` (−52.4%) and keeps the export max wait at 20.9 s against RabbitMQ FIFO's 18.3 s.
- **Aging is the likely reason.** RabbitMQ priorities never age, so short jobs stay ahead through the flash sale.
  Harbinger's default 5 s aging lifts waiting long jobs back up, as in `v3-aging-1`. With weights and pausing aging
  (`predictive_weighted`), Harbinger gets most of the same gain. E3 does not isolate aging, so this is an explanation,
  not a measured cause.
- **Static priorities without aging starve the long class.** `rabbitmq_priority`'s longest export wait was 101 s (median
  across seeds), 5.2× FIFO (S8, expected). Harbinger's `predictive` bounds that wait at a fifth of it (G2).

## E2: a 30-minute run, broker on its own host ([report](e2/report.md))

**No gate (two repetitions).** Each check must hold in both repetitions (seeds 101 and 202):

| Check | Seed 101, 202 | Result |
|---|---|---|
| C1 `predictive` mean vs `fifo` (≤ −15%) | −6.1%, −7.4% | **fail** |
| C2 short-class P50 vs `fifo` (≤ −30%) | −19.7%, −31.8% | **fail** |
| C3 all-message P99 vs `fifo` (≤ +25%) | −1.9%, −2.5% | pass |
| C4 export max wait vs `fifo` (≤ 2×) | +1.2%, +0.5% | pass |
| C5 shifted-phase mean vs `fifo` (≤ −15%) | −14.7%, −23.1% | **fail** (seed 101 by 0.3 points) |
| C6 `predictive` vs `static_tuned`, mean (≤ +10%) | −2.2%, −3.1% | pass |
| C7 `shadow` vs `fifo`, mean (±5%) | −0.3%, −0.2% | pass |
| C8 throughput vs `fifo` (±2%) | −0.0%, −0.0% | pass |
| G3 no lost work, every arm | – | pass |

Mean latency by phase, median of the two repetitions (ms):

| Phase | `fifo` | `static_tuned` | `shadow` | `predictive` |
|---|---|---|---|---|
| steady (0–6 min) | 59 | 50 | 59 | 46 |
| ramp (6–9) | 471 | 284 | 466 | 215 |
| flash sale (9–15) | 67,720 | 64,867 | 67,733 | 62,897 |
| recovery (15–21) | 91,573 | 89,178 | 91,274 | 87,678 |
| shifted (21–30) | 12,593 | 10,908 | 12,367 | 10,325 |

**What the results show:**
- **The gain shrinks to a few percent when backlogs last minutes.** Outside overload, predictive routing still halves
  latency (steady −23%, ramp −54%). But the 6-minute flash sale pushes FIFO's mean latency to 68 s, and to 92 s in the
  recovery after it, and those phases dominate the overall mean. With waits that long, today's 5 s aging lifts almost
  every waiting job to the top level, so routing likely changes the order very little. The pre-registration named this risk: C1 "may therefore pass by less than in v2, or fail".
- **Everything else holds.** No guardrail moved: P99 and the export max wait match FIFO. Shadow mode costs nothing,
  and `predictive` stays ahead of the hand-tuned static map. After the mid-run shift it is 15–23% ahead of FIFO, with
  no drift alerts and the same tier agreement (0.80) as in E1.
- **This is the `v3-aging-1` result on real infrastructure.** It is the regime the opt-in weights and pausing aging
  were built for. E2 did not include `predictive_weighted`; E3 shows its effect on the 5-minute workload.

## E4: broker cost ([report](e4/report.md))

4 producers and 4 consumers with empty handlers, 64-byte payloads, 30 s runs (5 s warm-up), 5 repetitions. No
pass/fail.

| | Harbinger | RabbitMQ | Redis |
|---|---|---|---|
| Peak delivered/s (median) | 5,293 | 7,371 | 21,182 |
| 1,000 msg/s: kept up, P50 / P99 | 5/5, 0.24 / 0.62 ms | 5/5, 0.15 / 0.52 ms | 5/5, 0.16 / 0.54 ms |
| 5,000 msg/s: kept up, P50 / P99 | 5/5, 0.30 / 10.6 ms | 5/5, 0.12 / 0.31 ms | 5/5, 0.10 / 0.31 ms |
| 20,000 msg/s: kept up, P50 / P99 | 0/5, 30.8 / 40.6 **s** | 5/5, 0.23 / 11.4 ms | 3/5, 0.23 / 76.7 ms |

**What the results show:**
- **Harbinger is the slowest broker,** as the pre-registration expected: about 5,300 deliveries per second, 72% of
  RabbitMQ (prefetch 1) and a quarter of Redis.
- **At 5,000 msg/s Harbinger is at its limit.** All five runs count as kept up (rate and send lateness met). But two
  of them built a queue: P50 91 ms and 462 ms, P99 446 ms and 1,227 ms. The other three stayed at 0.30 ms P50 and
  3.7–10.6 ms P99. The pre-registration expected latency "within the same sub-millisecond range" at 5,000 msg/s; that
  holds only at 1,000 msg/s.
- **RabbitMQ's "lost" messages at peak rate were not delivered by the end of the drain.** At full speed RabbitMQ
  accepted about 56,000 publishes per second but delivered about 7,400 per second, and 8–14% of messages
  (147,505–244,907 per run) were still undelivered when the 60 s drain ended. Publishing had no errors. Without
  publisher confirms, the benchmark cannot tell whether those messages were still queued. The peak-rate table counts
  them as lost because they were not delivered. All fixed-rate RabbitMQ runs lost nothing.
- **Within the production flow these costs are small** next to handlers of milliseconds to seconds at about 116
  messages per second. They do show up as E3's S1 gap.

## Limits

- One synthetic workload (production flow), one cloud provider and region, two instance types.
- E2 has two repetitions, so its checks are per repetition, without intervals.
- One placement group: a low-latency network, not a WAN.
- E1 ran on a different host of the same type, with an image rebuilt from the same commit (Amendment 1). Its CPU
  calibration matches the pair's client.
- RabbitMQ runs with prefetch 1, which limits its throughput (E4) and is how priorities reorder at all (E3).
- Brokers are non-persistent; durable modes, Kafka and managed queues are out of scope.
