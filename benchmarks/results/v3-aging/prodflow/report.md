# Production-flow benchmark (v3-aging-prodflow-1)

Seeds per arm: 5, 5, 5 (fifo, predictive, predictive_weighted). Values are medians across seeds; criteria use paired per-seed deltas with a bootstrap 95% interval.

| Arm | Mean ms | P50 ms | P95 ms | P99 ms | Short-class P50 ms | Export max wait s | Slowdown P50 / P99 | Throughput/s |
|---|---|---|---|---|---|---|---|---|
| fifo | 13167.4 | 15544.1 | 26020.8 | 27079.6 | 15565.0 | 25.91 | 841.6 / 8911.1 | 111.0 |
| predictive | 9660.4 | 9213.5 | 25209.9 | 27789.0 | 6418.5 | 26.71 | 505.0 / 5433.5 | 111.2 |
| predictive_weighted | 5706.7 | 31.4 | 29291.3 | 31398.7 | 18.3 | 30.10 | 4.2 / 1664.3 | 111.0 |

## Pre-registered criteria

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| G1_long_class_max_wait_vs_fifo | predictive_weighted vs fifo | 1.14× | [1.11, 1.16] | **pass** |
| G2_all_p99_vs_predictive | predictive_weighted vs predictive | 1.12× | [1.11, 1.13] | **fail** |
| G3_no_lost_work | predictive_weighted vs – | – | – | **pass** |
| G4_v2_gain_kept_vs_fifo | predictive_weighted vs fifo | -56.9% | [-57.4%, -56.4%] | **pass** |
| G5_throughput_vs_fifo | predictive_weighted vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| S1_mean_vs_predictive | predictive_weighted vs predictive | -42.0% | [-45.5%, -38.7%] | **pass** |
| S2_predictive_replicates_v2 | predictive vs fifo | -25.4% | [-29.2%, -21.6%] | **pass** |

**Gate (G1_long_class_max_wait_vs_fifo, G2_all_p99_vs_predictive, G3_no_lost_work, G4_v2_gain_kept_vs_fifo, G5_throughput_vs_fifo): NOT PASSED.**

## Per-class P50 / P99 latency (ms, median across seeds)

| Class | fifo | predictive | predictive_weighted |
|---|---|---|---|
| deliver_webhook | 15589.0 / 27356.0 | 16303.5 / 28436.1 | 18202.2 / 32501.4 |
| export_report | 18518.7 / 28241.6 | 18644.0 / 29004.6 | 16140.9 / 32329.0 |
| generate_invoice | 15547.0 / 27038.1 | 5070.3 / 17570.0 | 15.6 / 113.8 |
| push_notification | 15667.8 / 27067.9 | 10711.2 / 22652.3 | 24.3 / 512.8 |
| reindex_product | 14313.4 / 26711.8 | 5118.1 / 17533.6 | 12.7 / 115.2 |
| reserve_inventory | 15822.2 / 27063.8 | 5281.7 / 17658.0 | 16.9 / 119.9 |
| resize_image | 14418.3 / 26931.3 | 15410.1 / 27868.8 | 17254.8 / 31557.1 |
| send_order_email | 15696.5 / 27085.4 | 16503.5 / 28011.1 | 18390.3 / 31730.1 |
| send_sms | 15361.2 / 27130.9 | 16092.6 / 28073.5 | 18082.7 / 31786.4 |

## Mean latency by phase (ms, median across seeds)

| Phase | fifo | predictive | predictive_weighted |
|---|---|---|---|
| steady | 93.7 | 68.8 | 69.2 |
| ramp | 764.3 | 380.7 | 383.9 |
| flash_sale | 16665.5 | 11675.9 | 7707.2 |
| recovery | 22707.4 | 18640.0 | 9296.5 |
| shifted | 14604.0 | 9935.3 | 5989.8 |

## Predictor behaviour (predictive arm, median across seeds)

Lookups 34956, routed by prediction 34766; outcomes {'predicted': 34766, 'unready': 103, 'cold_key': 87, 'high_spread': 0, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 2.68 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.92; keys 9; parent fallbacks 0.

## Predictor behaviour (predictive_weighted arm, median across seeds)

Lookups 34956, routed by prediction 34765; outcomes {'predicted': 34765, 'unready': 103, 'cold_key': 83, 'high_spread': 0, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 2.61 µs; tier agreement 0.79; drift alerts 0; mean |log2 error| 0.96; keys 9; parent fallbacks 0.
