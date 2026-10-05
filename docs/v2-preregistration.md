# v2 evaluation pre-registration (issue #23)

**Frozen 2026-10-05, before any predictive or shadow arm was run.** Configuration:
[`benchmarks/configs/v2-prodflow.json`](../benchmarks/configs/v2-prodflow.json) (`v2-prodflow-1`). Analysis:
[`benchmarks/prodflow_report.py`](../benchmarks/prodflow_report.py). The v1 results (issues #4/#5) are preserved
unchanged. Changing anything below after results exist requires a new version, and the old results stay published.

## Why v2

- Issue #4 showed static priority with true classes lowers median latency but leaves P95 flat and raises P99 when
  long jobs are common: non-preemptive reordering cannot shorten a long job's own service time. The v1 budget
  ("10% P95 improvement") could not be met even by a perfect static map.
- Issue #5's synthetic workload made a per-key mean near-optimal by construction.
- **These v2 budgets were chosen after seeing issue #4 data and two FIFO-only load pilots** (used only to set the
  load level). They are not blind. No shadow, predictive, static or oracle arm had been run when they were frozen.

## Hypotheses

1. **H1 (primary).** Learned per-key routing lowers mean latency and short-job latency versus FIFO on a realistic
   mixed workload, without hand configuration and without unacceptable harm to the tail or to long jobs.
2. **H2.** It is no worse than a well-informed static map and better than a plausible misconfigured one.
3. **H3 (zero configuration).** With no job labels at all (key = producer service only) it still beats FIFO.
4. **H4.** Shadow mode costs nothing measurable.

## Workload: production-flow benchmark

An e-commerce platform's background-job queue (the role Celery, Sidekiq or SQS workers play). Five services,
each its own producer connection, submit nine job types through the real `Producer` client; four workers run
them through the real `Consumer` client (which measures handler time and Acks it) against an embedded broker
with aging on (5000/500 ms, the standalone server default).

| Service | Job type | Work (real CPU unless noted) | Body-dependent cost |
|---|---|---|---|
| checkout | `send_order_email` | render 24 KiB HTML + SMTP call (simulated, lognormal median 35 ms) | no |
| checkout | `generate_invoice` | build an invoice with N line items and deflate it at level 9 | yes, line items |
| checkout | `reserve_inventory` | database round trips (simulated, ~6 ms) | no |
| catalog | `resize_image` | fill and box-filter an RGB image with a gamma LUT | yes, pixels (most ~0.8 MP, 8% ~9 MP) |
| catalog | `reindex_product` | tokenize a description into an inverted index | slight |
| notifications | `push_notification` | push gateway call (simulated, ~9 ms) | no |
| notifications | `send_sms` | SMS provider call (simulated, ~80 ms) | no |
| partners | `deliver_webhook` | partner HTTP call: 86% ~40 ms, 12% ~300 ms, 2% time out at 1.5 s, fail and are retried | heavy tail |
| analytics | `export_report` | generate, sort and aggregate 10–30 million order rows (~1–2.6 s) | yes, rows |

Network calls are sleeps from realistic latency distributions (no real SMTP/HTTP). CPU kernels are real and
compete with the broker for the machine's 4 cores.

**Timeline (300 s, open-loop Poisson arrivals, `rate_scale` 1.8):** 0–60 s steady (~60% worker utilization in the
FIFO pilot); 60–90 s ramp; 90–150 s flash sale (checkout ×3, notifications ×2.5, partners ×2; ~125% utilization,
a backlog forms); 150–210 s recovery; 210–300 s shifted regime: 55% of image uploads become high resolution and
a partner degrades (35% slow webhooks). Arrivals stop at 300 s; the run drains for up to 180 s. Traces are
generated deterministically per seed and are identical across arms.

## Arms

| Arm | Routing |
|---|---|
| `fifo` | everything in one tier |
| `static_tuned` | an operator's informed map: fast I/O jobs tier 0; email, invoice, SMS, webhooks tier 1; image resize and report export tier 2 |
| `static_misconfigured` | priority by business importance: checkout, webhooks and the executive report tier 0 |
| `shadow` | predictor runs and learns but routing stays default (all tier 1) |
| `predictive` | per-key predictor, key = producer + `job_type`, all defaults (median, p99/p50 ≤ 16, N_min 20), **cold start** |
| `predictive_producer` | same, key = producer only (no job labels) |
| `oracle` | benchmark-only: tier from the true planned cost, boundaries at the trace's cost terciles |

The static maps are fixed for the whole run, so after the 210 s shift they are stale for image resizing.
Seeds: 11, 22, 33, 44, 55. Arm order is rotated per seed (`benchmarks/run_prodflow.sh`).

## Metrics

Latency = Producer submit to successful handler completion. Per message: wait (submit to first handler start),
slowdown (latency / handler time, 1 ms floor). Reported per arm: mean, P50/P95/P99, short-class P50
(`reserve_inventory`, `push_notification`, `reindex_product`, `generate_invoice`), per-class P50/P99, export max
wait, slowdown P50/P95/P99, throughput, per-phase means, and predictor counters.

## Criteria

All comparisons pair arms by seed. For each criterion the per-seed relative delta `(arm − baseline) / baseline`
(or ratio) is averaged and given a bootstrap 95% interval (10,000 resamples, seed 2026).

| Id | Claim | Passes when |
|---|---|---|
| P1 | predictive mean latency vs FIFO | point ≤ −15% and interval upper bound < 0 |
| P2 | predictive short-class P50 vs FIFO | point ≤ −30% and upper bound < 0 |
| G1 | predictive all-message P99 vs FIFO | ratio interval upper bound ≤ 1.25 |
| G2 | predictive export max wait vs FIFO | ratio upper bound ≤ 2.0 and no export waits over 60 s |
| G3 | no lost work | every message completed, 0 DLQ, 0 submit errors in every predictive run |
| G4 | throughput vs FIFO | ratio lower bound ≥ 0.98 |
| S1 | predictive vs `static_tuned` mean | non-inferior: upper bound ≤ +10% |
| S2 | predictive vs `static_misconfigured` mean | upper bound < 0 |
| S3 | shadow overhead vs FIFO mean | interval within ±5% and mean lookup ≤ 20 µs |
| S4 | `predictive_producer` vs FIFO mean | point ≤ −5% and upper bound < 0 |

**Gate:** predictive routing is eligible for explicit opt-in only if P1, P2, G1, G2, G3 and G4 all pass. S1–S4
are reported as secondary claims either way. With five seeds the intervals are wide; an unevaluable or failed
criterion is a failure, never a pass. Default routing stays static regardless of the result.

## Limitations stated in advance

- One machine (4 vCPU), workers and broker share it; results are not a capacity claim.
- Simulated network latency; real CPU work. No real SMTP, HTTP or databases.
- Aging is on in every arm, which bounds starvation for all policies.
- The predictor starts cold in every run; a warm-start arm is not part of this matrix.
- Synthetic-workload results say nothing about traffic whose cost structure differs from this one.
