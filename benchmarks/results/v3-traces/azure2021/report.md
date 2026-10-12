# Azure Functions 2021 trace simulation (v3-traces rules) (v3-traces-1)

936382 simulated invocations after 1044569 history invocations; tier boundaries (history terciles) 2 / 198 ms; 64 workers, aging 5000/500 ms.

History split by time, windows by time (rows per window 78845, 208403, 132990, 153642, 133158, 135356, 93988); metric classes short < 100 ms <= medium < 1000 ms <= long.

## Offered load 0.50

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 4239.17 | 78 | 35090.7 | 4 | 10797.6 | 35859.3 | 1.0 / 32775100 |
| static_history | 4239.19 | 78 | 35091.7 | 4 | 10798.0 | 35861.8 | 1.0 / 32773400 |
| static_random | 4239.24 | 78 | 35091.0 | 4 | 10797.8 | 35859.0 | 1.0 / 32775100 |
| predictive_cold | 4239.54 | 78 | 35089.4 | 4 | 10801.3 | 35863.0 | 1.0 / 32776000 |
| predictive_warm | 4239.54 | 78 | 35089.4 | 4 | 10801.3 | 35863.0 | 1.0 / 32776000 |
| oracle | 4238.36 | 78 | 35089.7 | 4 | 10797.6 | 35859.3 | 1.0 / 32768300 |
| predictive_warm+weights+pausing | 3758.22 | 78 | 35965.8 | 4 | 10602.9 | 40521.2 | 1.0 / 29109900 |
| oracle+weights+pausing | 2076.35 | 78 | 34746.0 | 3 | 10805.9 | 35883.3 | 1.0 / 59584 |

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -0.7% | [-1.5%, -0.1%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [0.99, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -15.4% | [-25.8%, -5.4%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.00× | [0.95, 1.05] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.08× | [1.03, 1.13] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -0.0% | [-0.0%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | -0.5% | [-1.2%, -0.0%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -1.0% | [-2.0%, -0.2%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -0.7% | [-1.5%, -0.1%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -1.2% | [-2.0%, -0.4%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -26.5% | [-43.8%, -12.0%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -14.8% | [-25.1%, -5.0%] | **pass** |

**Gate A_predictive_routing at load 0.50: NOT PASSED.**
**Gate B_overload_design at load 0.50: PASSED.**

Without the extreme window (window 6, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -0.9% | [-1.7%, -0.2%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [0.99, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -15.1% | [-27.3%, -3.5%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 0.99× | [0.94, 1.05] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.07× | [1.02, 1.13] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | +0.0% | [+0.0%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | -0.6% | [-1.4%, -0.1%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -1.1% | [-2.3%, -0.2%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -0.9% | [-1.7%, -0.2%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -1.4% | [-2.3%, -0.6%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -19.1% | [-30.2%, -9.6%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -14.4% | [-26.8%, -3.1%] | **pass** |

## Offered load 0.80

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 4660.99 | 93 | 36044.3 | 9 | 11160.7 | 36941.8 | 1.0 / 35393800 |
| static_history | 4660.97 | 93 | 36043.1 | 9 | 11161.3 | 36942.1 | 1.0 / 35392300 |
| static_random | 4661.14 | 93 | 36045.7 | 9 | 11160.9 | 36940.9 | 1.0 / 35393300 |
| predictive_cold | 4661.24 | 93 | 36042.7 | 9 | 11165.5 | 36942.2 | 1.0 / 35394300 |
| predictive_warm | 4661.23 | 93 | 36042.7 | 9 | 11165.5 | 36942.2 | 1.0 / 35394300 |
| oracle | 4659.60 | 93 | 36038.9 | 9 | 11160.7 | 36941.6 | 1.0 / 35389200 |
| predictive_warm+weights+pausing | 4300.75 | 93 | 38194.4 | 8 | 11036.5 | 39672.7 | 1.0 / 32022700 |
| oracle+weights+pausing | 2200.27 | 86 | 35608.9 | 8 | 11169.4 | 36961.0 | 1.0 / 76146 |

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -0.9% | [-2.0%, -0.1%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -22.8% | [-37.2%, -9.3%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.06× | [1.00, 1.14] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.11× | [1.03, 1.23] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -2.0% | [-6.1%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | -0.1% | [-0.7%, +0.6%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -1.1% | [-2.3%, -0.2%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -0.9% | [-2.0%, -0.1%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -1.6% | [-3.3%, -0.5%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -33.3% | [-49.0%, -18.5%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -22.1% | [-36.7%, -8.8%] | **pass** |

**Gate A_predictive_routing at load 0.80: NOT PASSED.**
**Gate B_overload_design at load 0.80: PASSED.**

Without the extreme window (window 6, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -1.0% | [-2.3%, -0.2%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -24.9% | [-40.8%, -9.1%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.06× | [0.99, 1.15] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.12× | [1.03, 1.26] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -2.4% | [-7.1%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | -0.1% | [-0.8%, +0.7%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -1.2% | [-2.7%, -0.2%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -1.0% | [-2.3%, -0.2%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -1.9% | [-3.7%, -0.7%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -27.1% | [-39.5%, -14.8%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -24.1% | [-40.1%, -8.5%] | **pass** |

## Offered load 0.95

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 4776.67 | 109 | 36437.7 | 15 | 11260.2 | 37277.7 | 1.0 / 35999900 |
| static_history | 4776.57 | 109 | 36437.0 | 15 | 11260.9 | 37278.6 | 1.0 / 35994800 |
| static_random | 4776.88 | 109 | 36437.9 | 15 | 11260.5 | 37276.6 | 1.0 / 36001500 |
| predictive_cold | 4776.60 | 109 | 36437.5 | 15 | 11264.5 | 37278.5 | 1.0 / 35989600 |
| predictive_warm | 4776.60 | 109 | 36437.5 | 15 | 11264.5 | 37278.5 | 1.0 / 35989600 |
| oracle | 4774.92 | 109 | 36434.7 | 15 | 11260.2 | 37277.7 | 1.0 / 35990000 |
| predictive_warm+weights+pausing | 4356.87 | 94 | 38480.7 | 13 | 11104.7 | 39714.0 | 1.0 / 33028900 |
| oracle+weights+pausing | 2234.41 | 93 | 36057.9 | 13 | 11269.0 | 37298.3 | 1.0 / 79877 |

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -1.2% | [-2.8%, -0.2%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.01] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -25.3% | [-40.3%, -10.6%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.10× | [1.01, 1.19] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.14× | [1.03, 1.27] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -2.0% | [-6.1%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | -0.1% | [-0.8%, +0.6%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -1.5% | [-3.3%, -0.3%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -1.2% | [-2.8%, -0.2%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -1.8% | [-3.9%, -0.5%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -36.7% | [-52.6%, -20.8%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -24.4% | [-39.2%, -10.1%] | **pass** |

**Gate A_predictive_routing at load 0.95: NOT PASSED.**
**Gate B_overload_design at load 0.95: PASSED.**

Without the extreme window (window 6, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -1.4% | [-3.2%, -0.3%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [0.99, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.01] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -27.6% | [-44.1%, -10.6%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.10× | [1.01, 1.21] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.15× | [1.03, 1.30] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -2.4% | [-7.1%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | -0.1% | [-0.9%, +0.7%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -1.7% | [-3.7%, -0.3%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -1.4% | [-3.2%, -0.3%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -2.2% | [-4.4%, -0.7%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -31.0% | [-45.5%, -16.5%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -26.5% | [-42.8%, -9.9%] | **pass** |

