# Production-flow benchmark (v3-sizebins-1)

Seeds per arm: 5, 5, 5, 5 (fifo, predictive, predictive_sized, oracle). Values are medians across seeds; criteria use paired per-seed deltas with a bootstrap 95% interval.

| Arm | Mean ms | P50 ms | P95 ms | P99 ms | Short-class P50 ms | Export max wait s | Slowdown P50 / P99 | Throughput/s |
|---|---|---|---|---|---|---|---|---|
| fifo | 10089.2 | 9591.9 | 23279.2 | 24437.6 | 9689.8 | 23.04 | 547.3 / 7897.6 | 114.7 |
| predictive | 7325.2 | 4920.7 | 22925.2 | 25638.7 | 1429.2 | 24.32 | 224.9 / 4653.7 | 113.9 |
| predictive_sized | 7403.2 | 5209.3 | 22826.1 | 25596.0 | 1710.8 | 24.35 | 235.2 / 4703.0 | 113.7 |
| oracle | 7210.2 | 4847.7 | 22315.6 | 25385.5 | 1829.5 | 23.97 | 209.1 / 4526.2 | 113.9 |

## Pre-registered criteria

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| P1_mean_latency_vs_predictive | predictive_sized vs predictive | -1.5% | [-5.1%, +1.7%] | **fail** |
| G1_all_p99_vs_predictive | predictive_sized vs predictive | 1.00× | [0.99, 1.02] | **pass** |
| G2_unsized_classes_vs_predictive | predictive_sized vs predictive | -2.3% | [-5.8%, +0.6%] | **pass** |
| G3_no_lost_work | predictive_sized vs – | – | – | **pass** |
| G4_long_class_max_wait_vs_fifo | predictive_sized vs fifo | 1.02× | [0.97, 1.04] | **pass** |
| G5_throughput_vs_fifo | predictive_sized vs fifo | 1.00× | [0.99, 1.00] | **pass** |
| S1_resize_mean_vs_predictive | predictive_sized vs predictive | -4.4% | [-8.1%, -0.8%] | **fail** |
| S2_resize_shifted_mean_vs_predictive | predictive_sized vs predictive | -5.1% | [-14.3%, +2.9%] | **fail** |
| S3_invoice_mean_vs_predictive | predictive_sized vs predictive | +11.1% | [+3.5%, +21.0%] | **fail** |
| S4_sized_mean_vs_fifo | predictive_sized vs fifo | -26.2% | [-31.7%, -22.4%] | **pass** |
| S5_predictive_replicates_v2 | predictive vs fifo | -25.1% | [-29.0%, -21.7%] | **pass** |

**Gate (P1_mean_latency_vs_predictive, G1_all_p99_vs_predictive, G2_unsized_classes_vs_predictive, G3_no_lost_work, G4_long_class_max_wait_vs_fifo, G5_throughput_vs_fifo): NOT PASSED.**

## Per-class P50 / P99 latency (ms, median across seeds)

| Class | fifo | predictive | predictive_sized | oracle |
|---|---|---|---|---|
| deliver_webhook | 9232.6 / 24702.9 | 10741.5 / 26010.0 | 10951.8 / 25974.6 | 10009.8 / 25756.9 |
| export_report | 10745.9 / 24444.9 | 11710.8 / 25535.0 | 12057.6 / 25577.4 | 11376.8 / 25576.4 |
| generate_invoice | 9989.7 / 24393.5 | 322.6 / 15416.9 | 945.1 / 19163.1 | 657.2 / 19868.7 |
| push_notification | 9677.1 / 24457.9 | 5511.7 / 20490.7 | 5617.6 / 20467.1 | 4432.0 / 20698.6 |
| reindex_product | 8485.9 / 24086.4 | 42.3 / 15380.5 | 60.4 / 15355.4 | 34.1 / 15124.2 |
| reserve_inventory | 10189.6 / 24460.0 | 857.5 / 18151.8 | 634.5 / 15460.7 | 2117.0 / 20051.5 |
| resize_image | 8555.1 / 24349.3 | 9806.4 / 25708.5 | 9751.5 / 25647.6 | 7013.7 / 25391.6 |
| send_order_email | 10215.0 / 24437.6 | 11634.0 / 25856.9 | 11857.1 / 25833.9 | 10792.2 / 25557.8 |
| send_sms | 9467.7 / 24552.6 | 11151.6 / 25928.5 | 11263.6 / 25900.1 | 10708.8 / 25657.2 |

## Mean latency by phase (ms, median across seeds)

| Phase | fifo | predictive | predictive_sized | oracle |
|---|---|---|---|---|
| steady | 63.5 | 52.0 | 53.1 | 47.8 |
| ramp | 361.8 | 195.7 | 194.6 | 198.0 |
| flash_sale | 14856.1 | 10228.7 | 10245.7 | 9933.1 |
| recovery | 18720.5 | 15612.5 | 15714.1 | 15368.3 |
| shifted | 6824.1 | 4134.9 | 4823.3 | 4359.7 |

## Size-driven classes: mean latency overall / in the shifted phase (ms, median across seeds)

| Class | fifo | predictive | predictive_sized | oracle |
|---|---|---|---|---|
| resize_image | 8922.7 / 6926.2 | 9994.4 / 8494.4 | 9650.3 / 9042.6 | 8404.3 / 7575.8 |
| generate_invoice | 10411.1 / 6650.7 | 4200.1 / 935.0 | 4680.7 / 1546.0 | 4815.1 / 1488.6 |
| export_report | 10445.1 / 9357.5 | 10660.1 / 10441.8 | 10765.3 / 10933.1 | 11152.3 / 10113.3 |
| *all other classes* | 10126.3 | 7677.1 | 7715.5 | 7549.1 |

## Predictor behaviour (predictive arm, median across seeds)

Lookups 34956, routed by prediction 34767; outcomes {'predicted': 34767, 'unready': 103, 'cold_key': 86, 'high_spread': 0, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 5.06 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.84; keys 9; parent fallbacks 0.

## Predictor behaviour (predictive_sized arm, median across seeds)

Lookups 34956, routed by prediction 34767; outcomes {'predicted': 34767, 'unready': 103, 'cold_key': 85, 'high_spread': 0, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 4.80 µs; tier agreement 0.81; drift alerts 0; mean |log2 error| 0.66; keys 28; parent fallbacks 263.
