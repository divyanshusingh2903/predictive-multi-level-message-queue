# Real trace replay (v4-replay-1, Huawei Public Cloud Trace 2025, request tables, Region 5 (day 30))

64 workers, time scale 1, 28 windows of equal trace time; primary interval: moving-block bootstrap, block 4, 10000 resamples. The independent seven-window interval of v3-traces-1 is listed after.

## Runs left out

none

## Offered load 0.50

Repeats used (valid for every arm): rep-1, rep-2, rep-3.

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 0.18 | 26 | 1.5 | 26 | 10.3 | 1.6 | 1.0 / 48 |
| oracle | 0.17 | 25 | 1.6 | 25 | 10.3 | 2.0 | 1.0 / 10 |
| oracle+weights+pausing | 0.17 | 26 | 1.6 | 25 | 10.3 | 2.0 | 1.0 / 11 |
| predictive_cold | 0.16 | 26 | 1.3 | 25 | 10.3 | 3.9 | 1.0 / 11 |
| predictive_warm | 0.16 | 26 | 1.3 | 25 | 10.3 | 3.1 | 1.0 / 7 |
| predictive_warm+weights+pausing | 0.16 | 26 | 1.3 | 25 | 10.3 | 2.8 | 1.0 / 8 |
| static_history | 0.16 | 26 | 1.3 | 25 | 10.3 | 2.9 | 1.0 / 7 |

Block bootstrap (primary):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -2.5% | [-4.6%, +0.0%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.01] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.03× | [1.00, 1.06] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -2.4% | [-4.4%, +0.0%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.00× | [1.00, 1.01] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.03× | [1.00, 1.06] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -0.4% | [-0.7%, +0.4%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | -0.0% | [-0.1%, +0.1%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -2.3% | [-4.1%, +0.0%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -1.8% | [-3.2%, +0.0%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -1.8% | [-3.2%, +0.0%] | **fail** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | +0.3% | [+0.0%, +0.5%] | **fail** |

**Gate A_predictive_routing at load 0.50: NOT PASSED.**

**Gate B_overload_design at load 0.50: NOT PASSED.**

Independent seven windows (v3-traces-1 rule):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -5.0% | [-14.4%, -0.1%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 0.97× | [0.92, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.13× | [1.00, 1.39] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -4.7% | [-13.7%, -0.0%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 0.97× | [0.92, 1.00] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.11× | [1.00, 1.33] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -0.5% | [-1.4%, +0.4%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | -0.0% | [-0.2%, +0.1%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -4.4% | [-12.7%, -0.1%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -3.5% | [-9.9%, -0.1%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -3.4% | [-9.8%, -0.1%] | **fail** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | +0.3% | [+0.0%, +1.0%] | **fail** |

**Gate A_predictive_routing (seven windows): NOT PASSED.**

**Gate B_overload_design (seven windows): NOT PASSED.**

Each repeat on its own:

- rep-1: {'A_predictive_routing': False, 'B_overload_design': False}
- rep-2: {'A_predictive_routing': False, 'B_overload_design': False}
- rep-3: {'A_predictive_routing': False, 'B_overload_design': False}

## Offered load 0.80

Repeats used (valid for every arm): rep-1, rep-2, rep-3.

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 17.89 | 9306 | 60.8 | 9231 | 31.2 | 61.6 | 341.0 / 53044 |
| oracle | 14.95 | 4508 | 60.0 | 3978 | 31.8 | 61.7 | 118.5 / 43747 |
| oracle+weights+pausing | 6.82 | 33 | 61.4 | 32 | 33.5 | 64.8 | 1.8 / 1754 |
| predictive_cold | 15.54 | 5592 | 57.5 | 5391 | 32.5 | 63.3 | 180.6 / 45396 |
| predictive_warm | 14.97 | 5088 | 56.7 | 4948 | 32.0 | 62.7 | 172.1 / 44415 |
| predictive_warm+weights+pausing | 1.72 | 65 | 51.5 | 56 | 36.7 | 74.4 | 3.0 / 373 |
| static_history | 14.68 | 4115 | 55.8 | 3682 | 32.0 | 62.3 | 114.4 / 44128 |

Block bootstrap (primary):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -10.9% | [-14.8%, -1.6%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.01× | [1.00, 1.04] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.04× | [1.00, 1.08] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -37.0% | [-66.0%, -4.0%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.07× | [1.01, 1.15] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.14× | [1.02, 1.21] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -13.0% | [-17.4%, -2.3%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +8.2% | [+0.1%, +17.2%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -9.7% | [-13.7%, -1.5%] | **pass** |
| S5_oracle_vs_fifo | oracle vs fifo | -12.7% | [-17.9%, -2.0%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -29.3% | [-48.9%, -5.5%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -33.4% | [-64.4%, -1.9%] | **pass** |

**Gate A_predictive_routing at load 0.80: NOT PASSED.**

**Gate B_overload_design at load 0.80: PASSED.**

Independent seven windows (v3-traces-1 rule):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -14.1% | [-25.4%, -4.0%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 0.97× | [0.94, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.03× | [1.01, 1.07] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -46.9% | [-79.3%, -13.5%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.05× | [0.94, 1.18] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.18× | [1.05, 1.32] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -26.1% | [-49.6%, -4.4%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +9.5% | [+0.2%, +26.9%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -13.3% | [-25.0%, -3.1%] | **pass** |
| S5_oracle_vs_fifo | oracle vs fifo | -16.6% | [-31.9%, -3.8%] | **pass** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -37.9% | [-62.7%, -11.9%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -41.4% | [-76.8%, -12.7%] | **pass** |

**Gate A_predictive_routing (seven windows): NOT PASSED.**

**Gate B_overload_design (seven windows): PASSED.**

Each repeat on its own:

- rep-1: {'A_predictive_routing': False, 'B_overload_design': True}
- rep-2: {'A_predictive_routing': False, 'B_overload_design': True}
- rep-3: {'A_predictive_routing': False, 'B_overload_design': True}

## Offered load 0.95

Repeats used (valid for every arm): rep-1, rep-2, rep-3.

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 42.05 | 31537 | 100.3 | 31416 | 56.0 | 101.8 | 1477.0 / 95588 |
| oracle | 37.47 | 27734 | 97.8 | 27008 | 56.7 | 102.4 | 1123.9 / 85789 |
| oracle+weights+pausing | 14.02 | 39 | 103.2 | 37 | 60.7 | 107.2 | 2.5 / 3011 |
| predictive_cold | 38.94 | 30148 | 96.4 | 29954 | 57.6 | 104.5 | 1299.1 / 86769 |
| predictive_warm | 38.07 | 29362 | 95.2 | 29058 | 57.0 | 103.2 | 1236.8 / 86071 |
| predictive_warm+weights+pausing | 7.07 | 136 | 103.2 | 120 | 65.9 | 111.9 | 6.7 / 2677 |
| static_history | 37.19 | 26213 | 95.5 | 25904 | 57.0 | 102.9 | 1151.1 / 85884 |

Block bootstrap (primary):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -8.4% | [-11.5%, -3.2%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.01× | [0.99, 1.04] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.02× | [1.00, 1.04] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -47.1% | [-74.8%, -17.9%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.11× | [1.02, 1.18] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.14× | [1.03, 1.21] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -14.1% | [-21.6%, -4.7%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +28.6% | [+0.5%, +58.2%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -7.0% | [-9.7%, -2.8%] | **pass** |
| S5_oracle_vs_fifo | oracle vs fifo | -13.5% | [-19.1%, -5.1%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -48.1% | [-68.9%, -23.9%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -43.8% | [-72.5%, -14.7%] | **pass** |

**Gate A_predictive_routing at load 0.95: NOT PASSED.**

**Gate B_overload_design at load 0.95: PASSED.**

Independent seven windows (v3-traces-1 rule):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -9.8% | [-14.3%, -5.5%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 0.97× | [0.95, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.01× | [1.00, 1.02] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -53.3% | [-78.5%, -27.0%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.05× | [0.99, 1.12] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.08× | [1.01, 1.16] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -10.2% | [-14.9%, -5.5%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +26.3% | [+1.4%, +68.6%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -8.5% | [-12.4%, -4.7%] | **pass** |
| S5_oracle_vs_fifo | oracle vs fifo | -16.8% | [-28.5%, -7.1%] | **pass** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -55.9% | [-71.6%, -35.5%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -48.0% | [-76.2%, -18.8%] | **pass** |

**Gate A_predictive_routing (seven windows): NOT PASSED.**

**Gate B_overload_design (seven windows): PASSED.**

Each repeat on its own:

- rep-1: {'A_predictive_routing': False, 'B_overload_design': True}
- rep-2: {'A_predictive_routing': False, 'B_overload_design': True}
- rep-3: {'A_predictive_routing': False, 'B_overload_design': True}

## Against the simulator

Mean-latency change against FIFO (all messages), replay and simulator, and their ratio; gate verdicts side by side.

### Load 0.50

| Arm | Replay | Simulator | Ratio |
|---|---|---|---|
| oracle | -7.8% | -7.0% | 1.12 |
| oracle+weights+pausing | -7.8% | -7.0% | 1.12 |
| predictive_cold | -10.1% | -9.3% | 1.08 |
| predictive_warm | -11.4% | -10.6% | 1.08 |
| predictive_warm+weights+pausing | -10.9% | -9.6% | 1.13 |
| static_history | -11.3% | -10.4% | 1.09 |

- gate A_predictive_routing: replay fail, simulator fail
- gate B_overload_design: replay fail, simulator fail

### Load 0.80

| Arm | Replay | Simulator | Ratio |
|---|---|---|---|
| oracle | -16.4% | -16.9% | 0.97 |
| oracle+weights+pausing | -61.8% | -61.7% | 1.00 |
| predictive_cold | -13.1% | -13.5% | 0.97 |
| predictive_warm | -16.3% | -16.9% | 0.97 |
| predictive_warm+weights+pausing | -90.4% | -89.7% | 1.01 |
| static_history | -17.9% | -18.4% | 0.98 |

- gate A_predictive_routing: replay fail, simulator pass (differs)
- gate B_overload_design: replay pass, simulator pass

### Load 0.95

| Arm | Replay | Simulator | Ratio |
|---|---|---|---|
| oracle | -10.9% | -11.2% | 0.97 |
| oracle+weights+pausing | -66.7% | -66.6% | 1.00 |
| predictive_cold | -7.4% | -7.7% | 0.96 |
| predictive_warm | -9.5% | -9.8% | 0.97 |
| predictive_warm+weights+pausing | -83.2% | -82.8% | 1.01 |
| static_history | -11.6% | -11.8% | 0.98 |

- gate A_predictive_routing: replay fail, simulator fail
- gate B_overload_design: replay pass, simulator pass

## Run-to-run spread (all-message mean latency over valid repeats)

| Load | Arm | Repeats | Min s | Max s | Max/min |
|---|---|---|---|---|---|
| 0.5 | fifo | 3 | 0.18 | 0.18 | 1.000 |
| 0.5 | oracle | 3 | 0.17 | 0.17 | 1.001 |
| 0.5 | oracle+weights+pausing | 3 | 0.17 | 0.17 | 1.000 |
| 0.5 | predictive_cold | 3 | 0.16 | 0.16 | 1.006 |
| 0.5 | predictive_warm | 3 | 0.16 | 0.16 | 1.005 |
| 0.5 | predictive_warm+weights+pausing | 3 | 0.16 | 0.16 | 1.006 |
| 0.5 | static_history | 3 | 0.16 | 0.16 | 1.002 |
| 0.8 | fifo | 3 | 17.88 | 17.89 | 1.001 |
| 0.8 | oracle | 3 | 14.93 | 14.97 | 1.003 |
| 0.8 | oracle+weights+pausing | 3 | 6.82 | 6.83 | 1.002 |
| 0.8 | predictive_cold | 3 | 15.52 | 15.57 | 1.003 |
| 0.8 | predictive_warm | 3 | 14.95 | 14.99 | 1.003 |
| 0.8 | predictive_warm+weights+pausing | 3 | 1.72 | 1.72 | 1.003 |
| 0.8 | static_history | 3 | 14.66 | 14.71 | 1.003 |
| 0.95 | fifo | 3 | 42.02 | 42.06 | 1.001 |
| 0.95 | oracle | 3 | 37.44 | 37.50 | 1.002 |
| 0.95 | oracle+weights+pausing | 3 | 14.00 | 14.04 | 1.003 |
| 0.95 | predictive_cold | 3 | 38.89 | 38.97 | 1.002 |
| 0.95 | predictive_warm | 3 | 38.04 | 38.09 | 1.001 |
| 0.95 | predictive_warm+weights+pausing | 3 | 6.89 | 7.25 | 1.052 |
| 0.95 | static_history | 3 | 37.17 | 37.21 | 1.001 |


Latency basis: measured from the actual submit instead of the scheduled arrival, mean latency differs by at most 0.06% on any valid run.

## Overhead probe (light load; latency minus the job's own duration, trace ms)

- fifo/0.1/rep-1: p50 0.52, p99 1.27 over 54547 messages
