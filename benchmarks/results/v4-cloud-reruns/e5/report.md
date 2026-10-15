# Production-flow benchmark (v4-cloud-aging-1)

Seeds per arm: 5, 5, 5 (fifo, predictive, predictive_weighted). Values are medians across seeds; criteria use paired per-seed deltas with a bootstrap 95% interval.

| Arm | Mean ms | P50 ms | P95 ms | P99 ms | Short-class P50 ms | Export max wait s | Slowdown P50 / P99 | Throughput/s |
|---|---|---|---|---|---|---|---|---|
| fifo | 6734.8 | 3516.6 | 19291.5 | 20289.0 | 3860.4 | 18.44 | 181.6 / 6967.8 | 115.9 |
| predictive | 4839.5 | 334.6 | 18353.1 | 20988.1 | 38.7 | 19.09 | 12.4 / 3783.3 | 115.9 |
| predictive_weighted | 2891.4 | 26.3 | 21296.6 | 23230.2 | 14.9 | 21.09 | 2.9 / 1429.8 | 115.9 |

## Pre-registered criteria

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| G1_long_class_max_wait_vs_fifo | predictive_weighted vs fifo | 1.13× | [1.10, 1.14] | **pass** |
| G2_all_p99_vs_predictive | predictive_weighted vs predictive | 1.10× | [1.09, 1.11] | **fail** |
| G3_no_lost_work | predictive_weighted vs – | – | – | **pass** |
| G4_v2_gain_kept_vs_fifo | predictive_weighted vs fifo | -57.5% | [-58.1%, -56.9%] | **pass** |
| G5_throughput_vs_fifo | predictive_weighted vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| S1_mean_vs_predictive | predictive_weighted vs predictive | -38.4% | [-41.9%, -34.7%] | **pass** |
| S2_predictive_replicates_v2 | predictive vs fifo | -30.7% | [-35.4%, -27.0%] | **pass** |

**Gate (G1_long_class_max_wait_vs_fifo, G2_all_p99_vs_predictive, G3_no_lost_work, G4_v2_gain_kept_vs_fifo, G5_throughput_vs_fifo): NOT PASSED.**

## Per-class P50 / P99 latency (ms, median across seeds)

| Class | fifo | predictive | predictive_weighted |
|---|---|---|---|
| deliver_webhook | 2862.4 / 20541.6 | 3830.4 / 21242.1 | 3906.3 / 23513.8 |
| export_report | 2067.1 / 19313.4 | 1941.1 / 20013.0 | 1892.5 / 21960.2 |
| generate_invoice | 4694.8 / 20255.6 | 17.8 / 10708.7 | 10.3 / 83.5 |
| push_notification | 3937.9 / 20295.5 | 73.9 / 15792.3 | 20.4 / 154.7 |
| reindex_product | 415.9 / 20004.2 | 11.4 / 10688.2 | 7.6 / 90.8 |
| reserve_inventory | 5207.8 / 20275.9 | 637.1 / 15841.7 | 17.6 / 158.0 |
| resize_image | 646.0 / 20196.4 | 187.3 / 21010.4 | 93.5 / 23275.5 |
| send_order_email | 4854.9 / 20307.7 | 6245.6 / 21122.2 | 6598.9 / 23396.9 |
| send_sms | 4042.5 / 20399.4 | 5793.7 / 21167.5 | 6045.7 / 23444.7 |

## Mean latency by phase (ms, median across seeds)

| Phase | fifo | predictive | predictive_weighted |
|---|---|---|---|
| steady | 47.6 | 40.2 | 40.3 |
| ramp | 240.4 | 124.3 | 123.5 |
| flash_sale | 12320.1 | 8550.7 | 5428.3 |
| recovery | 12780.0 | 9936.0 | 5186.3 |
| shifted | 562.2 | 256.4 | 258.3 |

## Predictor behaviour (predictive arm, median across seeds)

Lookups 34956, routed by prediction 34767; outcomes {'predicted': 34767, 'unready': 103, 'cold_key': 86, 'high_spread': 0, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 1.48 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.72; keys 9; parent fallbacks 0.

## Predictor behaviour (predictive_weighted arm, median across seeds)

Lookups 34956, routed by prediction 34767; outcomes {'predicted': 34767, 'unready': 103, 'cold_key': 83, 'high_spread': 0, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 1.56 µs; tier agreement 0.79; drift alerts 0; mean |log2 error| 0.71; keys 9; parent fallbacks 0.
