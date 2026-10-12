# Parallel Workloads Archive LPC-EGEE-2004 trace simulation (v3-traces-2)

117823 simulated invocations after 96499 history invocations; tier boundaries (history terciles) 7000 / 41000 ms; 64 workers, aging 5000/500 ms.

History split by time, windows by time (rows per window 21413, 13193, 14362, 8973, 27846, 13218, 18818); metric classes short < 60000 ms <= medium < 600000 ms <= long.

## Offered load 0.50

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 26154.90 | 210442 | 278730.0 | 13000 | 45797.2 | 337550.0 | 1.0 / 63190 |
| static_history | 26154.20 | 210000 | 278717.0 | 13000 | 45797.1 | 337573.0 | 1.0 / 63166 |
| static_random | 26155.00 | 210884 | 278716.0 | 13000 | 45799.1 | 337578.0 | 1.0 / 63191 |
| predictive_cold | 26155.40 | 210489 | 278725.0 | 13000 | 45797.2 | 337549.0 | 1.0 / 63190 |
| predictive_warm | 26155.40 | 210493 | 278725.0 | 13000 | 45797.2 | 337549.0 | 1.0 / 63190 |
| oracle | 26150.90 | 210000 | 278719.0 | 13000 | 45797.2 | 337549.0 | 1.0 / 63190 |
| predictive_warm+weights+pausing | 26428.30 | 193000 | 278230.0 | 13000 | 45842.9 | 338116.0 | 1.0 / 64591 |
| oracle+weights+pausing | 15604.10 | 107558 | 211690.0 | 13000 | 45812.4 | 338629.0 | 1.0 / 9046 |

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | +0.0% | [+0.0%, +0.0%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | +1.0% | [-0.7%, +2.8%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 0.99× | [0.98, 1.00] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.01× | [1.00, 1.01] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | +0.0% | [+0.0%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +0.0% | [-0.0%, +0.0%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -0.0% | [-0.0%, +0.0%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | +0.0% | [+0.0%, +0.0%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -0.0% | [-0.0%, -0.0%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -33.1% | [-47.2%, -22.7%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | +1.0% | [-0.7%, +2.8%] | **fail** |

**Gate A_predictive_routing at load 0.50: NOT PASSED.**
**Gate B_overload_design at load 0.50: NOT PASSED.**

Without the extreme window (window 5, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | +0.0% | [+0.0%, +0.0%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | +0.9% | [-1.1%, +3.0%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 0.99× | [0.97, 1.00] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.01× | [1.00, 1.01] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | +0.0% | [+0.0%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +0.0% | [-0.0%, +0.0%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -0.0% | [-0.0%, +0.0%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | +0.0% | [+0.0%, +0.0%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -0.0% | [-0.0%, -0.0%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -33.2% | [-49.3%, -21.2%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | +0.9% | [-1.1%, +2.9%] | **fail** |

## Offered load 0.80

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 153489.00 | 106612000 | 773470.0 | 80701500 | 205225.0 | 794623.0 | 452.1 / 185534 |
| static_history | 153485.00 | 106605000 | 773469.0 | 80669600 | 205224.0 | 794575.0 | 452.3 / 185534 |
| static_random | 153486.00 | 106592000 | 773460.0 | 80699900 | 205226.0 | 794598.0 | 452.0 / 185540 |
| predictive_cold | 153488.00 | 106647000 | 773466.0 | 80707700 | 205225.0 | 794625.0 | 452.0 / 185533 |
| predictive_warm | 153488.00 | 106647000 | 773466.0 | 80707700 | 205225.0 | 794625.0 | 452.0 / 185533 |
| oracle | 153484.00 | 106612000 | 773469.0 | 80701200 | 205225.0 | 794629.0 | 452.2 / 185535 |
| predictive_warm+weights+pausing | 161012.00 | 94008800 | 774333.0 | 73419700 | 206143.0 | 798085.0 | 347.4 / 200213 |
| oracle+weights+pausing | 71069.20 | 1682000 | 723146.0 | 386550 | 205564.0 | 795019.0 | 48.2 / 10246 |

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -0.0% | [-0.0%, +0.0%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | +1.9% | [-3.8%, +7.2%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.05× | [1.00, 1.14] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.05× | [0.99, 1.14] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | +0.0% | [+0.0%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +0.0% | [-0.0%, +0.0%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -0.0% | [-0.0%, +0.0%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -0.0% | [-0.0%, +0.0%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -0.0% | [-0.0%, -0.0%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -52.1% | [-59.1%, -45.2%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | +1.9% | [-3.8%, +7.2%] | **fail** |

**Gate A_predictive_routing at load 0.80: NOT PASSED.**
**Gate B_overload_design at load 0.80: NOT PASSED.**

Without the extreme window (window 6, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -0.0% | [-0.0%, +0.0%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | +2.2% | [-4.8%, +8.1%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.06× | [1.00, 1.16] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.06× | [0.99, 1.16] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | +0.0% | [+0.0%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +0.0% | [-0.0%, +0.0%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -0.0% | [-0.0%, +0.0%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -0.0% | [-0.0%, +0.0%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -0.0% | [-0.0%, -0.0%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -51.7% | [-60.1%, -43.8%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | +2.2% | [-4.8%, +8.1%] | **fail** |

## Offered load 0.95

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 328587.00 | 187734000 | 1225150.0 | 153768000 | 461388.0 | 1234200.0 | 2495.1 / 408169 |
| static_history | 328582.00 | 187689000 | 1225160.0 | 153773000 | 461386.0 | 1234040.0 | 2495.3 / 408159 |
| static_random | 328585.00 | 187686000 | 1225150.0 | 153834000 | 461389.0 | 1234180.0 | 2495.9 / 408159 |
| predictive_cold | 328585.00 | 187759000 | 1225150.0 | 153769000 | 461387.0 | 1234160.0 | 2495.0 / 408153 |
| predictive_warm | 328585.00 | 187759000 | 1225150.0 | 153769000 | 461387.0 | 1234160.0 | 2494.8 / 408153 |
| oracle | 328581.00 | 187733000 | 1225160.0 | 153761000 | 461388.0 | 1234200.0 | 2495.1 / 408170 |
| predictive_warm+weights+pausing | 334068.00 | 189788000 | 1224750.0 | 149527000 | 473226.0 | 1233890.0 | 1687.9 / 311562 |
| oracle+weights+pausing | 155362.00 | 2967780 | 1165940.0 | 588881 | 461990.0 | 1236000.0 | 85.2 / 15274 |

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -0.0% | [-0.0%, -0.0%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | +8.5% | [-1.1%, +21.8%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.08× | [1.00, 1.19] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.12× | [1.04, 1.21] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -0.0% | [-0.0%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +0.0% | [-0.0%, +0.0%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -0.0% | [-0.0%, +0.0%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -0.0% | [-0.0%, -0.0%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -0.0% | [-0.0%, -0.0%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -53.8% | [-62.3%, -46.1%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | +8.5% | [-1.1%, +21.8%] | **fail** |

**Gate A_predictive_routing at load 0.95: NOT PASSED.**
**Gate B_overload_design at load 0.95: NOT PASSED.**

Without the extreme window (window 6, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| A_P1_mean_latency_vs_fifo | predictive_warm vs fifo | -0.0% | [-0.0%, -0.0%] | **fail** |
| A_G1_all_p99_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| A_G2_long_max_wait_vs_fifo | predictive_warm vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| B_P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | +11.0% | [+0.9%, +25.6%] | **fail** |
| B_G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.09× | [1.00, 1.20] | **pass** |
| B_G2_long_max_wait_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.15× | [1.06, 1.23] | **pass** |
| S1_short_p50_vs_fifo | predictive_warm vs fifo | -0.0% | [-0.0%, +0.0%] | **fail** |
| S2_vs_static_history_noninferior | predictive_warm vs static_history | +0.0% | [-0.0%, +0.0%] | **pass** |
| S3_beats_static_random | predictive_warm vs static_random | -0.0% | [-0.0%, +0.0%] | **fail** |
| S4_cold_start_vs_fifo | predictive_cold vs fifo | -0.0% | [-0.0%, -0.0%] | **fail** |
| S5_oracle_vs_fifo | oracle vs fifo | -0.0% | [-0.0%, -0.0%] | **fail** |
| S6_oracle_weights_pausing_vs_fifo | oracle+weights+pausing vs fifo | -54.3% | [-64.2%, -45.3%] | **pass** |
| S7_weights_pausing_vs_predictive_warm | predictive_warm+weights+pausing vs predictive_warm | +11.0% | [+0.9%, +25.6%] | **fail** |

