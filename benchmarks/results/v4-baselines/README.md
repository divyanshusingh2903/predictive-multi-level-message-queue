# Baselines, predictor ablation and aging isolation (`v4-baselines-1`)

Pre-registered in [docs/v4-baselines-preregistration.md](../../../docs/v4-baselines-preregistration.md) and frozen in commit
`13c05c2`, before any registered run. The run used that commit, the `harbinger-bench` image built from it, one `c8i.2xlarge`
alone in us-east-1d with the broker embedded over loopback (`manifest.txt`, `host-client.txt`), 2026-10-15 08:00 to 17:14 UTC.
11 arms on 10 seeds (11 to 110), 110 runs of about 35,000 messages. Every run completed every message, with no dead letter, no
submit error and no lost lease. A debug pass on seed 7 (11 arms, 60 s each, commit `97a10b0`) preceded the freeze and was not
analysed. The raw `messages.tsv` and `run.json` files (hashed in `SHA256SUMS`, 122 MB packed) are not committed; `runs/` holds every
`run.json`. `report.md` and `summary.json` are the pre-registered analysis, unchanged.

**Conclusion.** (1) Predictive routing reproduces its production-flow gain on a fresh set of ten seeds (−32.1% mean against FIFO,
Q1). (2) A queue per job type with equal worker-time shares and **no prediction** beats it on mean latency by a wide margin (B1).
(3) **Aging explains the sidecar gap**: with aging off the predictor's mean latency falls 37.4% further, to −57.6% against FIFO, the
size of the RabbitMQ sidecar's gain in E3 (C1, C2). (4) None of the four predictor components shows a benefit on this workload.

## Part A: baselines (change of the first arm against the second; mean latency, 10 seeds)

| ID | Comparison | Change | 95% CI | Reading |
|---|---|---|---|---|
| B1 | `predictive` vs `per_type_fair` | **+74.1%** | [+64.5%, +83.9%] | predictive worse |
| B2 | `predictive` vs `static_tuned` | −15.5% | [−17.8%, −13.3%] | predictive better |
| B3 | `predictive_mean` vs `predictive` | +3.6% | [+1.4%, +6.1%] | variant worse |
| B4 | `predictive_p75` vs `predictive` | +3.8% | [+1.6%, +6.3%] | variant worse |
| Q1 | `predictive` vs `fifo` | −32.1% | [−34.4%, −29.6%] | pass (E1 −30.8%, E5 −30.7%, E6 −30.6%) |

Medians across seeds:

| Arm | Mean ms | Short-class P50 ms | All-message P99 s | Export (long) max wait s |
|---|---|---|---|---|
| `fifo` | 6,928 | 4,965 | 21.1 | 20.8 |
| `predictive` | 4,780 | 39 | 21.8 | 21.7 |
| `static_tuned` | 5,608 | 450 | 21.5 | 26.1 |
| `per_type_fair` | **2,726** | 16 | **32.0** | 2.9 |
| `predictive_noaging` | **2,925** | 15 | 24.0 | 23.9 |

- **`per_type_fair` wins the mean, the median and most classes, and loses the all-message P99** (+52% against predictive,
  [+45.9%, +58.1%]) because its slowest classes wait longer (`deliver_webhook` P99 34.6 s against 22.1 s). It needs an operator to
  create one queue per job type and, as built, one level per type, which does not scale to hundreds of keys (Azure and Huawei have
  about 430). The pre-registration said what this means: the prediction's advantage here is that it needs no per-type queues, not
  that it schedules better. This is one workload with nine near-deterministic job types, the case where per-type queues fit best.
- The mean and p75 summaries are each about 3.7% worse than the median the broker uses. Their short-class P50 is higher (+36.5% and
  +30.6%) with intervals that include 0.

## Part B: predictor ablation (removing one component; change against `predictive`)

| Component | Overall mean | Shifted phase | Reading |
|---|---|---|---|
| decay and idle forgetting (`ablate_decay`) | +0.0% [−2.1, +2.3] not supported here | +1.0% [−3.6, +6.1] inconclusive | inconclusive |
| 99th-percentile spread check (`ablate_spread`) | +0.1% [−0.4, +0.6] not supported here | +1.0% [−1.5, +3.9] inconclusive | inconclusive |
| staleness rule (`ablate_stale`) | +0.4% [−0.2, +1.1] not supported here | +2.1% [−0.8, +6.1] inconclusive | inconclusive |
| minimum 20 observations (`ablate_nmin`) | +0.2% [−1.1, +2.0] not supported here | +1.7% [−0.7, +4.1] inconclusive | inconclusive |

No component is shown to help or to hurt overall (every interval lies within ±3%). The shifted-phase intervals are too wide to
decide, which the pre-registered rule reads as inconclusive, not as a null. The paper says that the justification for these
components rests on the traces and on reasoning, not on this workload, whose costs are stable and whose keys stay active. The
censored-share check is not ablated (this workload has no failures).

## Part C: aging isolation

| ID | Comparison | Change | 95% CI | Reading |
|---|---|---|---|---|
| **C1** | `predictive_noaging` vs `predictive`, mean | **−37.4%** | [−39.8%, −35.1%] | **aging costs mean latency** |
| C2 | `predictive_noaging` vs `fifo`, mean | −57.6% | [−58.1%, −57.1%] | reported |
| C3 | `predictive_noaging` vs `fifo`, P99 | +13.6% | [+12.6%, +14.6%] | reported |
| C4 | `predictive_noaging` vs `fifo`, long-class max wait | +13.6% | [+12.0%, +14.7%] | reported |
| C5, C6 | `predictive_noaging` vs `predictive`, P99 and long max wait | +10.1%, +12.2% | | reported |

- C2 equals the RabbitMQ sidecar's −57.6% in E3. The two ran on different host layouts with different baselines, so the match is
  not tested, but it is what the paper's explanation predicts, and it is no longer only an explanation.
- **What aging buys:** without it, P99 and the long class's maximum wait rise about 10 to 14%. In runs of five minutes nothing
  starves; the guardrail runs of the 30-minute E2 and the traces are where aging matters.
- It agrees with two other results: weights with pausing aging cut the mean by 38 to 42% against predictive (E5), and in the
  simulator the aging threshold dominated gate A (`v4-sensitivity-1`).

## Files

| File | Content |
|---|---|
| `report.md`, `summary.json` | the pre-registered analysis (`python -m benchmarks.prodflow_report benchmarks/configs/v4-baselines.json <raw>`) |
| `runs/seed-<seed>-<arm>.json` | every run's `run.json` |
| `SHA256SUMS` | hashes of every raw file, paths relative to the raw `e7/` directory |
| `manifest.txt`, `host-client.txt`, `progress.log` | commit, image, host and run log |
