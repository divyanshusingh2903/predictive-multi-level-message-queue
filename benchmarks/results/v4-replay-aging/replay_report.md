# Real trace replay (v4-replay-aging-1, Huawei Public Cloud Trace 2025, request tables, Region 5 (day 30))

64 workers, time scale 1, 28 windows of equal trace time; primary interval: moving-block bootstrap, block 4, 10000 resamples. The independent seven-window interval of v3-traces-1 is listed after.

## Runs left out

none

## Offered load 0.80

Repeats used (valid for every arm): rep-1, rep-2, rep-3.

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 17.73 | 9081 | 60.4 | 9019 | 31.0 | 61.2 | 333.3 / 52682 |
| predictive_warm | 14.81 | 4949 | 56.3 | 4800 | 31.8 | 62.3 | 164.2 / 44032 |
| predictive_warm+no_aging | 1.68 | 57 | 52.4 | 49 | 36.5 | 73.8 | 2.6 / 376 |

Block bootstrap (primary):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -11.0% | [-15.1%, -1.7%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.01× | [1.00, 1.04] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.04× | [1.00, 1.08] | **pass** |
| N_P1_mean_latency_vs_fifo | predictive_warm+no_aging vs fifo | -37.4% | [-66.0%, -4.4%] | **pass** |
| N_G1_all_p99_vs_fifo | predictive_warm+no_aging vs fifo | 1.07× | [1.01, 1.15] | **pass** |
| N_G2_long_max_wait_vs_fifo | predictive_warm+no_aging vs fifo | 1.16× | [1.02, 1.23] | **pass** |
| N_vs_A_mean_latency | predictive_warm+no_aging vs predictive_warm | -34.1% | [-64.4%, -2.5%] | **pass** |

**Gate A_predictive_routing at load 0.80: NOT PASSED.**

**Gate N_no_aging at load 0.80: PASSED.**

Independent seven windows (v3-traces-1 rule):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -14.3% | [-25.7%, -4.1%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 0.97× | [0.93, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.03× | [1.01, 1.07] | **pass** |
| N_P1_mean_latency_vs_fifo | predictive_warm+no_aging vs fifo | -47.6% | [-79.3%, -13.4%] | **pass** |
| N_G1_all_p99_vs_fifo | predictive_warm+no_aging vs fifo | 1.05× | [0.94, 1.18] | **pass** |
| N_G2_long_max_wait_vs_fifo | predictive_warm+no_aging vs fifo | 1.22× | [1.05, 1.43] | **pass** |
| N_vs_A_mean_latency | predictive_warm+no_aging vs predictive_warm | -42.5% | [-77.0%, -12.7%] | **pass** |

**Gate A_predictive_routing (seven windows): NOT PASSED.**

**Gate N_no_aging (seven windows): PASSED.**

Each repeat on its own:

- rep-1: {'A_predictive_routing': False, 'N_no_aging': True}
- rep-2: {'A_predictive_routing': False, 'N_no_aging': True}
- rep-3: {'A_predictive_routing': False, 'N_no_aging': True}

## Offered load 0.95

Repeats used (valid for every arm): rep-1, rep-2, rep-3.

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 41.79 | 31356 | 99.9 | 31210 | 55.7 | 101.4 | 1462.3 / 95166 |
| predictive_warm | 37.81 | 28956 | 94.8 | 28714 | 56.7 | 102.9 | 1215.9 / 85595 |
| predictive_warm+no_aging | 7.17 | 95 | 102.6 | 85 | 65.7 | 111.7 | 4.9 / 2624 |

Block bootstrap (primary):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -8.5% | [-11.7%, -3.2%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.01× | [0.99, 1.04] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.02× | [1.00, 1.04] | **pass** |
| N_P1_mean_latency_vs_fifo | predictive_warm+no_aging vs fifo | -45.4% | [-73.6%, -16.2%] | **pass** |
| N_G1_all_p99_vs_fifo | predictive_warm+no_aging vs fifo | 1.12× | [1.02, 1.18] | **pass** |
| N_G2_long_max_wait_vs_fifo | predictive_warm+no_aging vs fifo | 1.14× | [1.03, 1.21] | **pass** |
| N_vs_A_mean_latency | predictive_warm+no_aging vs predictive_warm | -41.8% | [-71.2%, -12.9%] | **pass** |

**Gate A_predictive_routing at load 0.95: NOT PASSED.**

**Gate N_no_aging at load 0.95: PASSED.**

Independent seven windows (v3-traces-1 rule):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -9.6% | [-13.7%, -5.5%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 0.97× | [0.95, 0.99] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.01× | [0.99, 1.02] | **pass** |
| N_P1_mean_latency_vs_fifo | predictive_warm+no_aging vs fifo | -50.8% | [-77.7%, -24.0%] | **pass** |
| N_G1_all_p99_vs_fifo | predictive_warm+no_aging vs fifo | 1.05× | [1.00, 1.12] | **pass** |
| N_G2_long_max_wait_vs_fifo | predictive_warm+no_aging vs fifo | 1.08× | [1.02, 1.17] | **pass** |
| N_vs_A_mean_latency | predictive_warm+no_aging vs predictive_warm | -45.4% | [-75.1%, -15.6%] | **pass** |

**Gate A_predictive_routing (seven windows): NOT PASSED.**

**Gate N_no_aging (seven windows): PASSED.**

Each repeat on its own:

- rep-1: {'A_predictive_routing': False, 'N_no_aging': True}
- rep-2: {'A_predictive_routing': False, 'N_no_aging': True}
- rep-3: {'A_predictive_routing': False, 'N_no_aging': True}

## Against the simulator

Mean-latency change against FIFO (all messages), replay and simulator, and their ratio; gate verdicts side by side.

### Load 0.80

| Arm | Replay | Simulator | Ratio |
|---|---|---|---|
| predictive_warm | -16.4% | -16.9% | 0.97 |

- gate A_predictive_routing: replay fail, simulator pass (differs)

### Load 0.95

| Arm | Replay | Simulator | Ratio |
|---|---|---|---|
| predictive_warm | -9.5% | -9.8% | 0.97 |

- gate A_predictive_routing: replay fail, simulator fail

## Run-to-run spread (all-message mean latency over valid repeats)

| Load | Arm | Repeats | Min s | Max s | Max/min |
|---|---|---|---|---|---|
| 0.8 | fifo | 3 | 17.72 | 17.75 | 1.002 |
| 0.8 | predictive_warm | 3 | 14.81 | 14.82 | 1.000 |
| 0.8 | predictive_warm+no_aging | 3 | 1.65 | 1.73 | 1.050 |
| 0.95 | fifo | 3 | 41.77 | 41.81 | 1.001 |
| 0.95 | predictive_warm | 3 | 37.81 | 37.82 | 1.000 |
| 0.95 | predictive_warm+no_aging | 3 | 7.12 | 7.26 | 1.019 |


Latency basis: measured from the actual submit instead of the scheduled arrival, mean latency differs by at most 0.01% on any valid run.
