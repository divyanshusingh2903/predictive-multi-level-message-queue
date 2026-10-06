# Azure Functions trace simulation (v2-azure-sim-1)

990476 simulated invocations after 990475 history invocations; tier boundaries (history terciles) 1 / 171 ms; 64 workers, aging 5000/500 ms.

## Offered load 0.50

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 4111.54 | 92 | 35293.2 | 0 | 6123.8 | 36003.0 | 1.0 / 33298200 |
| static_history | 4111.60 | 86 | 35291.5 | 0 | 6124.2 | 36004.0 | 1.0 / 33297500 |
| static_random | 4111.22 | 90 | 35288.1 | 0 | 6123.6 | 36002.7 | 1.0 / 33296900 |
| predictive_cold | 4111.93 | 84 | 35293.3 | 0 | 6125.6 | 36002.9 | 1.0 / 33298100 |
| predictive_warm | 4111.93 | 84 | 35293.3 | 0 | 6125.6 | 36002.9 | 1.0 / 33298100 |
| oracle | 4110.74 | 83 | 35290.6 | 0 | 6123.9 | 36002.8 | 1.0 / 33292600 |

| Criterion | Point | 95% CI | Result |
|---|---|---|---|
| P1_mean_latency_vs_fifo | -0.9% | [-1.8%, -0.2%] | **fail** |
| G1_all_p99_vs_fifo | 1.00× | [0.99, 1.00] | **pass** |
| G2_long_class_max_wait | 1.09× | [1.00, 1.25] | **pass** |
| S1_vs_static_history_noninferior | -0.4% | [-1.4%, +0.6%] | **pass** |
| S2_beats_static_random | +0.1% | [-0.3%, +0.4%] | **fail** |
| S3_cold_start_vs_fifo | -0.9% | [-1.8%, -0.2%] | **fail** |

**Gate at load 0.50: NOT PASSED.**

## Offered load 0.80

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 4474.67 | 109 | 36202.4 | 0 | 6456.1 | 37152.0 | 1.0 / 35765600 |
| static_history | 4474.71 | 109 | 36200.1 | 0 | 6456.8 | 37151.8 | 1.0 / 35765300 |
| static_random | 4474.05 | 109 | 36200.9 | 0 | 6455.6 | 37151.6 | 1.0 / 35753600 |
| predictive_cold | 4474.90 | 109 | 36202.0 | 0 | 6458.1 | 37151.9 | 1.0 / 35756800 |
| predictive_warm | 4474.90 | 109 | 36202.0 | 0 | 6458.1 | 37151.9 | 1.0 / 35756800 |
| oracle | 4473.30 | 109 | 36201.2 | 0 | 6456.2 | 37152.2 | 1.0 / 35759700 |

| Criterion | Point | 95% CI | Result |
|---|---|---|---|
| P1_mean_latency_vs_fifo | -1.4% | [-2.6%, -0.5%] | **fail** |
| G1_all_p99_vs_fifo | 0.99× | [0.98, 1.00] | **pass** |
| G2_long_class_max_wait | 1.18× | [1.00, 1.53] | **pass** |
| S1_vs_static_history_noninferior | -0.2% | [-1.4%, +1.3%] | **pass** |
| S2_beats_static_random | +0.2% | [-0.3%, +0.5%] | **fail** |
| S3_cold_start_vs_fifo | -1.4% | [-2.6%, -0.5%] | **fail** |

**Gate at load 0.80: NOT PASSED.**

## Offered load 0.95

| Arm | Mean s | P50 ms | P99 s | Short P50 ms | Long mean s | Long max wait s | Slowdown P50 / P99 |
|---|---|---|---|---|---|---|---|
| fifo | 4575.88 | 171 | 36712.5 | 0 | 6548.7 | 37454.7 | 1.0 / 36377300 |
| static_history | 4575.86 | 154 | 36710.9 | 0 | 6549.5 | 37458.7 | 1.0 / 36373100 |
| static_random | 4575.10 | 156 | 36711.0 | 0 | 6548.0 | 37458.5 | 1.0 / 36373000 |
| predictive_cold | 4575.92 | 150 | 36712.5 | 0 | 6550.6 | 37458.5 | 1.0 / 36377300 |
| predictive_warm | 4575.92 | 150 | 36712.5 | 0 | 6550.6 | 37458.5 | 1.0 / 36377300 |
| oracle | 4574.17 | 140 | 36710.3 | 0 | 6548.8 | 37454.7 | 1.0 / 36369500 |

| Criterion | Point | 95% CI | Result |
|---|---|---|---|
| P1_mean_latency_vs_fifo | -2.8% | [-5.5%, -0.6%] | **fail** |
| G1_all_p99_vs_fifo | 0.99× | [0.96, 1.00] | **pass** |
| G2_long_class_max_wait | 1.20× | [1.00, 1.61] | **pass** |
| S1_vs_static_history_noninferior | -1.1% | [-1.8%, -0.4%] | **pass** |
| S2_beats_static_random | -0.3% | [-1.2%, +0.5%] | **fail** |
| S3_cold_start_vs_fifo | -2.8% | [-5.5%, -0.6%] | **fail** |

**Gate at load 0.95: NOT PASSED.**

