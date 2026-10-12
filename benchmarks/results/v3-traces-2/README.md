# Job queues that do not recur (`v3-traces-2`, issue #46)

Pre-registered in [docs/v3-traces-2-preregistration.md](../../../docs/v3-traces-2-preregistration.md), frozen in commit
`695699b` before any scheduling arm was simulated on either log.

| | |
|---|---|
| Simulator | `harbinger_trace_sim` built from `695699b` (binary SHA-256 `b272b6d4…09d5`, the same as for `v3-traces-1`) |
| Inputs | SHA-256 matched the configs |
| Run time | 5.5 s (LPC-EGEE) and 1.2 s (DAS2-fs2) |

Data checks: [pwa-datacheck](../pwa-datacheck/README.md).

## Result: no gate passed on either log

| Gate | LPC 0.5 | LPC 0.8 | LPC 0.95 | DAS2 0.5 | DAS2 0.8 | DAS2 0.95 |
|---|---|---|---|---|---|---|
| A: predictive routing as built | not passed | not passed | not passed | not passed | not passed | not passed |
| B: + weights `[8, 3, 1]` + pausing aging | not passed | not passed | not passed | not passed | not passed (guardrails failed) | not passed (guardrails failed) |

### Mean latency vs FIFO

| Arm | LPC 0.5 | LPC 0.8 | LPC 0.95 | DAS2 0.5 | DAS2 0.8 | DAS2 0.95 |
|---|---|---|---|---|---|---|
| A `predictive_warm` | +0.0% | −0.0% | −0.0% | −0.0% | −0.0% | +0.0% |
| B `predictive_warm+weights+pausing` | +1.0% | +1.9% | +8.5% | +2.7% | **+88.8%** | **+72.2%** |
| `oracle` (strict, today's aging) | −0.0% | −0.0% | −0.0% | −0.0% | −0.1% | −0.0% |
| `oracle+weights+pausing` | −33.1% | −52.1% | −53.8% | −6.0% | −30.1% | −49.2% |

### Guardrails, B vs FIFO (upper bounds; limit 2×)

| | LPC 0.5 | LPC 0.8 | LPC 0.95 | DAS2 0.5 | DAS2 0.8 | DAS2 0.95 |
|---|---|---|---|---|---|---|
| All-message P99 | 1.00 | 1.14 | 1.19 | 1.00 | **5.06** (fail) | **10.68** (fail) |
| Long-job (≥ 10 min) max wait | 1.01 | 1.14 | 1.21 | 1.54 | **10.99** (fail) | **6.28** (fail) |

Gate A's guardrails all passed: it is FIFO in effect. The view without the extreme window gives the same gate outcomes;
full tables are in `<log>/report.md`.

## What the results show

**Gate A collapses completely.** Every strict-priority arm, including the oracle, is within 0.1% of FIFO on both logs.
Waits here are minutes to days, and the broker's default 5 s aging lifts every waiting job to the top level. This is
the Azure collapse in its extreme form, as expected.

**On LPC-EGEE the predictor mostly does not predict.** Only 24,402 of 117,823 evaluation jobs got a prediction:

| Outcome | Jobs |
|---|---|
| Predicted | 24,402 |
| Stale key | 62,046 |
| Cold key | 28,399 |
| High spread | 2,976 |

The predictor drops a key's history after 30 minutes without activity (`stale_after`), and on this log most keys are
idle longer than that. The rest are too new or too variable. So most jobs take the default tier, and gate B stays near
FIFO (+1% to +9%, intervals including 0).

**On DAS2-fs2, gate B trades long jobs for short ones and loses on balance.** Load 0.8, by window:

| Window | Mean, FIFO → B → oracle+B | Short-job median, FIFO → B | Longest wait of a long job, FIFO → B |
|---|---|---|---|
| 2 | 4.6 h → 13.2 h → 2.9 h | 21 min → 1.9 min | 11.6 h → 53.3 h |
| 3 | 19.7 h → 38.7 h → 8.5 h | 12.5 h → 2.1 h | 25.1 h → 61.5 h |
| 4 | 16.8 h → 15.1 h → 6.7 h | 14.0 h → 2.2 h | 21.0 h → 57.0 h |
| 5 | 4.5 h → 2.2 h → 2.1 h | 4.3 h → 1.2 h | 6.6 h → 25.7 h |

- **Short jobs gain 4–11×. Long jobs wait 2.5–4.6× longer.**
- Long jobs carry 88% of the work, so the mean worsens, and P99 and long-job waits breach the 2× guardrails.
- The oracle with the same scheduler improves every window without that cost.
- **Why:** see the exploratory diagnosis below. Wrong predictions misorder jobs, and the 5 s pausing aging promotes
  large jobs over hours-long waits.

**The scheduler is not the limit: the predictions and their time-scale settings are.** With perfect sizes the same
overload design cuts mean latency 30–54% at loads 0.8–0.95 on both logs.

## Exploratory diagnosis of the DAS2-fs2 harm (not pre-registered)

Run after the results above, to explain them, on DAS2-fs2 at load 0.8 only. These are extra arms of the same simulator
(`exploratory-das2-diagnosis/config.json` and `simulation.json`). They are not part of any gate.

Mean latency over all evaluation jobs (the gated tables above average per-window changes instead, so gate B shows +89%
there):

| Variant | Mean | vs FIFO | Short | Medium | Long | Longest long-job wait |
|---|---|---|---|---|---|---|
| FIFO | 3.82 h | — | 0.70 h | 6.52 h | 12.44 h | 25.1 h |
| Predictions, strict, 5 s aging (gate A) | 3.82 h | 0% | 0.70 h | 6.52 h | 12.44 h | 25.1 h |
| Predictions, strict, **no aging** | 5.44 h | +43% | 0.20 h | 9.36 h | 24.03 h | 73.9 h |
| Predictions, weights `[8, 3, 1]`, **no aging** | 3.56 h | −7% | 0.65 h | 6.27 h | 10.44 h | 32.2 h |
| Predictions, weights, pausing aging (gate B) | 5.51 h | +44% | 0.27 h | 9.61 h | 22.81 h | 61.5 h |
| Predictions, weights `[4, 2, 1]`, pausing aging | 5.32 h | +39% | 0.48 h | 9.24 h | 20.60 h | 58.3 h |
| Oracle, strict, no aging | 1.68 h | −56% | 0.03 h | 2.02 h | 13.16 h | 27.5 h |
| Oracle, weights, pausing aging | 1.73 h | −55% | 0.05 h | 2.12 h | 13.13 h | 27.5 h |

Predictor outcomes in gate B's run:

| Outcome | Share of jobs |
|---|---|
| Predicted | 63% |
| Stale key | 19% |
| Cold key | 14% |
| High spread | 4% |

**What the diagnosis shows:**

1. **The predictor misorders jobs.**
   - With the predictor's tiers and no aging, short jobs gain (0.70 → 0.20 h), but medium jobs get slower
     (6.5 → 9.4 h) and long jobs wait about twice as long.
   - With correct tiers (the oracle), medium jobs get faster (6.5 → 2.0 h) and long jobs lose 6%.
   - The predictions, about as good as a global median on this log, put jobs in the wrong order.
2. **Wrong guesses are expensive here.** Workers do not preempt. A job predicted short that runs for a day holds a
   worker for that day, ahead of work that waited. On serverless traces a misjudged job costs about a second.
3. **The 5 s aging is the wrong time scale.**
   - Plain aging promotes everything within seconds, so gate A is exactly FIFO.
   - Pausing aging promotes one job per level every 0.5 s while the level above is not behind. Over waits of hours
     that moves thousands of jobs, large mispredicted ones included, into the upper levels.
   - Weights without aging are 7% better than FIFO; with pausing aging, 44% worse.
4. **Weights alone limit the damage.** The weighted share keeps the lower levels moving, so predictions + weights
   without aging do not lose to FIFO. The softer `[4, 2, 1]` weights change little.

**The method's preconditions, which these logs do not meet:**
- per-key predictability;
- aging thresholds on the jobs' own time scale.

Scaling aging (and `stale_after`) to the workload was not tested; it needs its own pre-registration.

## Against the stated expectations

| Expectation | Outcome |
|---|---|
| Gate A fails on both logs (aging) | Yes, completely (within 0.1% of FIFO) |
| LPC-EGEE gate B fails or gains little | Fails, no gain (+1% to +9%); mostly because keys went stale, which was not anticipated |
| DAS2-fs2 gate B uncertain | Fails, with harm: mean +72% to +89% at 0.8–0.95 |
| Guardrails expected to hold | Held on LPC-EGEE; **failed for gate B on DAS2-fs2** |
| Oracles locate the failure | Yes: oracle+weights gains 30–54% where the predictor fails |

## What this means

- **Harbinger's predictive routing has a boundary.** It helps where keys recur and predict well (serverless:
  `v3-traces-1`). On job queues where keys churn or predict poorly it gives nothing (LPC-EGEE), and with the overload
  design it can do real harm to long jobs (DAS2-fs2).
- **The overload design should only be enabled where per-key prediction has been shown to work,** for example with a
  per-key predictability check like `trace_analysis.py` on the system's own history. That is a deployment guideline,
  and a candidate for an automatic safeguard (future work).
- **Two defaults are tuned for short jobs** and do not suit jobs that run for hours: aging (5 s) and `stale_after`
  (30 min). This version tested them as shipped (zero configuration). Whether scaled settings would rescue the
  predictor is not tested here and needs its own pre-registration.

## Files

- `<log>/simulation.json`: simulator output.
  - SHA-256 LPC-EGEE `16a1bee451a5c00d2fbf201f228c951241341942ddb8f4958eecbe31441d5519`.
  - SHA-256 DAS2-fs2 `c6694f35a37064e622496d4443c69d4ac564e4575f5657c2614114c41548d804`.
- `<log>/report.md`, `<log>/summary.json`: output of `python -m benchmarks.sim_report CONFIG SIMULATION.json`.
- `exploratory-das2-diagnosis/`: the diagnosis above (not pre-registered).
  - `config.json`: the frozen DAS2-fs2 config with load 0.8 and extra arms.
  - `simulation.json`: SHA-256 `7baaa05fa3019d18361299c57430d0c807d91984dc5e3527b33943be81ef1cc6`.

**Reproduce:** convert the logs as in the configs, then run
`harbinger_trace_sim TRACE.csv benchmarks/configs/v3-traces2-<log>.json OUT.json` and the report command.
