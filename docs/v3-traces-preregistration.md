# Real traces under common rules: pre-registration (issue #46, `v3-traces-1`)

**Frozen 2026-10-11, before any `v3-traces-1` simulation of a scheduling arm on the Huawei 2025 trace, and before
any run of the Azure 2021 trace under these rules.**

| | |
|---|---|
| Configurations | [`v3-traces-huawei2025-r5.json`](../benchmarks/configs/v3-traces-huawei2025-r5.json), [`v3-traces-azure2021.json`](../benchmarks/configs/v3-traces-azure2021.json) |
| Simulator | `tools/trace_sim.cpp`; its new options are listed below |
| Analysis | [`benchmarks/sim_report.py`](../benchmarks/sim_report.py) |

Changing anything below after results exist requires a new version, and these results stay published.

## Question

Does per-key duration prediction generalise beyond the one real trace used so far (Azure Functions 2021) to a second
provider (Huawei Cloud 2025)? Two parts:

- **A.** Does the broker's predictive routing as built help: strict priority with today's aging?
- **B.** Does the opt-in overload design from #40 help: worker-time weights `[8, 3, 1]` with pausing aging?

## Traces

| Trace | Rows | Keys | Span | Conversion |
|---|---|---|---|---|
| Huawei Cloud 2025, Region 5 request tables (day 30) | 3,443,079 | 491 | 24 h | [data checks](../benchmarks/results/huawei2025-r5/README.md) |
| Azure Functions 2021 | 1,980,951 | 424 | 14 days | as in `v2-azure-sim-1` |

Both traces are read in the common `harbinger-trace-v1` format ([`analysis/trace_format.py`](../analysis/trace_format.py)).
Source and converted SHA-256 hashes are in the configs.

**Huawei conversion decisions**, each checked on the data:
- The timestamps are completion times, so arrival is `time_frontend − totalCost_frontend`.
- Duration is `totalCost_worker`, because `runtimeCost` is missing (logged as 0) on 1.2% of rows.

**Azure is read through its converted file, not the original.** Rows with equal arrival times (2%) are ordered as in
the file instead of by the original reader's unstable sort. This alone moves the weighted arms by up to 2.5% under the
`v3-aging` rules ([analysis/README.md](../analysis/README.md#common-trace-format-47)). Under the published rules the
other arms do not change.

## Rules common to all traces

New simulator options (each is absent in older configs, which reproduce byte for byte):

| Rule | `v2-azure-sim` / `v3-aging` | `v3-traces-1` | Why |
|---|---|---|---|
| History | first 50% of **rows** | first 50% of the **time span** (`history_split: time`) | Huawei's traffic is concentrated early in the day: half the rows is only its first 5 h. |
| Evaluation windows | 7 of equal **row count** | 7 of equal **duration** (`window_split: time`) | Equal-row windows put four of Huawei's seven windows in overload at mean load 0.8. |
| Metric classes | history terciles of duration | **short < 100 ms ≤ medium < 1 s ≤ long** (`class_bounds_ms`) | Huawei's terciles are 22.7 / 30.4 ms, so "long" would mean 36% of requests. Fixed bounds make "long" mean the same on every trace. |

**Unchanged:**
- **Routing tiers.** Predictor routing and the oracle still use the broker's rule, global terciles of the history
  durations.
- **Simulator settings.** 64 workers, 3 levels, default tier 1, aging 5000 / 500 ms.
- **Loads.** Arrivals are compressed so the mean offered load over the evaluation span is **0.5, 0.8 and 0.95**.
- **Analysis.** Values are paired by window; bootstrap 95% intervals use 10,000 resamples, seed 2026.

**Long-job wait floor.** For the long-job max-wait ratios (G2), both values are first raised to at least the
long-class lower bound: 1 s here (`"floor": "long_class_bound"`).
- Without it, a quiet window where no long job waited under FIFO divides by zero and the criterion is unevaluable.
  This happened in the synthetic check below.
- A long job that waits less than its own minimum duration is not counted as held back. A wait of several seconds
  where FIFO made none still counts at full size: 6 s against 0 s is a ratio of 6.
- The floor follows the long-class bound, not a fixed number. A trace registered with a lower bound, such as a
  millisecond-scale trace with "long" set at 100 ms, gets a 100 ms floor.

**Long jobs must exist.** G2 is unevaluable, and so not passed, unless every compared window holds at least
**30 long jobs** (`min_class_count`). An empty or near-empty long class cannot pass the guardrail by default.

**Why 1 s is the long bound for these two traces.** From durations only, before freezing:

| Trace (evaluation half) | P99 | Long jobs (1 s or more) | Their share of work | Long jobs per window |
|---|---|---|---|---|
| Huawei R5 | 1.26 s | 1.2% of requests | 79% | 348 to 2,195 |
| Azure 2021 | 101 s | 15.1% | 98% | 7,543 to 60,399 |

A later trace sets its own class bounds when it is pre-registered. Its long class must carry a substantial share of
the work and at least 30 jobs in every window.

Window loads at mean 0.8, from arrivals and durations only (computed before freezing; no arm was simulated):

| Trace | Window offered loads |
|---|---|
| Huawei R5 | 1.30, 0.88, 0.69, 0.69, 0.55, 0.60, 0.89 |
| Azure 2021 | 0.25, 0.37, 0.40, 0.26, 0.43, 3.46, 0.44 |

## Arms

| Arm | Routing | Level selection | Aging |
|---|---|---|---|
| `fifo` | one level | – | – |
| `static_history` | each key's history median → tier (well configured) | strict | today's |
| `static_random` | random tier per key, seed 2026 (misconfigured) | strict | today's |
| `predictive_cold` | predictive, empty predictor at evaluation start | strict | today's |
| **`predictive_warm`** | predictive, warm-started on history | strict | today's |
| `oracle` | true duration's tier | strict | today's |
| **`predictive_warm+weights+pausing`** | predictive, warm | weights `[8, 3, 1]` | pausing |
| `oracle+weights+pausing` | true duration's tier | weights `[8, 3, 1]` | pausing |

## Criteria (each trace, each load)

**Gate A, `A_predictive_routing` (arm `predictive_warm`):**

| Criterion | Pass if |
|---|---|
| A_P1 mean latency vs FIFO | point ≤ −15% and interval below 0 |
| A_G1 all-message P99 ratio vs FIFO | interval upper bound ≤ 2.0 |
| A_G2 long-class max wait ratio vs FIFO (floor = long bound, ≥ 30 long jobs per window) | interval upper bound ≤ 2.0 |

**Gate B, `B_overload_design` (arm `predictive_warm+weights+pausing`):** the same three criteria, B_P1, B_G1 and B_G2.

The two gates are reported separately. Their thresholds are those of `v3-aging-1`. `v2-azure-sim-1` used 1.25 for P99;
2.0 is kept here for comparability with v3.

**Secondary (reported, not gated):**

| Criterion | Comparison | Pass if |
|---|---|---|
| S1 | `predictive_warm` short-class P50 vs FIFO | ≤ −30% |
| S2 | `predictive_warm` vs `static_history`, mean | ≤ +10% (non-inferior) |
| S3 | `predictive_warm` vs `static_random`, mean | interval below 0 |
| S4 | `predictive_cold` vs FIFO, mean | ≤ −5% |
| S5 | `oracle` vs FIFO, mean | ≤ −15% (ceiling for A) |
| S6 | `oracle+weights+pausing` vs FIFO, mean | ≤ −15% (ceiling for B) |
| S7 | `predictive_warm+weights+pausing` vs `predictive_warm`, mean | interval below 0 |

**Without the extreme window.** Every criterion is also evaluated without the window with the highest FIFO mean
latency, per load. This view is reported, not gated.

## What was seen before freezing

**Huawei, data only** (in [`benchmarks/results/huawei2025-r5/`](../benchmarks/results/huawei2025-r5/README.md)):
- duration and work distributions, keys, and the size hint;
- per-key predictability on a 50/50 row split;
- offered load per window for both split rules.

One FIFO-only simulation with the `v3-aging` config was run to check the file loads; only its row counts were read.
**No latency of any arm on Huawei has been seen.**

**Azure:**
- All published `v2-azure-sim` and `v3-aging` results, under the old rules.
- The converted file run under the `v3-aging` config, to compare readers. Only the differences between the two
  readers were read.

Azure under these rules has not been run. Because its old-rule results are known, the Azure re-run is a consistency
check; **Huawei is the new evidence.**

**Rules chosen after the descriptive checks.** The split, window and class rules were chosen after seeing the window
loads and tier fractions above, but before any policy result under them.

**Synthetic check of the pipeline.** Both configs were run on a synthetic diurnal trace written for the purpose, to
check that the arms run and the report renders. That check found the unevaluable G2 windows, and the floor was added
before freezing. The floor was then tied to the long-class bound, and the 30-long-jobs requirement added, so the rule
also holds for traces on other time scales.

## Expectations, stated before running

**Huawei:**
- **The work is in a heavy tail:** 0.9% of requests run 1 s or more and carry 63% of the work. Per-key routing should
  send sub-millisecond keys first, the dominant ~28 ms key to the middle tier, and slower keys last. Short requests
  then stop waiting behind multi-second ones.
- **A_P1:** expected to pass at 0.8 and 0.95; uncertain at 0.5, where little queueing happens.
- **The overloaded window** (1.30 at 0.8) may collapse to FIFO under today's aging, as on Azure. That window should
  favour B.
- **A_G2 / B_G2:** in the evaluation half, requests of 30 ms or more carry 93% of the work, and everything faster
  about 7%. Long jobs should be delayed little by what is served ahead of them, so G2 is expected to pass. The 0.95
  load is the risk.
- **The oracle** splits the dominant key across tiers by differences of a few milliseconds. It may be no better than
  the predictor (S5).

**Azure:**
- Six of the seven windows are lightly loaded (0.25–0.44 at 0.8), and one is extreme (3.46).
- **A:** expected to fail P1, as in `v2-azure-sim-1`.
- **B:** uncertain. `v3-aging-1` passed only at 0.8 under equal-row windows; here one extreme window dominates, as in
  `v2-azure-sim-1`.

A failed gate will be published as such.

## Later traces

The Parallel Workloads Archive and Alibaba GPU 2020 traces will be added as `v3-traces-2`. They follow these rules
unless a trace makes one impossible, and that version will be frozen after their conversion and data checks.
- **Multi-processor jobs.** The simulator models one job per worker, so their pre-registration must say how jobs
  using many processors or GPUs are handled.
- **Size-binned arm.** It is left out on Huawei, where request size is uninformative (ρ ≈ 0 within keys). It belongs
  with the Parallel Workloads Archive, whose requested run time is a real estimate.

## Limits

- Simulation, not a broker run: non-preemptive, no network or broker overhead.
- One region of one day for Huawei.
- Arrivals are reconstructed from completion times on both traces.
- The rules were chosen after descriptive checks, as stated above.
- 64 workers and the load scaling are modelling choices; real platforms scale out instead of queueing.
