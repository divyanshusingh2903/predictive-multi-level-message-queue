# Huawei Cloud 2025 Region 5 trace simulation (v4-sensitivity-1-w28)

483183 simulated invocations after 2959896 history invocations; tier boundaries (history quantiles) 23 / 31 ms; 64 workers, aging 5000/500 ms.

History split by time, windows by time (rows per window 37233, 42325, 32876, 34920, 31665, 15006, 16768, 10977, 14694, 18254, 10050, 9605, 9689, 10074, 13089, 12011, 10905, 6670, 6383, 6877, 8173, 14000, 11447, 7172, 8645, 16485, 27467, 39723); metric classes short < 100 ms <= medium < 1000 ms <= long.

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
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -2.5% | [-6.8%, -0.2%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.01] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.03× | [1.00, 1.10] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -2.3% | [-6.2%, -0.2%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.00× | [1.00, 1.01] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.03× | [1.00, 1.09] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -0.7% | [-1.4%, -0.2%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | -0.1% | [-0.3%, +0.1%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -2.2% | [-5.8%, -0.2%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -2.2% | [-6.0%, -0.2%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -1.7% | [-4.5%, -0.2%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -1.7% | [-4.5%, -0.2%] | **fail** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | +0.5% | [+0.0%, +1.4%] | **fail** |

**Gate A_predictive_routing at load 0.50: NOT PASSED.**
**Gate B_overload_design at load 0.50: NOT PASSED.**

Without the extreme window (window 0, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -0.4% | [-0.8%, -0.1%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.01] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -0.4% | [-0.8%, -0.1%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -0.4% | [-0.8%, -0.1%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +0.0% | [+0.0%, +0.1%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -0.6% | [-1.2%, -0.1%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -0.4% | [-0.8%, -0.1%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -0.4% | [-0.8%, -0.1%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -0.4% | [-0.8%, -0.1%] | **fail** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | +0.0% | [+0.0%, +0.1%] | **fail** |

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
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -11.6% | [-18.2%, -6.0%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.02× | [0.99, 1.06] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.04× | [1.01, 1.09] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -36.8% | [-53.1%, -21.1%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.09× | [1.04, 1.15] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.13× | [1.07, 1.21] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -15.1% | [-23.2%, -8.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +8.7% | [+0.4%, +18.8%] | **fail** |
| S3_beats_static_random | predictive_warm vs static_random | -8.3% | [-13.3%, -4.0%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -10.4% | [-16.4%, -5.2%] | **pass** |
| S5_oracle_vs_fifo | oracle vs fifo | -13.3% | [-20.7%, -6.8%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -29.3% | [-41.6%, -17.5%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -32.8% | [-48.8%, -17.8%] | **pass** |

**Gate A_predictive_routing at load 0.80: NOT PASSED.**
**Gate B_overload_design at load 0.80: PASSED.**

Without the extreme window (window 4, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -11.7% | [-18.5%, -5.8%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.02× | [0.99, 1.06] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.04× | [1.01, 1.09] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -34.7% | [-51.5%, -19.3%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.09× | [1.04, 1.15] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.13× | [1.06, 1.21] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -15.3% | [-23.8%, -8.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +8.9% | [+0.3%, +19.5%] | **fail** |
| S3_beats_static_random | predictive_warm vs static_random | -8.5% | [-13.6%, -4.0%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -10.5% | [-16.8%, -5.1%] | **pass** |
| S5_oracle_vs_fifo | oracle vs fifo | -13.5% | [-21.2%, -6.6%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -28.1% | [-40.8%, -16.1%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -30.7% | [-46.7%, -16.0%] | **pass** |

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
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -8.6% | [-12.8%, -5.1%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [0.99, 1.03] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.02× | [1.01, 1.05] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -46.0% | [-61.3%, -30.5%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.12× | [1.06, 1.20] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.13× | [1.06, 1.22] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -13.6% | [-21.0%, -7.3%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +17.2% | [+2.6%, +42.2%] | **fail** |
| S3_beats_static_random | predictive_warm vs static_random | -1.9% | [-6.0%, +1.7%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -7.2% | [-10.4%, -4.5%] | **pass** |
| S5_oracle_vs_fifo | oracle vs fifo | -13.0% | [-18.8%, -7.9%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -46.6% | [-58.4%, -34.1%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -42.5% | [-58.1%, -27.1%] | **pass** |

**Gate A_predictive_routing at load 0.95: NOT PASSED.**
**Gate B_overload_design at load 0.95: PASSED.**

Without the extreme window (window 5, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -8.7% | [-13.0%, -5.1%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.01× | [0.99, 1.03] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.02× | [1.01, 1.05] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -44.3% | [-59.7%, -28.8%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.13× | [1.06, 1.20] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.14× | [1.06, 1.23] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -13.9% | [-21.7%, -7.4%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +17.8% | [+2.8%, +43.5%] | **fail** |
| S3_beats_static_random | predictive_warm vs static_random | -1.9% | [-6.1%, +1.8%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -7.3% | [-10.5%, -4.5%] | **pass** |
| S5_oracle_vs_fifo | oracle vs fifo | -13.3% | [-19.4%, -7.9%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -45.6% | [-57.7%, -33.0%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -40.7% | [-56.4%, -25.5%] | **pass** |

