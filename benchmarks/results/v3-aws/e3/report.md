# Production-flow benchmark (v3-brokers-prodflow-1)

Seeds per arm: 5, 5, 5, 5, 5, 5, 5, 5 (fifo, static_tuned, predictive, predictive_weighted, rabbitmq_fifo, rabbitmq_priority, rabbitmq_predictive, redis_fifo). Values are medians across seeds; criteria use paired per-seed deltas with a bootstrap 95% interval.

| Arm | Mean ms | P50 ms | P95 ms | P99 ms | Short-class P50 ms | Export max wait s | Slowdown P50 / P99 | Throughput/s |
|---|---|---|---|---|---|---|---|---|
| fifo | 7319.0 | 4148.3 | 20327.4 | 21395.4 | 4309.3 | 19.66 | 233.1 / 7373.7 | 115.9 |
| static_tuned | 6035.8 | 1659.0 | 20134.6 | 22600.7 | 314.8 | 23.69 | 71.9 / 5996.1 | 115.9 |
| predictive | 5295.8 | 555.3 | 18690.5 | 22322.9 | 44.9 | 20.45 | 20.8 / 4208.4 | 115.9 |
| predictive_weighted | 3140.7 | 27.0 | 22684.9 | 24498.4 | 15.1 | 22.50 | 3.0 / 1512.7 | 115.8 |
| rabbitmq_fifo | 6642.7 | 3380.2 | 19177.2 | 20127.9 | 3668.0 | 18.29 | 171.6 / 6868.9 | 116.1 |
| rabbitmq_priority | 4152.8 | 34.2 | 16488.0 | 72187.4 | 17.8 | 101.22 | 3.1 / 5702.1 | 116.0 |
| rabbitmq_predictive | 2838.8 | 25.5 | 21052.7 | 23074.2 | 14.4 | 20.90 | 2.8 / 1407.3 | 116.0 |
| redis_fifo | 7007.6 | 3877.0 | 19809.6 | 20819.6 | 4016.0 | 19.01 | 205.9 / 7124.8 | 115.9 |

## Pre-registered criteria

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| P1_mean_vs_rabbitmq_fifo | predictive vs rabbitmq_fifo | -21.7% | [-26.2%, -18.4%] | **pass** |
| P2_short_p50_vs_rabbitmq_fifo | predictive vs rabbitmq_fifo | -89.5% | [-98.8%, -75.5%] | **pass** |
| P3_mean_vs_redis_fifo | predictive vs redis_fifo | -25.5% | [-29.5%, -22.6%] | **pass** |
| P4_short_p50_vs_redis_fifo | predictive vs redis_fifo | -90.7% | [-99.0%, -78.0%] | **pass** |
| G1_all_p99_vs_rabbitmq_fifo | predictive vs rabbitmq_fifo | 1.10× | [1.07, 1.12] | **pass** |
| G2_long_max_wait_vs_rabbitmq_priority | predictive vs rabbitmq_priority | 0.20× | [0.18, 0.23] | **pass** |
| G3_no_lost_work | predictive vs – | – | – | **pass** |
| G4_throughput_vs_rabbitmq_fifo | predictive vs rabbitmq_fifo | 1.00× | [0.99, 1.00] | **pass** |
| S1_fifo_equivalence_rabbitmq | fifo vs rabbitmq_fifo | +11.0% | [+10.4%, +11.6%] | **fail** |
| S2_fifo_equivalence_redis | fifo vs redis_fifo | +5.6% | [+4.9%, +6.2%] | **pass** |
| S3_transfer_rabbitmq_predictive_vs_fifo | rabbitmq_predictive vs rabbitmq_fifo | -57.6% | [-58.6%, -56.8%] | **pass** |
| S4_rabbitmq_predictive_vs_priority | rabbitmq_predictive vs rabbitmq_priority | -26.3% | [-31.5%, -21.1%] | **pass** |
| S5_predictive_vs_static_tuned | predictive vs static_tuned | -13.2% | [-17.1%, -10.7%] | **pass** |
| S6_weighted_mean_vs_rabbitmq_fifo | predictive_weighted vs rabbitmq_fifo | -52.4% | [-53.4%, -51.0%] | **pass** |
| S7_weighted_p99_vs_rabbitmq_fifo | predictive_weighted vs rabbitmq_fifo | 1.21× | [1.20, 1.24] | **pass** |
| S8_rabbitmq_priority_long_wait_vs_fifo | rabbitmq_priority vs rabbitmq_fifo | 5.23× | [4.79, 5.55] | **fail** |
| G3b_no_lost_work_rabbitmq_fifo | rabbitmq_fifo vs – | – | – | **pass** |
| G3c_no_lost_work_rabbitmq_priority | rabbitmq_priority vs – | – | – | **pass** |
| G3d_no_lost_work_rabbitmq_predictive | rabbitmq_predictive vs – | – | – | **pass** |
| G3e_no_lost_work_redis_fifo | redis_fifo vs – | – | – | **pass** |

**Gate (P1_mean_vs_rabbitmq_fifo, P2_short_p50_vs_rabbitmq_fifo, P3_mean_vs_redis_fifo, P4_short_p50_vs_redis_fifo, G1_all_p99_vs_rabbitmq_fifo, G2_long_max_wait_vs_rabbitmq_priority, G3_no_lost_work, G4_throughput_vs_rabbitmq_fifo): PASSED.**

## Per-class P50 / P99 latency (ms, median across seeds)

| Class | fifo | static_tuned | predictive | predictive_weighted | rabbitmq_fifo | rabbitmq_priority | rabbitmq_predictive | redis_fifo |
|---|---|---|---|---|---|---|---|---|
| deliver_webhook | 3414.5 / 21661.2 | 2701.6 / 21824.1 | 4329.4 / 22650.8 | 4376.3 / 24830.0 | 2679.2 / 20143.6 | 464.4 / 17384.2 | 3807.0 / 23228.6 | 2979.3 / 21073.3 |
| export_report | 3529.2 / 20517.9 | 6768.8 / 24960.0 | 2086.9 / 21312.6 | 1997.5 / 23362.8 | 2005.0 / 19143.7 | 2591.4 / 102327.7 | 1784.1 / 21762.3 | 2502.0 / 19868.3 |
| generate_invoice | 5567.5 / 21356.8 | 5813.4 / 21621.7 | 20.8 / 12082.3 | 10.6 / 81.0 | 4538.7 / 20114.4 | 718.2 / 17375.2 | 10.0 / 79.8 | 5093.3 / 20786.3 |
| push_notification | 4404.6 / 21401.8 | 58.2 / 16330.4 | 119.7 / 17191.4 | 21.2 / 155.7 | 3841.4 / 20152.8 | 18.0 / 97.6 | 20.6 / 128.7 | 4084.8 / 20827.1 |
| reindex_product | 709.3 / 21086.1 | 23.0 / 16361.8 | 12.4 / 12076.3 | 8.3 / 92.3 | 385.1 / 19862.2 | 9.0 / 102.6 | 7.6 / 86.4 | 493.4 / 20528.4 |
| reserve_inventory | 6235.1 / 21396.3 | 142.2 / 16344.2 | 1172.2 / 17190.7 | 17.8 / 153.7 | 5146.2 / 20134.6 | 14.4 / 90.2 | 17.3 / 117.8 | 5598.7 / 20814.4 |
| resize_image | 1227.5 / 21309.7 | 5258.7 / 26799.8 | 1525.0 / 19641.6 | 220.7 / 24551.2 | 598.4 / 20057.7 | 2192.0 / 101896.8 | 308.4 / 23134.8 | 725.4 / 20742.6 |
| send_order_email | 5924.1 / 21422.3 | 5926.3 / 21655.1 | 7018.2 / 22525.5 | 7717.3 / 24640.3 | 4732.5 / 20170.2 | 885.9 / 17377.6 | 6394.7 / 23238.1 | 5319.3 / 20848.5 |
| send_sms | 4565.0 / 21514.5 | 5181.3 / 21743.4 | 6348.2 / 22607.2 | 6651.3 / 24720.4 | 3956.9 / 20257.4 | 572.3 / 17461.7 | 6001.0 / 23276.5 | 4223.7 / 20933.1 |

## Mean latency by phase (ms, median across seeds)

| Phase | fifo | static_tuned | predictive | predictive_weighted | rabbitmq_fifo | rabbitmq_priority | rabbitmq_predictive | redis_fifo |
|---|---|---|---|---|---|---|---|---|
| steady | 48.6 | 43.7 | 40.9 | 41.0 | 47.6 | 43.1 | 40.2 | 47.9 |
| ramp | 257.9 | 183.1 | 134.7 | 136.6 | 230.9 | 366.8 | 125.5 | 241.5 |
| flash_sale | 12900.7 | 10544.0 | 9020.6 | 5624.0 | 12223.5 | 8262.5 | 5358.6 | 12624.1 |
| recovery | 14200.9 | 12244.3 | 11232.7 | 5737.7 | 12561.7 | 5189.2 | 5074.4 | 13425.1 |
| shifted | 1109.5 | 562.9 | 502.0 | 469.4 | 480.3 | 203.9 | 223.8 | 783.7 |

## Predictor behaviour (predictive arm, median across seeds)

Lookups 34956, routed by prediction 34767; outcomes {'predicted': 34767, 'unready': 103, 'cold_key': 86, 'high_spread': 0, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 1.50 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.71; keys 9; parent fallbacks 0.

## Predictor behaviour (predictive_weighted arm, median across seeds)

Lookups 34956, routed by prediction 34767; outcomes {'predicted': 34767, 'unready': 103, 'cold_key': 83, 'high_spread': 0, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 1.47 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.71; keys 9; parent fallbacks 0.
