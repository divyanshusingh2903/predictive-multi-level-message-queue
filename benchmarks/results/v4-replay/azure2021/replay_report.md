# Real trace replay (v4-replay-1, AzureFunctionsInvocationTraceForTwoWeeksJan2021)

64 workers, time scale 4.5, 28 windows of equal trace time; primary interval: moving-block bootstrap, block 4, 10000 resamples. The independent seven-window interval of v3-traces-1 is listed after.

## Runs left out

none

## Offered load 0.50

Repeats used (valid for every arm): rep-1.

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 4240.02 | 82 | 35095.6 | 8 | 10798.9 | 35863.9 | 2.3 / 32781047 |
| predictive_warm | 4240.44 | 82 | 35095.5 | 8 | 10802.7 | 35867.5 | 2.4 / 32782179 |
| predictive_warm+weights+pausing | 3804.87 | 81 | 36559.4 | 7 | 10617.7 | 40625.3 | 2.3 / 29093336 |

Block bootstrap (primary):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -0.9% | [-1.9%, -0.2%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.03× | [1.01, 1.06] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -8.0% | [-11.5%, -4.3%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.00× | [0.99, 1.03] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.06× | [1.04, 1.11] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | +0.8% | [-0.7%, +2.1%] | **fail** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -7.2% | [-10.4%, -3.6%] | **pass** |

**Gate A_predictive_routing at load 0.50: NOT PASSED.**

**Gate B_overload_design at load 0.50: NOT PASSED.**

Independent seven windows (v3-traces-1 rule):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -0.7% | [-1.4%, -0.1%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [0.99, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -14.5% | [-24.7%, -5.3%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.00× | [0.94, 1.06] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.07× | [1.02, 1.11] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | +0.6% | [-0.7%, +2.9%] | **fail** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -13.9% | [-24.3%, -4.7%] | **pass** |

**Gate A_predictive_routing (seven windows): NOT PASSED.**

**Gate B_overload_design (seven windows): NOT PASSED.**

Each repeat on its own:

- rep-1: {'A_predictive_routing': False, 'B_overload_design': False}

## Offered load 0.80

Repeats used (valid for every arm): rep-1.

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 4661.92 | 97 | 36049.9 | 13 | 11162.1 | 36946.9 | 2.7 / 35400257 |
| oracle | 4660.53 | 96 | 36044.9 | 12 | 11162.1 | 36946.7 | 2.7 / 35395441 |
| oracle+weights+pausing | 2200.83 | 88 | 35616.7 | 11 | 11172.0 | 36969.5 | 2.7 / 76169 |
| predictive_warm | 4662.19 | 96 | 36048.0 | 12 | 11167.0 | 36948.0 | 2.7 / 35400822 |
| predictive_warm+weights+pausing | 4295.55 | 95 | 38211.7 | 12 | 11042.4 | 39643.2 | 2.6 / 31925904 |

Block bootstrap (primary):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -1.6% | [-3.0%, -0.6%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [0.99, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.01× | [1.00, 1.02] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -15.3% | [-23.9%, -9.0%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.00× | [0.96, 1.03] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.04× | [1.03, 1.07] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -2.2% | [-2.8%, -1.6%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -2.4% | [-4.1%, -1.1%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -24.0% | [-34.3%, -13.1%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -14.2% | [-22.6%, -8.1%] | **pass** |

**Gate A_predictive_routing at load 0.80: NOT PASSED.**

**Gate B_overload_design at load 0.80: PASSED.**

Independent seven windows (v3-traces-1 rule):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -0.8% | [-2.0%, -0.1%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -20.8% | [-32.8%, -8.9%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.06× | [1.00, 1.13] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.09× | [1.03, 1.18] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -2.2% | [-3.2%, -1.2%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -1.6% | [-3.4%, -0.5%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -33.3% | [-49.0%, -18.6%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -20.1% | [-32.2%, -8.4%] | **pass** |

**Gate A_predictive_routing (seven windows): NOT PASSED.**

**Gate B_overload_design (seven windows): PASSED.**

Each repeat on its own:

- rep-1: {'A_predictive_routing': False, 'B_overload_design': True}

## Offered load 0.95

Repeats used (valid for every arm): rep-1.

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 4777.61 | 114 | 36443.2 | 17 | 11261.6 | 37282.1 | 3.0 / 36005571 |
| predictive_warm | 4777.63 | 113 | 36443.9 | 17 | 11265.9 | 37283.8 | 3.0 / 35995238 |
| predictive_warm+weights+pausing | 4334.24 | 99 | 38518.9 | 16 | 11119.8 | 39664.3 | 3.0 / 32837308 |

Block bootstrap (primary):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -2.2% | [-3.9%, -1.1%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 0.99× | [0.97, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.03× | [1.00, 1.05] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -17.2% | [-25.8%, -11.6%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.01× | [0.99, 1.05] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.08× | [1.05, 1.11] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -3.8% | [-8.2%, -1.4%] | **fail** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -15.4% | [-24.1%, -9.7%] | **pass** |

**Gate A_predictive_routing at load 0.95: NOT PASSED.**

**Gate B_overload_design at load 0.95: PASSED.**

Independent seven windows (v3-traces-1 rule):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -1.2% | [-2.7%, -0.2%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.01] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -22.5% | [-35.3%, -9.9%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.12× | [1.02, 1.23] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.13× | [1.04, 1.23] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -1.5% | [-2.4%, -0.7%] | **fail** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -21.6% | [-34.5%, -9.1%] | **pass** |

**Gate A_predictive_routing (seven windows): NOT PASSED.**

**Gate B_overload_design (seven windows): PASSED.**

Each repeat on its own:

- rep-1: {'A_predictive_routing': False, 'B_overload_design': True}

## Against the simulator

Mean-latency change against FIFO (all messages), replay and simulator, and their ratio; gate verdicts side by side.

### Load 0.50

| Arm | Replay | Simulator | Ratio |
|---|---|---|---|
| predictive_warm | +0.0% | +0.0% | 1.13 |
| predictive_warm+weights+pausing | -10.3% | -11.3% | 0.90 |

- gate A_predictive_routing: replay fail, simulator fail
- gate B_overload_design: replay fail, simulator pass (differs)

### Load 0.80

| Arm | Replay | Simulator | Ratio |
|---|---|---|---|
| oracle | -0.0% | -0.0% | 1.00 |
| oracle+weights+pausing | -52.8% | -52.8% | 1.00 |
| predictive_warm | +0.0% | +0.0% | 1.11 |
| predictive_warm+weights+pausing | -7.9% | -7.7% | 1.02 |

- gate A_predictive_routing: replay fail, simulator fail
- gate B_overload_design: replay pass, simulator pass

### Load 0.95

| Arm | Replay | Simulator | Ratio |
|---|---|---|---|
| predictive_warm | +0.0% | -0.0% | -0.26 |
| predictive_warm+weights+pausing | -9.3% | -8.8% | 1.06 |

- gate A_predictive_routing: replay fail, simulator fail
- gate B_overload_design: replay pass, simulator pass


Latency basis: measured from the actual submit instead of the scheduled arrival, mean latency differs by at most 0.00% on any valid run.

## Overhead probe (light load; latency minus the job's own duration, trace ms)

- fifo/0.1/rep-1: p50 4.07, p99 6.80 over 311 messages
