# Production-flow benchmark (v4-cloud-sizebins-1)

Seeds per arm: 5, 5, 5, 5 (fifo, predictive, predictive_sized, oracle). Values are medians across seeds; criteria use paired per-seed deltas with a bootstrap 95% interval.

| Arm | Mean ms | P50 ms | P95 ms | P99 ms | Short-class P50 ms | Export max wait s | Slowdown P50 / P99 | Throughput/s |
|---|---|---|---|---|---|---|---|---|
| fifo | 6741.9 | 3517.3 | 19307.8 | 20296.8 | 3867.1 | 18.46 | 181.5 / 6956.3 | 115.9 |
| predictive | 4833.4 | 336.2 | 18365.7 | 20986.8 | 39.1 | 19.06 | 12.5 / 3796.2 | 115.9 |
| predictive_sized | 4801.0 | 337.6 | 18245.0 | 21107.9 | 39.2 | 19.21 | 12.4 / 3826.2 | 115.9 |
| oracle | 4590.5 | 220.3 | 18172.0 | 21104.9 | 32.3 | 19.23 | 9.5 / 3239.9 | 115.9 |

## Pre-registered criteria

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| P1_mean_latency_vs_predictive | predictive_sized vs predictive | -1.5% | [-4.5%, +0.4%] | **fail** |
| G1_all_p99_vs_predictive | predictive_sized vs predictive | 1.00× | [1.00, 1.00] | **pass** |
| G2_unsized_classes_vs_predictive | predictive_sized vs predictive | -2.0% | [-5.1%, -0.1%] | **pass** |
| G3_no_lost_work | predictive_sized vs – | – | – | **pass** |
| G4_long_class_max_wait_vs_fifo | predictive_sized vs fifo | 0.99× | [0.89, 1.05] | **pass** |
| G5_throughput_vs_fifo | predictive_sized vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| S1_resize_mean_vs_predictive | predictive_sized vs predictive | -1.1% | [-19.4%, +17.2%] | **fail** |
| S2_resize_shifted_mean_vs_predictive | predictive_sized vs predictive | +33.7% | [-25.3%, +99.8%] | **fail** |
| S3_invoice_mean_vs_predictive | predictive_sized vs predictive | +9.5% | [+4.4%, +15.9%] | **fail** |
| S4_sized_mean_vs_fifo | predictive_sized vs fifo | -31.7% | [-35.0%, -28.7%] | **pass** |
| S5_predictive_replicates_v2 | predictive vs fifo | -30.6% | [-34.7%, -26.9%] | **pass** |

**Gate (P1_mean_latency_vs_predictive, G1_all_p99_vs_predictive, G2_unsized_classes_vs_predictive, G3_no_lost_work, G4_long_class_max_wait_vs_fifo, G5_throughput_vs_fifo): NOT PASSED.**

## Per-class P50 / P99 latency (ms, median across seeds)

| Class | fifo | predictive | predictive_sized | oracle |
|---|---|---|---|---|
| deliver_webhook | 2860.0 / 20550.0 | 3837.6 / 21249.8 | 3743.8 / 21362.5 | 3466.9 / 21366.6 |
| export_report | 2059.8 / 19331.7 | 1938.9 / 20134.9 | 1929.3 / 20081.0 | 2055.7 / 20094.2 |
| generate_invoice | 4692.3 / 20261.1 | 18.3 / 10705.5 | 18.4 / 11028.4 | 22.5 / 15074.6 |
| push_notification | 3934.1 / 20306.3 | 74.1 / 15788.6 | 72.3 / 15922.5 | 68.5 / 18728.3 |
| reindex_product | 408.2 / 20010.2 | 11.3 / 10687.7 | 11.9 / 10807.4 | 11.8 / 10802.7 |
| reserve_inventory | 5204.0 / 20289.6 | 641.3 / 15833.8 | 37.8 / 14580.1 | 35.7 / 15865.8 |
| resize_image | 641.3 / 20202.7 | 184.6 / 21010.5 | 149.2 / 20643.1 | 222.5 / 21020.9 |
| send_order_email | 4863.5 / 20315.6 | 6261.7 / 21122.2 | 6181.5 / 21237.9 | 5478.9 / 21226.0 |
| send_sms | 4049.0 / 20397.9 | 5850.6 / 21163.4 | 5736.3 / 21279.0 | 5221.5 / 21300.9 |

## Mean latency by phase (ms, median across seeds)

| Phase | fifo | predictive | predictive_sized | oracle |
|---|---|---|---|---|
| steady | 47.3 | 40.5 | 40.2 | 39.3 |
| ramp | 237.4 | 124.1 | 126.8 | 123.9 |
| flash_sale | 12330.1 | 8543.9 | 8081.0 | 8017.5 |
| recovery | 12799.9 | 9919.3 | 9864.3 | 9578.4 |
| shifted | 562.3 | 253.2 | 246.1 | 251.5 |

## Size-driven classes: mean latency overall / in the shifted phase (ms, median across seeds)

| Class | fifo | predictive | predictive_sized | oracle |
|---|---|---|---|---|
| resize_image | 5493.3 / 597.3 | 5460.9 / 584.5 | 4320.0 / 422.1 | 4596.7 / 471.5 |
| generate_invoice | 7193.3 / 495.0 | 2174.2 / 20.1 | 2288.1 / 20.7 | 2654.7 / 24.0 |
| export_report | 6061.8 / 2075.8 | 5708.9 / 2176.5 | 5747.7 / 2091.2 | 6391.5 / 2130.1 |
| *all other classes* | 6763.6 | 5273.7 | 5306.6 | 4946.6 |

## Predictor behaviour (predictive arm, median across seeds)

Lookups 34956, routed by prediction 34767; outcomes {'predicted': 34767, 'unready': 103, 'cold_key': 86, 'high_spread': 0, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 1.51 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.71; keys 9; parent fallbacks 0.

## Predictor behaviour (predictive_sized arm, median across seeds)

Lookups 34956, routed by prediction 34767; outcomes {'predicted': 34767, 'unready': 103, 'cold_key': 86, 'high_spread': 0, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 1.58 µs; tier agreement 0.81; drift alerts 0; mean |log2 error| 0.60; keys 28; parent fallbacks 262.
