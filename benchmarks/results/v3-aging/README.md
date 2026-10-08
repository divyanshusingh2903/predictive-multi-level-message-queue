# Aging under sustained overload (`v3-aging-1`, issue #40)

Pre-registered in [docs/v3-aging-preregistration.md](../../../docs/v3-aging-preregistration.md) (frozen in commit
`e87f38d` before any registered run). Simulation: `harbinger_trace_sim` built from `e87f38d`, Azure Functions 2021
trace (`.rar` and `.txt` SHA-256 match the config), 27 s on one machine. The `fifo` and `predictive_warm` arms
reproduce the published post-#38 results exactly (`v2-azure-sim/exploratory-staleness/frozen-config-after-fix.json`).

## Azure trace simulation

**Result: the gate passed at load 0.8 only.** Worker-time weights `[8, 3, 1]` with pausing aging cut mean latency
against FIFO at every load, and against today's aging by 14–25%. At load 0.5 the gain was −14.6%, just short of the
−15% bar (its interval is below 0). At load 0.95 the long-job max-wait ratio's interval reached 2.23×, above the 2×
limit. P99 passed at every load.

| Criterion (primary arm vs FIFO) | Load 0.5 | Load 0.8 | Load 0.95 |
|---|---|---|---|
| P1 mean (≤ −15%, interval below 0) | −14.6% [−25.5, −5.9] **fail** | −23.7% [−36.5, −11.7] pass | −27.3% [−40.6, −13.9] pass |
| G1 all-message P99 (≤ 2×) | 1.01× [0.93, 1.08] pass | 1.09× [0.97, 1.23] pass | 1.08× [0.96, 1.22] pass |
| G2 long-job max wait (≤ 2×) | 1.21× [1.03, 1.51] pass | 1.23× [1.00, 1.60] pass | 1.46× [1.04, 2.23] **fail** |
| **Gate** | not passed | **passed** | not passed |

Secondary criteria (point, 95% interval), at loads 0.5 / 0.8 / 0.95:

| Criterion | 0.5 | 0.8 | 0.95 |
|---|---|---|---|
| S1 pausing vs plain aging, mean (interval below 0) | −6.3% pass | −16.8% pass | −21.0% pass |
| S2 pausing vs today's aging, mean (interval below 0) | −13.9% pass | −22.6% pass | −25.0% pass |
| S3 pausing vs no aging, mean (≤ +5%) | −5.5% pass | −2.5% pass | −3.0% pass |
| S4 no aging: long-job max wait vs FIFO (≤ 2×) | 1.41× [1.00, 2.21] fail | 1.48× [1.00, 2.39] fail | 1.48× [1.02, 2.32] fail |
| S5 weights + plain aging vs FIFO, mean (≤ −15%) | −9.3% fail | −8.5% fail | −7.2% fail |
| S6 oracle + weights + pausing vs FIFO, mean (≤ −15%) | −24.1% pass | −34.2% pass | −39.0% pass |
| S7 `[4, 2, 1]` vs FIFO, mean (≤ −15%) | −12.4% fail | −23.1% pass | −26.5% pass |
| S8 `[4, 2, 1]` vs FIFO, P99 (≤ 2×) | 1.01× pass | 1.09× pass | 1.08× pass |

**Without the extreme window** (window 6 at every load; reported, not gated), the verdicts are the same except that P1
at load 0.5 passes (−15.4%). G2 at 0.95 still fails (1.52×, interval up to 2.41).

Full tables: [`sim/report.md`](sim/report.md); raw output: [`sim/simulation.json`](sim/simulation.json).

### What the windows show (descriptive, not pre-registered)

Per-window values, primary arm vs FIFO (from `sim/simulation.json`):

- **Window 6 is a different regime.** FIFO's mean latency there is about 8 hours (28,700–31,800 s). In every other
  window it is under 2 minutes. Window 6 dominates the all-window means in `sim/report.md`, but the criteria pair by
  window, so it counts once. In window 6 pausing gains 6–10%, and no aging gains 3–4%.
- **The gain lives in the ordinary busy windows.** In windows 1, 4 and 5 pausing cuts the mean by 15–52%.
- **G2 fails because of window 5, not the extreme one.** In window 5 the longest wait of a long job is 1,430 s under
  FIFO. With pausing it is 2.1× (load 0.5), 2.3× (0.8) and 3.7× (0.95) that, and with no aging 3.8–4.2×. So pausing
  bounds long-job waits better than no aging, but not within 2× at the highest load. In every other window and load
  the ratio is at most 1.3×.
- **Plain aging with weights does not fully collapse to FIFO** (−7% to −9%), but it is clearly worse than pausing, by 6–21% (S1).
- **Weights with no aging** gain −9.5% / −21.8% / −25.3% against FIFO (paired by window). Pausing is 2.5–5.5% better
  still (S3) and bounds long-job waits better (G2 vs S4), so the pausing promotions help rather than cost.
- **The oracle with the same scheduler gains 24–39%,** so better predictions would still pay off.

## Production-flow guardrails (`v3-aging-prodflow-1`)

`harbinger_prodflow` built from `e87f38d`, 5 seeds × 3 arms, arm order rotated per seed. All 15 runs completed on the
first attempt (07:09–08:31 UTC, one 16-core machine), with no restarts.

**Result: the gate did NOT pass.** One guardrail failed: all-message P99 rose 12% against today's `predictive` arm,
above the 10% limit. Every other guardrail passed, and mean latency fell much more than expected.

| Criterion | Point | 95% CI | Result |
|---|---|---|---|
| G1 export max wait vs FIFO (≤ 2×, none over 60 s) | 1.14× | [1.11, 1.16] | pass (max 30.1 s) |
| G2 all-message P99 vs `predictive` (≤ 1.10) | 1.12× | [1.11, 1.13] | **fail** |
| G3 no lost work | all 15 runs complete, 0 DLQ, 0 submit errors | | pass |
| G4 v2 gain kept: mean vs FIFO (≤ −15%) | −56.9% | [−57.4%, −56.4%] | pass |
| G5 throughput vs FIFO (≥ 0.98) | 1.00× | [1.00, 1.00] | pass |
| S1 mean vs `predictive` (≤ +5%) | **−42.0%** | [−45.5%, −38.7%] | pass |
| S2 `predictive` vs FIFO (≤ −15%; v2 replicates) | −25.4% | [−29.2%, −21.6%] | pass |

Full tables: [`prodflow/report.md`](prodflow/report.md).

**The pre-stated expectation was wrong.** We expected pausing to rarely trigger here and the arm to behave like
`predictive`. In fact the flash-sale and recovery phases build backlogs of 16–23 s under FIFO, well past the 5 s aging
threshold. That is the same collapse as on the Azure trace, at a smaller scale:

- In the steady and ramp phases the two predictive arms are the same (69 vs 69 ms, 381 vs 384 ms).
- In the flash-sale, recovery and shifted phases, weights with pausing cut the mean by 34–50% against `predictive`.
- Short job types go from seconds to milliseconds. Short-class P50 falls from 6.4 s to 18 ms. For example,
  `reserve_inventory` P50 falls from 5.3 s to 17 ms, and `push_notification` P99 from 22.7 s to 0.5 s.
- **The cost is the tail of the longer job types.** `deliver_webhook`, `resize_image`, `send_order_email` and
  `send_sms` see P99 rise from about 28 s to 31–32 s. That is what moves the all-message P99 by 12%. Export max wait
  rose from 26.7 s to 30.1 s, within its guardrail.

This matrix has no weights-only arm, so it cannot separate the share's effect from pausing's.

## Conclusion

Neither gate passed in full, so **`level_weights` stays opt-in and off by default**, with pausing aging on whenever
weights are set. In both studies, weights with pausing keep priority meaningful under sustained overload, where
today's aging collapses to FIFO: on the Azure trace −14% to −25% mean against today's aging, and in the production
flow −42%. The price is a bounded rise in the tail of long and medium jobs: P99 +12% in the production flow, and
long-job max wait up to 1.5× FIFO on the trace (3.7× in one window at load 0.95). Operators who value mean and
short-job latency over the longest jobs' tail should turn it on.

Follow-ups, not part of this study:

- a larger bottom-level weight, to see whether it trades back some of the long-job tail;
- a weights-only arm in the production flow.

Both would need a new pre-registration.

## Files

- `sim/simulation.json`: raw simulator output. `sim/report.md`, `sim/summary.json`: output of
  `python -m benchmarks.sim_report benchmarks/configs/v3-aging.json sim/simulation.json`.
- `prodflow/report.md`, `prodflow/summary.json`: output of
  `python -m benchmarks.prodflow_report benchmarks/configs/v3-aging-prodflow.json <raw dir>`.
- `prodflow/runs/seed-<seed>-<arm>.json`: each run's `run.json`.
- `prodflow/SHA256SUMS`: hashes of every raw file (`raw/seed-*/<arm>/{messages.tsv,run.json}`, 60 MB, not committed).
- `prodflow/matrix-log.txt`: the runner log.

Reproduce: extract the trace (hashes in the config) and run
`harbinger_trace_sim AZURE.txt benchmarks/configs/v3-aging.json OUT.json` (about 30 s). For the production flow, build
with `-DHARBINGER_BUILD_BENCHMARKS=ON` and run
`PRODFLOW_CONFIG=benchmarks/configs/v3-aging-prodflow.json benchmarks/run_prodflow.sh build/benchmarks/harbinger_prodflow <raw dir> 11 22 33 44 55`
(about 1 h 20 min). Absolute numbers depend on the machine.
