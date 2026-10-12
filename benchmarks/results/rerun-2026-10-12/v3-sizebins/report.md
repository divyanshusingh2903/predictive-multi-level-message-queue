# Production-flow benchmark (v3-sizebins-1)

Seeds per arm: 5, 5, 5, 5 (fifo, predictive, predictive_sized, oracle). Values are medians across seeds; criteria use paired per-seed deltas with a bootstrap 95% interval.

| Arm | Mean ms | P50 ms | P95 ms | P99 ms | Short-class P50 ms | Export max wait s | Slowdown P50 / P99 | Throughput/s |
|---|---|---|---|---|---|---|---|---|
| fifo | 13164.9 | 15549.3 | 26056.8 | 27120.5 | 15566.7 | 25.96 | 841.3 / 8915.6 | 111.0 |
| predictive | 9596.5 | 9142.2 | 25163.0 | 27740.8 | 6230.2 | 26.66 | 497.6 / 5444.3 | 111.2 |
| predictive_sized | 9641.0 | 9214.4 | 25104.7 | 27744.6 | 6312.1 | 26.70 | 503.7 / 5413.4 | 111.2 |
| oracle | 9687.0 | 9499.0 | 24812.0 | 27615.8 | 6483.2 | 26.56 | 509.0 / 5297.0 | 111.2 |

## Pre-registered criteria

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| P1_mean_latency_vs_predictive | predictive_sized vs predictive | +0.7% | [+0.5%, +0.9%] | **fail** |
| G1_all_p99_vs_predictive | predictive_sized vs predictive | 1.00× | [1.00, 1.00] | **pass** |
| G2_unsized_classes_vs_predictive | predictive_sized vs predictive | +0.2% | [-0.0%, +0.5%] | **pass** |
| G3_no_lost_work | predictive_sized vs – | – | – | **pass** |
| G4_long_class_max_wait_vs_fifo | predictive_sized vs fifo | 1.00× | [0.95, 1.03] | **pass** |
| G5_throughput_vs_fifo | predictive_sized vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| S1_resize_mean_vs_predictive | predictive_sized vs predictive | -2.8% | [-3.4%, -2.1%] | **fail** |
| S2_resize_shifted_mean_vs_predictive | predictive_sized vs predictive | -1.4% | [-2.0%, -0.6%] | **fail** |
| S3_invoice_mean_vs_predictive | predictive_sized vs predictive | +8.7% | [+6.5%, +10.9%] | **fail** |
| S4_sized_mean_vs_fifo | predictive_sized vs fifo | -25.1% | [-28.9%, -21.2%] | **pass** |
| S5_predictive_replicates_v2 | predictive vs fifo | -25.6% | [-29.4%, -21.8%] | **pass** |

**Gate (P1_mean_latency_vs_predictive, G1_all_p99_vs_predictive, G2_unsized_classes_vs_predictive, G3_no_lost_work, G4_long_class_max_wait_vs_fifo, G5_throughput_vs_fifo): NOT PASSED.**

## Per-class P50 / P99 latency (ms, median across seeds)

| Class | fifo | predictive | predictive_sized | oracle |
|---|---|---|---|---|
| deliver_webhook | 15595.0 / 27393.1 | 16128.1 / 28389.7 | 16117.7 / 28398.5 | 15907.6 / 28153.1 |
| export_report | 18449.7 / 28266.4 | 18578.9 / 28928.5 | 18548.3 / 28917.7 | 18400.5 / 28753.7 |
| generate_invoice | 15554.8 / 27077.7 | 4963.1 / 17521.8 | 5628.8 / 21265.5 | 6084.5 / 22198.8 |
| push_notification | 15658.3 / 27109.3 | 10592.8 / 22606.8 | 10587.5 / 22611.8 | 9491.4 / 22863.5 |
| reindex_product | 14249.4 / 26749.6 | 4908.8 / 17488.0 | 4854.4 / 17485.4 | 4724.5 / 17345.2 |
| reserve_inventory | 15791.5 / 27103.6 | 5159.5 / 17605.4 | 5157.0 / 17618.1 | 6395.8 / 22245.0 |
| resize_image | 14354.7 / 26970.0 | 15256.1 / 27823.1 | 14448.5 / 27799.9 | 13368.1 / 27644.1 |
| send_order_email | 15695.0 / 27125.3 | 16316.4 / 27962.0 | 16307.6 / 27974.6 | 15813.3 / 27814.0 |
| send_sms | 15350.1 / 27170.6 | 15947.9 / 28026.8 | 15926.6 / 28051.0 | 15809.5 / 27896.4 |

## Mean latency by phase (ms, median across seeds)

| Phase | fifo | predictive | predictive_sized | oracle |
|---|---|---|---|---|
| steady | 91.5 | 67.1 | 66.6 | 61.4 |
| ramp | 744.6 | 381.4 | 379.2 | 340.6 |
| flash_sale | 16670.6 | 11638.1 | 11720.7 | 11703.3 |
| recovery | 22738.7 | 18574.6 | 18624.8 | 18760.6 |
| shifted | 14561.8 | 9760.6 | 9794.2 | 9933.3 |

## Size-driven classes: mean latency overall / in the shifted phase (ms, median across seeds)

| Class | fifo | predictive | predictive_sized | oracle |
|---|---|---|---|---|
| resize_image | 12208.2 / 14621.5 | 12853.6 / 14723.7 | 12395.0 / 14463.1 | 11633.5 / 13902.2 |
| generate_invoice | 13328.1 / 14402.3 | 5966.4 / 5271.8 | 6491.6 / 5817.3 | 7104.9 / 6775.7 |
| export_report | 14664.4 / 17565.3 | 14294.9 / 17652.4 | 14296.6 / 17633.8 | 14980.1 / 17520.3 |
| *all other classes* | 13212.0 | 9990.9 | 9988.2 | 9995.1 |

## Predictor behaviour (predictive arm, median across seeds)

Lookups 34956, routed by prediction 34766; outcomes {'predicted': 34766, 'unready': 103, 'cold_key': 87, 'high_spread': 0, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 2.60 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.92; keys 9; parent fallbacks 0.

## Predictor behaviour (predictive_sized arm, median across seeds)

Lookups 34956, routed by prediction 34766; outcomes {'predicted': 34766, 'unready': 103, 'cold_key': 87, 'high_spread': 0, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 2.76 µs; tier agreement 0.82; drift alerts 0; mean |log2 error| 0.72; keys 28; parent fallbacks 265.
