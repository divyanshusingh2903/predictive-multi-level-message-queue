# Azure Functions 2021 trace simulation (v3-traces rules) (v4-sensitivity-1-w28)

936382 simulated invocations after 1044569 history invocations; tier boundaries (history quantiles) 2 / 198 ms; 64 workers, aging 5000/500 ms.

History split by time, windows by time (rows per window 18565, 11354, 26213, 22713, 13467, 20240, 60069, 114627, 49461, 10463, 23322, 49744, 51298, 7807, 48193, 46344, 16199, 22272, 69363, 25324, 14453, 27501, 30098, 63304, 20819, 25222, 25233, 22714); metric classes short < 100 ms <= medium < 1000 ms <= long.

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
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -1.0% | [-1.9%, -0.2%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.03× | [1.00, 1.07] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -8.4% | [-13.0%, -4.3%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.00× | [0.98, 1.02] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.06× | [1.03, 1.11] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -0.8% | [-1.9%, -0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | -0.2% | [-0.6%, +0.2%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -1.3% | [-2.6%, -0.4%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -1.0% | [-1.9%, -0.2%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -1.6% | [-2.5%, -0.8%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -17.5% | [-26.9%, -8.9%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -7.6% | [-11.9%, -3.8%] | **pass** |

**Gate A_predictive_routing at load 0.50: NOT PASSED.**
**Gate B_overload_design at load 0.50: NOT PASSED.**

Without the extreme window (window 24, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -1.0% | [-1.9%, -0.3%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.03× | [1.00, 1.07] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -8.4% | [-13.1%, -4.1%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.00× | [0.98, 1.02] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.06× | [1.03, 1.11] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -0.8% | [-2.2%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | -0.2% | [-0.6%, +0.2%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -1.4% | [-2.7%, -0.4%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -1.0% | [-1.9%, -0.3%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -1.6% | [-2.6%, -0.8%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -15.8% | [-25.1%, -7.8%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -7.6% | [-12.1%, -3.5%] | **pass** |

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
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -1.6% | [-2.8%, -0.6%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [0.99, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.01× | [1.00, 1.02] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -15.8% | [-23.9%, -8.5%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.00× | [0.96, 1.04] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.05× | [1.02, 1.09] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -0.1% | [-0.2%, -0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | -0.1% | [-1.0%, +0.8%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -2.1% | [-3.6%, -0.8%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -1.6% | [-2.8%, -0.6%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -2.4% | [-3.8%, -1.2%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -24.0% | [-33.6%, -14.8%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -14.7% | [-22.5%, -7.7%] | **pass** |

**Gate A_predictive_routing at load 0.80: NOT PASSED.**
**Gate B_overload_design at load 0.80: PASSED.**

Without the extreme window (window 24, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -1.7% | [-2.9%, -0.7%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [0.99, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.01× | [1.00, 1.02] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -16.2% | [-24.5%, -8.6%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.00× | [0.95, 1.04] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.05× | [1.02, 1.09] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -0.1% | [-0.2%, -0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | -0.2% | [-1.0%, +0.8%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -2.1% | [-3.7%, -0.8%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -1.7% | [-2.8%, -0.7%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -2.4% | [-3.9%, -1.2%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -22.6% | [-32.0%, -13.6%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -15.0% | [-23.2%, -7.7%] | **pass** |

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
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -2.3% | [-3.7%, -1.1%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 0.99× | [0.98, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.03× | [1.00, 1.06] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -17.9% | [-26.3%, -10.2%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.01× | [0.98, 1.06] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.08× | [1.04, 1.12] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -2.7% | [-6.3%, -0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +0.2% | [-0.7%, +1.1%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -2.9% | [-4.7%, -1.3%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -2.3% | [-3.7%, -1.1%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -3.3% | [-5.3%, -1.7%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -27.3% | [-37.1%, -17.7%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -16.1% | [-24.4%, -8.6%] | **pass** |

**Gate A_predictive_routing at load 0.95: NOT PASSED.**
**Gate B_overload_design at load 0.95: PASSED.**

Without the extreme window (window 24, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -2.4% | [-3.9%, -1.1%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 0.99× | [0.98, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.03× | [1.00, 1.06] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -17.8% | [-26.5%, -9.8%] | **pass** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.01× | [0.97, 1.05] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.08× | [1.04, 1.13] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -2.8% | [-6.5%, -0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +0.2% | [-0.7%, +1.1%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -3.0% | [-4.9%, -1.4%] | **pass** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -2.4% | [-3.9%, -1.1%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -3.4% | [-5.4%, -1.7%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -25.9% | [-35.7%, -16.5%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | -15.9% | [-24.6%, -8.2%] | **pass** |

