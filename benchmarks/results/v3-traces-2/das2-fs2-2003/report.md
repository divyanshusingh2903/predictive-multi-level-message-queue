# Parallel Workloads Archive DAS2-fs2-2003 trace simulation (v3-traces-2)

50599 simulated invocations after 14061 history invocations; tier boundaries (history terciles) 66000 / 310000 ms; 64 workers, aging 5000/500 ms.

History split by time, windows by time (rows per window 5325, 3477, 4241, 2838, 9862, 11729, 13127); metric classes short < 60000 ms <= medium < 600000 ms <= long.

## Offered load 0.50

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 1372.64 | 62000 | 16811.4 | 54000 | 12664.0 | 18812.7 | 1.0 / 130 |
| static_history | 1372.35 | 62000 | 16808.6 | 54000 | 12663.4 | 18784.3 | 1.0 / 130 |
| static_random | 1371.96 | 62000 | 16801.0 | 54000 | 12662.5 | 18719.1 | 1.0 / 130 |
| predictive_cold | 1372.62 | 62000 | 16815.4 | 54000 | 12664.1 | 18815.4 | 1.0 / 130 |
| predictive_warm | 1372.62 | 62000 | 16815.4 | 54000 | 12664.1 | 18815.4 | 1.0 / 130 |
| oracle | 1372.52 | 62000 | 16810.3 | 54000 | 12664.0 | 18815.4 | 1.0 / 130 |
| predictive_warm+weights+pausing | 1482.84 | 62000 | 33987.1 | 54000 | 13377.4 | 36681.9 | 1.0 / 133 |
| oracle+weights+pausing | 1126.22 | 62000 | 17021.8 | 54000 | 12869.8 | 21602.1 | 1.0 / 42 |

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -0.0% | [-0.0%, -0.0%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | +2.7% | [-0.0%, +6.8%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.26× | [1.00, 1.54] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -0.3% | [-0.8%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +0.0% | [-0.0%, +0.0%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | +0.0% | [-0.0%, +0.0%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -0.0% | [-0.0%, +0.0%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -0.0% | [-0.0%, -0.0%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -6.0% | [-15.1%, +0.0%] | **fail** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | +2.7% | [-0.0%, +6.8%] | **fail** |

**Gate A_predictive_routing at load 0.50: NOT PASSED.**
**Gate B_overload_design at load 0.50: NOT PASSED.**

Without the extreme window (window 2, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -0.0% | [-0.0%, -0.0%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | +0.7% | [-0.0%, +2.0%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.14× | [1.00, 1.42] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | +0.0% | [+0.0%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | -0.0% | [-0.0%, +0.0%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -0.0% | [-0.0%, +0.0%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -0.0% | [-0.0%, -0.0%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -0.0% | [-0.0%, -0.0%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -1.7% | [-5.0%, +0.0%] | **fail** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | +0.7% | [-0.0%, +2.0%] | **fail** |

## Offered load 0.80

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 13738.50 | 223379 | 90208.3 | 65647 | 44800.7 | 90326.3 | 2.6 / 4507 |
| static_history | 13738.70 | 223212 | 90212.1 | 65837 | 44801.5 | 90319.1 | 2.5 / 4507 |
| static_random | 13739.10 | 223450 | 90204.6 | 65467 | 44801.0 | 90313.6 | 2.5 / 4506 |
| predictive_cold | 13738.70 | 223386 | 90206.9 | 65636 | 44801.3 | 90325.0 | 2.5 / 4507 |
| predictive_warm | 13738.80 | 223386 | 90207.0 | 65636 | 44801.3 | 90325.9 | 2.5 / 4507 |
| oracle | 13738.20 | 222655 | 90204.4 | 65409 | 44801.0 | 90326.9 | 2.5 / 4506 |
| predictive_warm+weights+pausing | 19819.50 | 220011 | 217158.0 | 62222 | 82108.4 | 221540.0 | 2.6 / 2846 |
| oracle+weights+pausing | 6235.96 | 180108 | 96563.6 | 66860 | 47256.0 | 99083.0 | 2.2 / 233 |

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -0.0% | [-0.0%, +0.0%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | +88.8% | [-7.9%, +213.1%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 2.61× | [1.20, 5.06] | **fail** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 5.32× | [1.87, 10.99] | **fail** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -0.1% | [-0.2%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | -0.0% | [-0.0%, +0.0%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -0.0% | [-0.0%, +0.0%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -0.0% | [-0.0%, +0.0%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -0.1% | [-0.1%, +0.0%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -30.1% | [-48.9%, -9.6%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | +88.9% | [-7.9%, +213.3%] | **fail** |

**Gate A_predictive_routing at load 0.80: NOT PASSED.**
**Gate B_overload_design at load 0.80: NOT PASSED.**

Without the extreme window (window 2, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -0.0% | [-0.0%, +0.0%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | +87.5% | [-20.8%, +231.6%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 2.70× | [1.09, 5.55] | **fail** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 5.80× | [1.77, 12.37] | **fail** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -0.1% | [-0.2%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | -0.0% | [-0.0%, +0.0%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -0.0% | [-0.1%, +0.0%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -0.0% | [-0.0%, +0.0%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -0.1% | [-0.1%, +0.0%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -25.7% | [-46.5%, -7.1%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | +87.6% | [-20.8%, +231.8%] | **fail** |

## Offered load 0.95

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 34973.60 | 26964700 | 113953.0 | 354981 | 62949.8 | 114178.0 | 206.7 / 22322 |
| static_history | 34974.10 | 26964700 | 113954.0 | 354819 | 62950.6 | 114183.0 | 206.7 / 22322 |
| static_random | 34974.80 | 26967700 | 113954.0 | 354664 | 62950.7 | 114185.0 | 206.8 / 22322 |
| predictive_cold | 34974.40 | 26965400 | 113949.0 | 354995 | 62950.4 | 114178.0 | 206.7 / 22322 |
| predictive_warm | 34974.20 | 26964900 | 113950.0 | 354995 | 62950.3 | 114179.0 | 206.7 / 22322 |
| oracle | 34971.70 | 26962700 | 113952.0 | 354621 | 62949.9 | 114180.0 | 206.6 / 22322 |
| predictive_warm+weights+pausing | 40975.40 | 1428530 | 297414.0 | 285515 | 128604.0 | 299879.0 | 21.2 / 9131 |
| oracle+weights+pausing | 7728.26 | 429850 | 118560.0 | 226848 | 67626.4 | 121420.0 | 6.2 / 325 |

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | +0.0% | [+0.0%, +0.0%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | +72.2% | [-6.6%, +155.6%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 4.57× | [1.21, 10.68] | **fail** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 3.74× | [1.99, 6.28] | **fail** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -0.0% | [-0.0%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +0.0% | [-0.0%, +0.0%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | +0.0% | [-0.0%, +0.0%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | +0.0% | [+0.0%, +0.0%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -0.0% | [-0.0%, -0.0%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -49.2% | [-75.3%, -22.1%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | +72.2% | [-6.6%, +155.6%] | **fail** |

**Gate A_predictive_routing at load 0.95: NOT PASSED.**
**Gate B_overload_design at load 0.95: NOT PASSED.**

Without the extreme window (window 3, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | +0.0% | [+0.0%, +0.0%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | +82.5% | [-6.8%, +177.2%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 5.02× | [1.11, 12.16] | **fail** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 3.93× | [1.88, 6.78] | **fail** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -0.0% | [-0.0%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +0.0% | [+0.0%, +0.0%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | +0.0% | [-0.0%, +0.0%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | +0.0% | [+0.0%, +0.0%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -0.0% | [-0.1%, -0.0%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -48.5% | [-78.2%, -17.0%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | +82.5% | [-6.8%, +177.2%] | **fail** |

