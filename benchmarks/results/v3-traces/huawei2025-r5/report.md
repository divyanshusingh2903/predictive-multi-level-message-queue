# Huawei Cloud 2025 Region 5 trace simulation (v3-traces-1)

483183 simulated invocations after 2959896 history invocations; tier boundaries (history terciles) 23 / 31 ms; 64 workers, aging 5000/500 ms.

History split by time, windows by time (rows per window 147354, 74416, 52603, 44863, 30835, 40792, 92320); metric classes short < 100 ms <= medium < 1000 ms <= long.

## Offered load 0.50

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 0.18 | 26 | 1.4 | 25 | 10.2 | 1.5 | 1.0 / 44 |
| static_history | 0.16 | 25 | 1.3 | 24 | 10.3 | 2.7 | 1.0 / 5 |
| static_random | 0.17 | 25 | 1.3 | 25 | 10.2 | 6.4 | 1.0 / 29 |
| predictive_cold | 0.16 | 25 | 1.3 | 24 | 10.3 | 3.5 | 1.0 / 8 |
| predictive_warm | 0.16 | 25 | 1.3 | 24 | 10.3 | 2.8 | 1.0 / 5 |
| oracle | 0.17 | 25 | 1.5 | 24 | 10.3 | 1.9 | 1.0 / 9 |
| predictive_warm+weights+pausing | 0.16 | 25 | 1.3 | 24 | 10.3 | 2.6 | 1.0 / 7 |
| oracle+weights+pausing | 0.17 | 25 | 1.5 | 24 | 10.3 | 1.9 | 1.0 / 9 |

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -4.8% | [-13.8%, -0.1%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 0.98× | [0.95, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.12× | [1.00, 1.37] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -4.3% | [-12.5%, -0.0%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 0.98× | [0.95, 1.00] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.11× | [1.00, 1.34] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -0.7% | [-1.5%, -0.1%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | -0.1% | [-0.3%, +0.1%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -3.7% | [-10.3%, -0.0%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -4.2% | [-12.1%, -0.1%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -3.2% | [-9.1%, -0.1%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -3.2% | [-9.0%, -0.1%] | **fail** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | +0.6% | [+0.0%, +1.8%] | **fail** |

**Gate A_predictive_routing at load 0.50: NOT PASSED.**
**Gate B_overload_design at load 0.50: NOT PASSED.**

Without the extreme window (window 4, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -5.5% | [-16.0%, -0.1%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 0.98× | [0.94, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.14× | [1.00, 1.43] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -5.1% | [-14.6%, -0.0%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 0.98× | [0.94, 1.00] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.13× | [1.00, 1.39] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -0.8% | [-1.7%, -0.1%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | -0.1% | [-0.4%, +0.1%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -4.3% | [-12.0%, -0.0%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -4.9% | [-14.1%, -0.1%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -3.7% | [-10.5%, -0.1%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -3.7% | [-10.5%, -0.1%] | **fail** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | +0.7% | [+0.0%, +2.1%] | **fail** |

## Offered load 0.80

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 17.13 | 8090 | 58.9 | 8035 | 30.4 | 59.7 | 301.9 / 51341 |
| static_history | 13.98 | 3077 | 54.1 | 2803 | 31.1 | 60.3 | 79.0 / 42396 |
| static_random | 16.01 | 6950 | 58.1 | 6881 | 27.9 | 63.7 | 244.9 / 44629 |
| predictive_cold | 14.82 | 4507 | 55.7 | 4378 | 31.6 | 61.4 | 142.4 / 43595 |
| predictive_warm | 14.23 | 4322 | 54.8 | 4029 | 31.1 | 60.8 | 128.6 / 42593 |
| oracle | 14.24 | 3500 | 58.0 | 3072 | 30.9 | 59.8 | 75.1 / 42040 |
| predictive_warm+weights+pausing | 1.77 | 64 | 55.7 | 54 | 35.5 | 71.0 | 2.9 / 426 |
| oracle+weights+pausing | 6.56 | 33 | 59.4 | 31 | 32.6 | 62.1 | 1.6 / 1696 |

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -15.0% | [-27.0%, -4.2%] | **pass** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 0.97× | [0.93, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.03× | [1.01, 1.06] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -46.5% | [-78.3%, -13.3%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.05× | [0.94, 1.16] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.16× | [1.05, 1.28] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -30.5% | [-60.0%, -5.2%] | **pass** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +10.6% | [+0.1%, +30.5%] | **fail** |
| S3_beats_static_random | predictive_warm vs static_random | -8.6% | [-15.4%, -2.2%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -14.0% | [-26.1%, -3.2%] | **pass** |
| S5_oracle_vs_fifo | oracle vs fifo | -17.5% | [-33.0%, -3.9%] | **pass** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -37.8% | [-62.5%, -11.8%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -40.8% | [-74.9%, -12.1%] | **pass** |

**Gate A_predictive_routing at load 0.80: PASSED.**
**Gate B_overload_design at load 0.80: PASSED.**

Without the extreme window (window 1, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -15.8% | [-28.9%, -3.2%] | **pass** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 0.97× | [0.92, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.03× | [1.00, 1.07] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -38.8% | [-71.0%, -8.6%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.04× | [0.92, 1.17] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.16× | [1.03, 1.29] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -33.4% | [-66.3%, -3.9%] | **pass** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +12.2% | [+0.0%, +35.5%] | **fail** |
| S3_beats_static_random | predictive_warm vs static_random | -9.2% | [-17.1%, -2.5%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -14.9% | [-28.3%, -2.2%] | **pass** |
| S5_oracle_vs_fifo | oracle vs fifo | -18.7% | [-36.7%, -2.9%] | **pass** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -33.0% | [-60.6%, -9.4%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -32.3% | [-62.5%, -3.3%] | **pass** |

## Offered load 0.95

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 40.77 | 30434 | 98.3 | 30319 | 54.7 | 99.9 | 1398.5 / 93498 |
| static_history | 35.97 | 24934 | 93.5 | 24176 | 55.7 | 101.0 | 1062.3 / 83734 |
| static_random | 38.71 | 28436 | 96.7 | 28360 | 51.6 | 102.9 | 1298.7 / 86526 |
| predictive_cold | 37.64 | 28288 | 94.4 | 28176 | 56.3 | 102.6 | 1214.8 / 84674 |
| predictive_warm | 36.79 | 26914 | 93.2 | 26788 | 55.7 | 101.3 | 1150.8 / 83845 |
| oracle | 36.21 | 26213 | 95.8 | 25791 | 55.4 | 100.4 | 1021.5 / 83560 |
| predictive_warm+weights+pausing | 7.03 | 122 | 101.7 | 108 | 64.2 | 109.5 | 6.0 / 2628 |
| oracle+weights+pausing | 13.60 | 38 | 101.0 | 36 | 59.2 | 104.6 | 2.3 / 2941 |

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -7.9% | [-11.2%, -4.5%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 0.98× | [0.96, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.01× | [1.01, 1.02] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -49.9% | [-77.4%, -21.1%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.06× | [1.02, 1.12] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.08× | [1.02, 1.16] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -10.8% | [-15.5%, -6.3%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +6.4% | [+1.2%, +14.2%] | **fail** |
| S3_beats_static_random | predictive_warm vs static_random | -1.1% | [-5.1%, +2.1%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -6.8% | [-9.5%, -3.8%] | **pass** |
| S5_oracle_vs_fifo | oracle vs fifo | -11.8% | [-19.0%, -5.6%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -50.0% | [-71.0%, -26.4%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -46.6% | [-75.4%, -16.7%] | **pass** |

**Gate A_predictive_routing at load 0.95: NOT PASSED.**
**Gate B_overload_design at load 0.95: PASSED.**

Without the extreme window (window 1, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -8.2% | [-11.9%, -4.1%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 0.98× | [0.95, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.01× | [1.00, 1.02] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -42.8% | [-71.3%, -14.9%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.06× | [1.01, 1.13] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.08× | [1.01, 1.17] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -11.6% | [-16.8%, -6.4%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +7.4% | [+1.4%, +16.3%] | **fail** |
| S3_beats_static_random | predictive_warm vs static_random | -0.8% | [-5.6%, +2.8%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -7.1% | [-10.2%, -3.7%] | **pass** |
| S5_oracle_vs_fifo | oracle vs fifo | -12.7% | [-20.8%, -5.6%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -47.0% | [-71.9%, -21.5%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -39.0% | [-68.8%, -10.8%] | **pass** |

