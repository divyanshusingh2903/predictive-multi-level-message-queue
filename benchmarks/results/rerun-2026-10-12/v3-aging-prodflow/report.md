# Production-flow benchmark (v3-aging-prodflow-1)

Seeds per arm: 5, 5, 5 (fifo, predictive, predictive_weighted). Values are medians across seeds; criteria use paired per-seed deltas with a bootstrap 95% interval.

| Arm | Mean ms | P50 ms | P95 ms | P99 ms | Short-class P50 ms | Export max wait s | Slowdown P50 / P99 | Throughput/s |
|---|---|---|---|---|---|---|---|---|
| fifo | 13114.4 | 15481.8 | 26005.2 | 27065.6 | 15507.4 | 25.89 | 837.0 / 8907.0 | 111.0 |
| predictive | 9609.0 | 9161.3 | 25181.2 | 27752.3 | 6257.9 | 26.68 | 497.5 / 5457.7 | 111.2 |
| predictive_weighted | 5689.7 | 31.5 | 29247.2 | 31389.5 | 18.2 | 30.13 | 4.2 / 1661.2 | 111.0 |

## Pre-registered criteria

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| G1_long_class_max_wait_vs_fifo | predictive_weighted vs fifo | 1.14× | [1.11, 1.16] | **pass** |
| G2_all_p99_vs_predictive | predictive_weighted vs predictive | 1.12× | [1.12, 1.13] | **fail** |
| G3_no_lost_work | predictive_weighted vs – | – | – | **pass** |
| G4_v2_gain_kept_vs_fifo | predictive_weighted vs fifo | -56.7% | [-57.3%, -56.1%] | **pass** |
| G5_throughput_vs_fifo | predictive_weighted vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| S1_mean_vs_predictive | predictive_weighted vs predictive | -41.9% | [-45.2%, -38.5%] | **pass** |
| S2_predictive_replicates_v2 | predictive vs fifo | -25.3% | [-29.1%, -21.5%] | **pass** |

**Gate (G1_long_class_max_wait_vs_fifo, G2_all_p99_vs_predictive, G3_no_lost_work, G4_v2_gain_kept_vs_fifo, G5_throughput_vs_fifo): NOT PASSED.**

## Per-class P50 / P99 latency (ms, median across seeds)

| Class | fifo | predictive | predictive_weighted |
|---|---|---|---|
| deliver_webhook | 15524.3 / 27339.8 | 16153.9 / 28400.9 | 18101.4 / 32334.7 |
| export_report | 18344.3 / 28168.3 | 18597.9 / 28974.4 | 16297.3 / 32234.7 |
| generate_invoice | 15505.0 / 27022.9 | 4982.6 / 17538.2 | 15.6 / 118.9 |
| push_notification | 15560.2 / 27056.0 | 10612.0 / 22618.2 | 24.2 / 404.3 |
| reindex_product | 14138.7 / 26695.3 | 4934.4 / 17500.6 | 12.6 / 137.6 |
| reserve_inventory | 15711.7 / 27048.6 | 5174.5 / 17621.3 | 16.8 / 113.4 |
| resize_image | 14258.6 / 26917.6 | 15276.3 / 27831.5 | 17163.6 / 31550.3 |
| send_order_email | 15596.7 / 27070.4 | 16348.4 / 27971.4 | 18262.2 / 31727.3 |
| send_sms | 15259.7 / 27118.5 | 15972.9 / 28035.9 | 17977.8 / 31779.4 |

## Mean latency by phase (ms, median across seeds)

| Phase | fifo | predictive | predictive_weighted |
|---|---|---|---|
| steady | 91.7 | 66.9 | 67.6 |
| ramp | 741.9 | 381.2 | 390.1 |
| flash_sale | 16634.3 | 11655.0 | 7688.2 |
| recovery | 22659.9 | 18587.4 | 9283.1 |
| shifted | 14455.3 | 9780.0 | 5954.9 |

## Predictor behaviour (predictive arm, median across seeds)

Lookups 34956, routed by prediction 34766; outcomes {'predicted': 34766, 'unready': 103, 'cold_key': 87, 'high_spread': 0, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 2.61 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.92; keys 9; parent fallbacks 0.

## Predictor behaviour (predictive_weighted arm, median across seeds)

Lookups 34956, routed by prediction 34765; outcomes {'predicted': 34765, 'unready': 103, 'cold_key': 83, 'high_spread': 0, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 2.60 µs; tier agreement 0.79; drift alerts 0; mean |log2 error| 0.96; keys 9; parent fallbacks 0.
