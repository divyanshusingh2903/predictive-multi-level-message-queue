# Phase 2 completion report (issue #10)

**Result: the pre-registered production-flow gate PASSED; the pre-registered Azure trace simulation gate did NOT
pass at any load.** Predictive routing is therefore available as an explicit opt-in for workloads that resemble the
production-flow benchmark (a shared job queue whose job types differ in cost and which is not in sustained, hours-long
overload). The default stays static routing. Nothing here was tuned after seeing results: the configurations and
criteria were frozen in [the v2 pre-registration](v2-preregistration.md) before any shadow, predictive, static, oracle
or simulated run.

## 1. Production-flow benchmark (`v2-prodflow-1`)

An e-commerce background-job queue: five services as separate producers, nine job types, real CPU work through the
real `Producer`/`Consumer` clients, simulated network latency, steady load (~60%), a flash-sale overload (~125%), a
recovery and a mid-run cost shift; 300 s of arrivals per run, seven arms, five seeds, 35 runs, about 35,000 messages
per run. Full tables: [`benchmarks/results/v2-prodflow/report.md`](../benchmarks/results/v2-prodflow/report.md).

| Arm | Mean | P50 | P99 | Short-job P50 | Export max wait | Slowdown P50 |
|---|---|---|---|---|---|---|
| FIFO | 8.03 s | 6.03 s | 22.3 s | 6.03 s | 20.6 s | 350 |
| Static, well configured | 6.95 s | 3.70 s | 23.9 s | 1.64 s | 24.9 s | 155 |
| Static, misconfigured | 8.69 s | 7.50 s | 27.6 s | 7.94 s | 19.6 s | 387 |
| Shadow | 7.97 s | 5.92 s | 22.3 s | 5.99 s | 20.6 s | 338 |
| **Predictive** (producer + `job_type`) | **6.16 s** | **2.13 s** | 23.4 s | **0.10 s** | 21.8 s | **98** |
| Predictive, producer only (no labels) | 6.91 s | 3.95 s | 22.9 s | 3.45 s | 22.7 s | 185 |
| Oracle (true planned cost) | 6.64 s | 3.66 s | 24.5 s | 1.05 s | 23.2 s | 159 |

Medians across seeds. Pre-registered criteria (paired per-seed deltas, bootstrap 95% interval):

| Criterion | Point | 95% CI | Result |
|---|---|---|---|
| P1 predictive mean latency vs FIFO (≤ −15%) | −27.9% | [−31.9%, −24.7%] | pass |
| P2 short-class P50 vs FIFO (≤ −30%) | −88.4% | [−99.0%, −72.6%] | pass |
| G1 all-message P99 vs FIFO (≤ 1.25×) | 1.04× | [1.02, 1.06] | pass |
| G2 export max wait vs FIFO (≤ 2×, none over 60 s) | 1.02× | [0.96, 1.05] | pass |
| G3 no lost work | all 35 runs complete, 0 DLQ, 0 submit errors | | pass |
| G4 throughput vs FIFO (≥ 0.98×) | 1.00× | [1.00, 1.00] | pass |
| S1 predictive vs well-configured static (non-inferior within +10%) | −10.9% | [−14.6%, −8.1%] | pass |
| S2 predictive vs misconfigured static | −33.8% | [−38.1%, −30.1%] | pass |
| S3 shadow overhead vs FIFO (within ±5%, lookup ≤ 20 µs) | −0.6% | [−1.2%, 0.0%] | pass |
| S4 zero-config (producer key) vs FIFO (≤ −5%) | −15.4% | [−18.0%, −13.2%] | pass |

What this shows and what it does not:

- **The learned map beat a human-tuned one** (−10.9% mean latency) and was far better than a plausible
  misconfiguration, which was worse than doing nothing. The operator map's biggest misjudgement was invoice
  generation: it sat in the middle tier with e-mail and SMS, but its median work is 4.6 ms. The predictor learned
  that it is among the cheapest jobs (invoice P50 27 ms versus 7.3 s under the static map).
- **Zero configuration works partly.** With no job labels, each service is one key; mixed services fall back or get
  an average tier. It still cut mean latency 15%, about half the labelled benefit.
- **The tail is not improved.** All-message P99 rose 4%; long jobs' own latency is unchanged or slightly worse, which a
  non-preemptive shortest-expected-first queue predicts. Aging (5 s) bounded long-job waits: export max wait was
  within 2% of FIFO.
- **The "oracle" is not optimal.** It ranks each message by its planned cost at fixed terciles, using cost rates
  calibrated on an idle machine; under contention those costs are approximate. Predictive and oracle are statistically
  tied (oracle better on 2 of 5 seeds). Treat the oracle as a reference, not a ceiling.
- **Predictor overhead is negligible:** mean lookup 2.7 µs; shadow mode is indistinguishable from FIFO. 99.5% of
  messages were routed by prediction; the rest were the cold start (first ~190 messages).
- **Limits:** one 4-vCPU machine; simulated network latency; one synthetic scenario, written by us. Five seeds give
  narrow intervals for this scenario, not generality. Container restarts interrupted some runs; every interrupted arm
  was re-run from scratch (the runner never reuses a partial run), and one arm that overlapped a build was re-run
  ([`matrix-log.txt`](../benchmarks/results/v2-prodflow/matrix-log.txt)).

## 2. Azure Functions trace simulation (`v2-azure-sim-1`)

990,476 invocations from the second week of the Azure Functions 2021 trace, replayed through 64 shared non-preemptive
workers with the broker's tier and aging rules; the predictive arms run the broker's own `PerKeyPredictor` on
simulation time. Report: [`benchmarks/results/v2-azure-sim/report.md`](../benchmarks/results/v2-azure-sim/report.md).

| Offered load | Predictive (warm) mean vs FIFO | 95% CI | Gate (P1, G1, G2) |
|---|---|---|---|
| 0.50 | −0.9% | [−1.8%, −0.2%] | not passed (P1) |
| 0.80 | −1.4% | [−2.6%, −0.5%] | not passed (P1) |
| 0.95 | −2.8% | [−5.5%, −0.6%] | not passed (P1) |

Every policy, including the oracle, was within 1–3% of FIFO. The guardrails passed; the improvement criterion failed.

**Why (exploratory diagnosis, not part of the pre-registered result):**

- The trace is extremely bursty under a single shared-worker model: at nominal 50% load the median hour runs at 16%
  but 5 of 197 hours exceed capacity, one by 53×. Those hours create multi-hour backlogs (mean latency over an hour,
  P99 about 10 hours) that dominate every mean.
- With aging on (5 s), every message in a backlog that long is promoted to the top tier within seconds, so all
  policies collapse to FIFO. Re-running with aging disabled (`exploratory-no-aging.json`) separates them: oracle
  −50%, predictive −31%, static history map −2% versus FIFO at load 0.5.
- The cold and warm predictor arms are nearly identical because busy keys relearn from `min_samples` labels within
  seconds, and the keys whose warm history goes stale before their first evaluation call are rarely called (corrected
  in #38; an earlier version of this report blamed staleness for all of it). The same check found that staleness ran
  from each key's last completion, so keys stuck in a backlog fell back as stale; that is fixed
  ([exploratory note](../benchmarks/results/v2-azure-sim/exploratory-staleness/README.md)).
- A pool of 64 workers shared by all functions is not how serverless platforms scale; the model, not just the
  predictor, limits what this trace can show.

**What follows from it:** aging and sustained overload interact: when backlogs last far longer than the aging
threshold, aging turns any priority policy into FIFO. That is a property of the broker's existing aging design, not
of the predictor, and it applies equally to static priority. It is a follow-up, not a reason to change the frozen
result.

## 3. Real-data predictability (issue #24)

On the same trace (first week learn, second week held out), a per-key median predicts held-out durations within a
median factor of 1.3 (62% within 2×, 79% correct tier); a global median is off by a median factor of 29 (10% within
2×, 57% correct tier). 73% of held-out traffic passes the predictor's sample and spread gates; 17% comes from keys
unseen in the first week. File: [`benchmarks/results/azure2021/predictability.json`](../benchmarks/results/azure2021/predictability.json).
No public trace with per-job payload features was found, so features were tested on self-generated data
(`feature-signal-v1`, pre-registered): real handlers on real inputs, six runs, 568,592 jobs. A per-key median has a
median error factor of about 1.95 and 68% tier accuracy; adding a log2 size bin per key cuts that to 1.2 and 93%,
in all six runs; gradient boosting over every feature adds almost nothing more (1.17, 93%). The control key whose cost
does not depend on its payload gains nothing. Decision: support a binned size feature in the key; a richer model is
not warranted. Details: [`benchmarks/results/feature-signal-v1/`](../benchmarks/results/feature-signal-v1/README.md).

## 4. Operating it

- **Enable:** `harbinger_server --routing-mode shadow` first (no routing change, records predictions), then
  `--routing-mode predictive`, or the `routing` section of a [`--config` file](configuration.md). Default: static.
- **Keys:** the broker-assigned producer id, optionally joined to a `job_type` header (producer-scoped). Labelled
  job types roughly doubled the benefit in the benchmark.
- **Levels and boundaries:** `num_levels`/`default_priority` come from the broker config; boundaries are learned global
  quantiles with hysteresis. Readiness: 100 global and 20 per-key samples by default; everything else uses the
  default tier and is counted per fallback reason.
- **Feedback retention:** independent of routing ([feedback.md](feedback.md)); the predictor learns from Ack
  settlement in memory and does not need persistent feedback.
- **State recovery:** optional snapshot (`routing.snapshot`), atomic with a `.prev` copy; a bad or incompatible
  snapshot starts cold and is reported at startup. Keys idle beyond `stale_after` are relearned.
- **Rollback:** one step: restart with `--routing-mode static`. Predictor state rollback is separate: delete the
  snapshot or restore `.prev`.
- **Watch:** `--stats-interval-ms` prints fallback counts, lookup latency, rolling tier agreement, log2 error and drift
  alerts (which never change routing).

## 5. Follow-ups

1. Aging under sustained overload: a weighted share of pulls across levels (#40) so priority still means something in
   long backlogs; evaluate on the Azure trace with a new pre-registration. (Capping promotions was considered and
   rejected: promoted messages still wait behind the whole top level.)
2. Warm start: not needed. Snapshot restore already ignores downtime (key ages are stored relative to save time). The
   real problem was staleness measured from completions during backlogs, fixed in #38.
3. Size-binned keys: implemented as opt-in `routing.key.size_source` (#35, [size bins](duration-predictor.md#size-bins-issue-35)); not yet evaluated end-to-end in the broker. A cloud repeat of the collection remains (#26, `benchmarks/collect/CLOUD.md`).
4. Per-function worker pools in the trace simulator to model serverless scaling.
