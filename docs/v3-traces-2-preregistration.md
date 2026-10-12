# Job queues that do not recur: pre-registration (issue #46, `v3-traces-2`)

**Frozen 2026-10-12, before any scheduling arm was simulated on either log.**

| | |
|---|---|
| Configurations | [`v3-traces2-lpc-egee-2004.json`](../benchmarks/configs/v3-traces2-lpc-egee-2004.json), [`v3-traces2-das2-fs2-2003.json`](../benchmarks/configs/v3-traces2-das2-fs2-2003.json) |
| Simulator | `tools/trace_sim.cpp` |
| Analysis | [`benchmarks/sim_report.py`](../benchmarks/sim_report.py) |
| Data checks | [`benchmarks/results/pwa-datacheck/`](../benchmarks/results/pwa-datacheck/README.md) |

Changing anything below after results exist requires a new version, and those results stay published.

## Question

`v3-traces-1` tested per-key prediction on serverless traces, where functions recur constantly and per-key medians
predict well (Huawei: 98% of jobs within 2×). Here the question is what happens on job queues where keys predict
poorly: HPC and grid logs from the Parallel Workloads Archive.
- **Does predictive routing still help?**
- **Does it at least do no harm?**

This is the boundary of the claim. The results say where per-key prediction should not be expected to help.

## Traces

| Log | Jobs | Keys | Key | Jobs kept | Data check |
|---|---|---|---|---|---|
| **LPC-EGEE-2004** (grid cluster, 4 months evaluated) | 214,322 | 219 | user + queue | single-processor only, matching the simulator's one job per worker | per-key 32% within 2×, against 43% for a global median |
| **DAS2-fs2-2003** (research cluster, 6 months evaluated) | 64,660 | 2,516 | user + executable | all; **a parallel job occupies one simulated worker for its run time** | per-key 71% within 2×, global 73% |

- Arrival is the submit time and duration the run time.
- The users' requested run times are converted but used by no arm. Comparing them with learned estimates is left to a
  later version.
- Hashes are in the configs.

## Rules

`v3-traces-1`'s rules, unchanged:
- **Split and windows:** history is the first half of the time span; seven equal-time windows.
- **Simulator:** 64 workers, 3 levels, the broker's tercile routing.
- **Loads:** mean offered loads of 0.5, 0.8 and 0.95.
- **Analysis:** windows paired, bootstrap 95% intervals (10,000 resamples, seed 2026). The long-wait guardrail floors
  waits at the long-class bound and needs at least 30 long jobs per window.

**One rule scales with the logs.** The metric classes are short < 1 min ≤ medium < 10 min ≤ long, because jobs here
run for seconds to days. Long jobs are 23.5% of LPC-EGEE's jobs (99% of work) and 6.4% of DAS2-fs2's (88% of work),
with at least 60 per window in both. A 1-hour threshold would have left DAS2 windows with 2–13 long jobs.

**Aging stays at the broker's default:** 5 s threshold, 0.5 s interval. This is the zero-configuration setting. On jobs
that wait minutes, it promotes almost everything, so gate A is expected to collapse towards FIFO, as on Azure. An
operator running hour-long jobs would set a longer threshold. This version does not test that.

## Arms and criteria

Identical to `v3-traces-1`.

**Arms:**
- `fifo`;
- `static_history` and `static_random`;
- `predictive_cold` and `predictive_warm`;
- `oracle`;
- `predictive_warm+weights+pausing` and `oracle+weights+pausing`.

**Gates (each log, each load):**
- **Gate A** (`predictive_warm`) and **Gate B** (`predictive_warm+weights+pausing`), judged separately.
- Each requires all three of:
  - mean latency vs FIFO: point ≤ −15%, interval below 0;
  - all-message P99: ≤ 2× FIFO (upper bound);
  - long-class max wait: ≤ 2× FIFO (upper bound, with the floor).

**Secondary:**
- S1: short-class P50 vs FIFO.
- S2: `predictive_warm` vs `static_history`.
- S3: `predictive_warm` vs `static_random`.
- S4: cold start vs FIFO.
- S5, S6: the oracles vs FIFO.
- S7: B vs A.

The without-extreme-window view is reported, not gated.

## What was seen before drafting

All in [`benchmarks/results/pwa-datacheck/`](../benchmarks/results/pwa-datacheck/README.md):
- the field survey of 16 logs;
- per-key predictability and users' estimate errors;
- the class shares;
- the window loads at mean 0.8:
  - LPC-EGEE: 0.61, 0.44, 0.84, 0.82, 1.06, 1.48, 0.35;
  - DAS2-fs2: 0.10, 2.01, 1.32, 1.17, 0.55, 0.23, 0.22.

Both configs were also run on a synthetic trace at HPC time scale, to check the arms run and nothing is unevaluable.
**No arm has been simulated on either log.**

## Expectations, stated before running

- **Gate A fails on both logs.** Waits of minutes pass the 5 s aging threshold, so aging lifts everything (as on
  Azure).
- **LPC-EGEE, gate B: likely fails or gains little.** Keys predict worse than a global median, so tiers are close to
  arbitrary. The weights still let short-tier work through during backlogs, so a small gain is possible.
- **DAS2-fs2, gate B: uncertain.** Keys recur but add little over a global median. The heavy tail (0.3% of jobs carry
  74% of the work) leaves room for gains if those keys are recognised.
- **Oracles.** The oracle arms show what perfect sizes would give. A large oracle gain with no predictive gain would
  locate the failure in prediction, not scheduling.
- **Guardrails.** Expected to hold. A poor predictor misorders jobs but should not starve long ones under pausing
  aging.

A failed gate will be published as such.

## Limits

- Simulation only. Old logs: 2003–2004.
- DAS2-fs2's parallel jobs are simplified to single workers.
- Aging is left at the default, which does not suit hour-long jobs.
- Rules were inherited from `v3-traces-1`, except the class bounds, which were chosen after the data checks.
