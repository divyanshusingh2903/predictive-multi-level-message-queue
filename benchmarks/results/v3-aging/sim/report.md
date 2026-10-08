# Azure Functions trace simulation (v3-aging-1)

990476 simulated invocations after 990475 history invocations; tier boundaries (history terciles) 1 / 171 ms; 64 workers, aging 5000/500 ms.

## Offered load 0.50

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 4111.54 | 92 | 35293.2 | 0 | 6123.8 | 36003.0 | 1.0 / 33298200 |
| predictive_warm | 4111.92 | 84 | 35293.5 | 0 | 6125.6 | 36003.8 | 1.0 / 33297600 |
| predictive_warm+weights+no_aging | 3934.07 | 78 | 35796.5 | 0 | 5987.8 | 36520.6 | 1.0 / 33637200 |
| predictive_warm+weights | 3594.68 | 82 | 36509.0 | 0 | 5112.0 | 37439.6 | 1.0 / 33976400 |
| predictive_warm+weights+pausing | 3715.43 | 78 | 33494.2 | 0 | 5919.3 | 40537.0 | 1.0 / 29880100 |
| oracle+weights+pausing | 2041.00 | 78 | 34916.6 | 0 | 6127.3 | 36021.2 | 1.0 / 65568 |
| predictive_warm+weights_alt+pausing | 3815.08 | 78 | 33913.1 | 0 | 6004.2 | 40523.5 | 1.0 / 31403800 |

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -14.6% | [-25.5%, -5.9%] | **fail** |
| G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.01× | [0.93, 1.08] | **pass** |
| G2_long_class_max_wait | predictive_warm+weights+pausing vs fifo | 1.21× | [1.03, 1.51] | **pass** |
| S1_pausing_vs_plain_aging | predictive_warm+weights+pausing vs predictive_warm+weights | -6.3% | [-11.8%, -1.2%] | **pass** |
| S2_vs_todays_aging | predictive_warm+weights+pausing vs predictive_warm | -13.9% | [-24.9%, -5.3%] | **pass** |
| S3_pausing_cost_vs_no_aging | predictive_warm+weights+pausing vs predictive_warm+weights+no_aging | -5.5% | [-12.4%, -0.7%] | **pass** |
| S4_no_aging_long_max_wait | predictive_warm+weights+no_aging vs fifo | 1.41× | [1.00, 2.21] | **fail** |
| S5_plain_aging_mean_vs_fifo | predictive_warm+weights vs fifo | -9.3% | [-17.9%, -3.3%] | **fail** |
| S6_oracle_mean_vs_fifo | oracle+weights+pausing vs fifo | -24.1% | [-36.3%, -13.1%] | **pass** |
| S7_sensitivity_mean_vs_fifo | predictive_warm+weights_alt+pausing vs fifo | -12.4% | [-23.4%, -4.9%] | **fail** |
| S8_sensitivity_p99_vs_fifo | predictive_warm+weights_alt+pausing vs fifo | 1.01× | [0.94, 1.09] | **pass** |

**Gate at load 0.50: NOT PASSED.**

Without the extreme window (window 6, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -15.4% | [-27.8%, -5.3%] | **pass** |
| G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 0.99× | [0.91, 1.07] | **pass** |
| G2_long_class_max_wait | predictive_warm+weights+pausing vs fifo | 1.22× | [1.02, 1.57] | **pass** |
| S1_pausing_vs_plain_aging | predictive_warm+weights+pausing vs predictive_warm+weights | -7.9% | [-13.1%, -2.8%] | **pass** |
| S2_vs_todays_aging | predictive_warm+weights+pausing vs predictive_warm | -14.6% | [-26.9%, -4.6%] | **pass** |
| S3_pausing_cost_vs_no_aging | predictive_warm+weights+pausing vs predictive_warm+weights+no_aging | -5.5% | [-13.6%, +0.0%] | **pass** |
| S4_no_aging_long_max_wait | predictive_warm+weights+no_aging vs fifo | 1.48× | [1.00, 2.41] | **fail** |
| S5_plain_aging_mean_vs_fifo | predictive_warm+weights vs fifo | -8.7% | [-18.7%, -2.6%] | **fail** |
| S6_oracle_mean_vs_fifo | oracle+weights+pausing vs fifo | -19.7% | [-30.5%, -10.9%] | **pass** |
| S7_sensitivity_mean_vs_fifo | predictive_warm+weights_alt+pausing vs fifo | -13.3% | [-26.1%, -4.5%] | **fail** |
| S8_sensitivity_p99_vs_fifo | predictive_warm+weights_alt+pausing vs fifo | 1.00× | [0.91, 1.09] | **pass** |

## Offered load 0.80

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 4474.67 | 109 | 36202.4 | 0 | 6456.1 | 37152.0 | 1.0 / 35765600 |
| predictive_warm | 4474.73 | 109 | 36201.1 | 0 | 6458.0 | 37151.9 | 1.0 / 35756300 |
| predictive_warm+weights+no_aging | 4299.03 | 93 | 37118.1 | 0 | 6205.3 | 37970.1 | 1.0 / 36335200 |
| predictive_warm+weights | 3926.18 | 109 | 37218.3 | 0 | 5434.1 | 38231.6 | 1.0 / 36388400 |
| predictive_warm+weights+pausing | 4160.51 | 93 | 37794.3 | 0 | 6280.1 | 39725.7 | 1.0 / 32874000 |
| oracle+weights+pausing | 2151.85 | 93 | 35828.0 | 0 | 6459.8 | 37168.9 | 1.0 / 81400 |
| predictive_warm+weights_alt+pausing | 4152.46 | 93 | 35657.3 | 0 | 6298.2 | 39721.1 | 1.0 / 33878600 |

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -23.7% | [-36.5%, -11.7%] | **pass** |
| G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.09× | [0.97, 1.23] | **pass** |
| G2_long_class_max_wait | predictive_warm+weights+pausing vs fifo | 1.23× | [1.00, 1.60] | **pass** |
| S1_pausing_vs_plain_aging | predictive_warm+weights+pausing vs predictive_warm+weights | -16.8% | [-28.1%, -5.3%] | **pass** |
| S2_vs_todays_aging | predictive_warm+weights+pausing vs predictive_warm | -22.6% | [-35.7%, -10.6%] | **pass** |
| S3_pausing_cost_vs_no_aging | predictive_warm+weights+pausing vs predictive_warm+weights+no_aging | -2.5% | [-8.0%, +1.8%] | **pass** |
| S4_no_aging_long_max_wait | predictive_warm+weights+no_aging vs fifo | 1.48× | [1.00, 2.39] | **fail** |
| S5_plain_aging_mean_vs_fifo | predictive_warm+weights vs fifo | -8.5% | [-16.2%, -2.7%] | **fail** |
| S6_oracle_mean_vs_fifo | oracle+weights+pausing vs fifo | -34.2% | [-45.2%, -22.5%] | **pass** |
| S7_sensitivity_mean_vs_fifo | predictive_warm+weights_alt+pausing vs fifo | -23.1% | [-35.7%, -11.3%] | **pass** |
| S8_sensitivity_p99_vs_fifo | predictive_warm+weights_alt+pausing vs fifo | 1.09× | [0.97, 1.23] | **pass** |

**Gate at load 0.80: PASSED.**

Without the extreme window (window 6, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -26.5% | [-38.9%, -13.2%] | **pass** |
| G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.10× | [0.96, 1.25] | **pass** |
| G2_long_class_max_wait | predictive_warm+weights+pausing vs fifo | 1.26× | [0.99, 1.69] | **pass** |
| S1_pausing_vs_plain_aging | predictive_warm+weights+pausing vs predictive_warm+weights | -20.6% | [-31.0%, -10.2%] | **pass** |
| S2_vs_todays_aging | predictive_warm+weights+pausing vs predictive_warm | -25.2% | [-37.9%, -11.9%] | **pass** |
| S3_pausing_cost_vs_no_aging | predictive_warm+weights+pausing vs predictive_warm+weights+no_aging | -2.4% | [-8.7%, +2.6%] | **pass** |
| S4_no_aging_long_max_wait | predictive_warm+weights+no_aging vs fifo | 1.55× | [1.00, 2.61] | **fail** |
| S5_plain_aging_mean_vs_fifo | predictive_warm+weights vs fifo | -7.9% | [-16.7%, -1.4%] | **fail** |
| S6_oracle_mean_vs_fifo | oracle+weights+pausing vs fifo | -31.2% | [-43.2%, -19.4%] | **pass** |
| S7_sensitivity_mean_vs_fifo | predictive_warm+weights_alt+pausing vs fifo | -25.8% | [-38.2%, -12.8%] | **pass** |
| S8_sensitivity_p99_vs_fifo | predictive_warm+weights_alt+pausing vs fifo | 1.09× | [0.96, 1.24] | **pass** |

## Offered load 0.95

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 4575.88 | 171 | 36712.5 | 0 | 6548.7 | 37454.7 | 1.0 / 36377300 |
| predictive_warm | 4575.72 | 151 | 36711.3 | 0 | 6550.4 | 37458.4 | 1.0 / 36373600 |
| predictive_warm+weights+no_aging | 4408.58 | 109 | 37495.3 | 0 | 6220.0 | 38286.8 | 1.0 / 36962200 |
| predictive_warm+weights | 3941.86 | 140 | 37525.0 | 0 | 5350.0 | 38566.5 | 1.0 / 37109100 |
| predictive_warm+weights+pausing | 4308.61 | 109 | 36655.8 | 0 | 6373.9 | 39753.9 | 1.0 / 34006500 |
| oracle+weights+pausing | 2182.72 | 109 | 36362.7 | 0 | 6552.5 | 37471.8 | 1.0 / 85198 |
| predictive_warm+weights_alt+pausing | 4320.08 | 109 | 36457.8 | 0 | 6396.8 | 40945.6 | 1.0 / 35933800 |

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -27.3% | [-40.6%, -13.9%] | **pass** |
| G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.08× | [0.96, 1.22] | **pass** |
| G2_long_class_max_wait | predictive_warm+weights+pausing vs fifo | 1.46× | [1.04, 2.23] | **fail** |
| S1_pausing_vs_plain_aging | predictive_warm+weights+pausing vs predictive_warm+weights | -21.0% | [-36.1%, -5.9%] | **pass** |
| S2_vs_todays_aging | predictive_warm+weights+pausing vs predictive_warm | -25.0% | [-39.1%, -11.4%] | **pass** |
| S3_pausing_cost_vs_no_aging | predictive_warm+weights+pausing vs predictive_warm+weights+no_aging | -3.0% | [-7.0%, +0.5%] | **pass** |
| S4_no_aging_long_max_wait | predictive_warm+weights+no_aging vs fifo | 1.48× | [1.02, 2.32] | **fail** |
| S5_plain_aging_mean_vs_fifo | predictive_warm+weights vs fifo | -7.2% | [-14.9%, -0.9%] | **fail** |
| S6_oracle_mean_vs_fifo | oracle+weights+pausing vs fifo | -39.0% | [-50.3%, -26.6%] | **pass** |
| S7_sensitivity_mean_vs_fifo | predictive_warm+weights_alt+pausing vs fifo | -26.5% | [-39.8%, -13.0%] | **pass** |
| S8_sensitivity_p99_vs_fifo | predictive_warm+weights_alt+pausing vs fifo | 1.08× | [0.97, 1.22] | **pass** |

**Gate at load 0.95: NOT PASSED.**

Without the extreme window (window 6, highest FIFO mean; reported, not gated):

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| P1_mean_latency_vs_fifo | predictive_warm+weights+pausing vs fifo | -30.9% | [-44.4%, -16.9%] | **pass** |
| G1_all_p99_vs_fifo | predictive_warm+weights+pausing vs fifo | 1.08× | [0.95, 1.24] | **pass** |
| G2_long_class_max_wait | predictive_warm+weights+pausing vs fifo | 1.52× | [1.03, 2.41] | **fail** |
| S1_pausing_vs_plain_aging | predictive_warm+weights+pausing vs predictive_warm+weights | -26.2% | [-40.0%, -12.8%] | **pass** |
| S2_vs_todays_aging | predictive_warm+weights+pausing vs predictive_warm | -28.3% | [-43.2%, -13.6%] | **pass** |
| S3_pausing_cost_vs_no_aging | predictive_warm+weights+pausing vs predictive_warm+weights+no_aging | -3.1% | [-7.3%, +1.0%] | **pass** |
| S4_no_aging_long_max_wait | predictive_warm+weights+no_aging vs fifo | 1.55× | [1.02, 2.53] | **fail** |
| S5_plain_aging_mean_vs_fifo | predictive_warm+weights vs fifo | -6.1% | [-14.8%, +0.1%] | **fail** |
| S6_oracle_mean_vs_fifo | oracle+weights+pausing vs fifo | -36.7% | [-49.9%, -24.0%] | **pass** |
| S7_sensitivity_mean_vs_fifo | predictive_warm+weights_alt+pausing vs fifo | -30.0% | [-43.8%, -16.2%] | **pass** |
| S8_sensitivity_p99_vs_fifo | predictive_warm+weights_alt+pausing vs fifo | 1.09× | [0.97, 1.25] | **pass** |

