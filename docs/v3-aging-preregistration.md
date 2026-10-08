# Aging under sustained overload: pre-registration (issue #40, `v3-aging-1`)

**Frozen 2026-10-08, before any `v3-aging-1` run on the Azure trace or the production-flow benchmark.**
Configurations: [`benchmarks/configs/v3-aging.json`](../benchmarks/configs/v3-aging.json) (trace simulation) and
[`benchmarks/configs/v3-aging-prodflow.json`](../benchmarks/configs/v3-aging-prodflow.json) (production flow).
Analysis: [`benchmarks/sim_report.py`](../benchmarks/sim_report.py) and
[`benchmarks/prodflow_report.py`](../benchmarks/prodflow_report.py). Before freezing, the new arms were run only on a
synthetic trace written for the purpose (to check the arms run and the report renders) and in one 20-second
production-flow smoke run (seed 7, not a registered seed; checked only that no work was lost). Changing anything below
after results exist requires a new version, and these results stay published.

## Question

Today's aging moves a message up one level after it has waited `threshold` (5 s) and restarts its wait. In a backlog
lasting hours (the Azure trace's busiest hour carried ~325× a typical hour's work) every message reaches the top level
long before it is served, so every policy collapses to FIFO: with aging, predictive routing was no better than FIFO,
while without aging it was ~33% better. Does the #40 design keep priority meaningful under that overload while still
bounding long-job waits?

## What is evaluated

1. **Worker-time share across levels**, weights `[8, 3, 1]`. Each level has a virtual clock; a pull serves the
   non-empty level with the smallest clock and charges it the message's expected cost ÷ the level's weight. Expected
   cost: the predicted duration if routing attached one, else the level's running average of Ack'd durations, else
   1 ms. An empty level rejoins at the floor of the active clocks, so it cannot save up credit.
2. **Pausing aging** (on by default with weights). Promotion from level L to L−1 is skipped while L−1 is *behind*:
   - L−1 still holds messages that were promoted into it (`priority < original_priority`; a retry resets priority to
     the original, so retried messages do not count), **and**
   - the head of L−1 has been in the broker for at least `threshold`, counted from **arrival**, not from its last
     promotion.
   - With pausing on, each aging pass promotes **at most one message per level**, so a large overdue backlog moves up
     gradually instead of in one avalanche.
   - At normal load the upper levels drain quickly and aging behaves as today; when a backlog clears, aging resumes
     on its own. Plain aging (no pausing) is unchanged, and is kept as an arm.

The simulator (`tools/trace_sim.cpp`) applies the same rules as the broker, including the bottom-level-first pass
order; arms that do not use pausing reproduce the post-#38 results exactly.

## Azure trace simulation (`v3-aging-1`)

The `v2-azure-sim-1` setup, unchanged: Azure Functions 2021 trace (hashes in the config), first half as history, 7
daily evaluation windows, 64 workers, 3 levels, aging 5000/500 ms, offered loads **0.5, 0.8 and 0.95**.

Arms:

| Arm | Routing | Level selection | Aging |
|---|---|---|---|
| `fifo` | one level | – | – |
| `predictive_warm` | predictive, warm-started | strict priority | today's (plain) |
| `predictive_warm+weights+no_aging` | predictive | weights `[8, 3, 1]` | none |
| `predictive_warm+weights` | predictive | weights `[8, 3, 1]` | plain (expected to collapse) |
| **`predictive_warm+weights+pausing`** | predictive | weights `[8, 3, 1]` | **pausing (proposed default)** |
| `oracle+weights+pausing` | true duration's tier | weights `[8, 3, 1]` | pausing |
| `predictive_warm+weights_alt+pausing` | predictive | weights `[4, 2, 1]` (sensitivity) | pausing |

The oracle attaches no predicted duration, so its charges come from the level averages.

Analysis: values paired by evaluation window (7), bootstrap 95% interval (10,000 resamples, seed 2026), as in v2.

**Gate (primary arm `predictive_warm+weights+pausing`, at each of the three loads):**

| Criterion | Pass if |
|---|---|
| P1 mean latency vs FIFO | point ≤ −15% and interval below 0 |
| G1 all-message P99 ratio vs FIFO | interval upper bound ≤ 2.0 |
| G2 long-class max wait ratio vs FIFO | interval upper bound ≤ 2.0 |

**Secondary (reported, not part of the gate):**

- S1 pausing vs plain aging, mean (interval below 0);
- S2 pausing vs today's aging (`predictive_warm`), mean (interval below 0);
- S3 cost of pausing vs no aging, mean (interval upper bound ≤ +5%);
- S4 no-aging arm's long-class max wait vs FIFO (≤ 2×), which shows what aging is still needed for;
- S5 plain aging with weights vs FIFO, mean (≤ −15%), expected to fail;
- S6 oracle with weights and pausing vs FIFO, mean (≤ −15%);
- S7, S8 the `[4, 2, 1]` sensitivity arm vs FIFO: mean (≤ −15%) and P99 (≤ 2×).

**Without the extreme window.** Every criterion is also evaluated with the window that has the highest FIFO mean
latency (per load) removed, so that a single overload event cannot carry or sink the result alone. This view is
reported next to the gated one and is not part of the gate.

## Production-flow benchmark (`v3-aging-prodflow-1`, guardrails)

The `v2-prodflow-1` workload, unchanged (300 s of arrivals, 4 workers, rate scale 1.8, 180 s drain, seeds 11, 22, 33,
44, 55, aging 5000/500 ms, arm order rotated per seed). Arms: `fifo`, `predictive` (today's aging), and
`predictive_weighted` (weights `[8, 3, 1]`, pausing aging by default). Paired by seed, bootstrap as above.

This workload has no hours-long backlog, so the question here is only whether the new default costs anything at
normal load. **Gate (all guardrails, primary arm `predictive_weighted`):**

| Criterion | Pass if |
|---|---|
| G1 export max wait vs FIFO | ratio upper bound ≤ 2.0 and none over 60 s |
| G2 all-message P99 vs `predictive` | ratio upper bound ≤ 1.10 |
| G3 no lost work | every message completed, 0 DLQ, 0 submit errors |
| G4 v2 gain kept: mean vs FIFO | point ≤ −15% and interval below 0 |
| G5 throughput vs FIFO | ratio lower bound ≥ 0.98 |

Secondary: S1 mean vs `predictive` (interval upper bound ≤ +5%); S2 `predictive` vs FIFO (≤ −15%), checking the v2
result replicates.

Run with `PRODFLOW_CONFIG=benchmarks/configs/v3-aging-prodflow.json benchmarks/run_prodflow.sh ...`.

## Expectation, stated before running

- Plain aging with weights should collapse towards FIFO in the long backlogs, as today's aging does.
- Pausing should keep most of the no-aging gain, since in a long backlog it promotes about one message per level per
  pass. Long-job max wait should stay within 2× FIFO because weights guarantee the bottom level ~8% of worker time.
- The gain is concentrated in the overload windows, so P1 may fail without them; the extreme-window view shows this.
- At 0.5 load backlogs are rarer and the gain smaller; P1 may fail there.
- In the production flow, upper levels drain quickly, so pausing should rarely trigger and the arm should behave
  close to `predictive`.

A failed gate will be published as such.

## Limits

- The simulation is non-preemptive and charges expected cost at dispatch, as the broker does. It does not model
  network or broker overhead.
- The Azure trace has no job-type labels; keys are `app/func`.
- The production flow is one synthetic scenario on one 4-vCPU machine.
- Weights `[8, 3, 1]` were chosen by reasoning, not tuned; `[4, 2, 1]` is the only sensitivity check.
