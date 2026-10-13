# Production-flow benchmark (v3-cloud-e1-1)

Seeds per arm: 5, 5, 5, 5, 5, 5, 5 (fifo, static_tuned, static_misconfigured, shadow, predictive, predictive_producer, oracle). Values are medians across seeds; criteria use paired per-seed deltas with a bootstrap 95% interval.

| Arm | Mean ms | P50 ms | P95 ms | P99 ms | Short-class P50 ms | Export max wait s | Slowdown P50 / P99 | Throughput/s |
|---|---|---|---|---|---|---|---|---|
| fifo | 6841.9 | 3645.6 | 19482.2 | 20490.0 | 3944.4 | 18.69 | 192.6 / 7003.4 | 115.9 |
| static_tuned | 5657.0 | 1014.2 | 19387.7 | 21650.6 | 149.3 | 22.94 | 52.1 / 5711.5 | 115.9 |
| static_misconfigured | 7361.6 | 4169.5 | 22349.6 | 25750.4 | 4918.2 | 17.64 | 199.6 / 10336.4 | 115.9 |
| shadow | 6838.5 | 3636.0 | 19476.1 | 20482.6 | 3935.1 | 18.68 | 192.2 / 7001.4 | 115.9 |
| predictive | 4887.8 | 359.6 | 17733.9 | 21450.7 | 42.6 | 19.58 | 13.7 / 3923.6 | 115.9 |
| predictive_producer | 5886.3 | 1883.3 | 17921.1 | 21292.1 | 1715.6 | 20.95 | 74.4 / 6129.6 | 115.8 |
| oracle | 4663.4 | 239.1 | 18314.6 | 21291.2 | 33.4 | 19.42 | 10.0 / 3303.2 | 115.9 |

## Pre-registered criteria

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| P1_mean_latency_vs_fifo | predictive vs fifo | -30.8% | [-35.3%, -27.1%] | **pass** |
| P2_short_class_p50_vs_fifo | predictive vs fifo | -94.8% | [-99.0%, -86.5%] | **pass** |
| G1_all_p99_vs_fifo | predictive vs fifo | 1.03× | [1.02, 1.05] | **pass** |
| G2_long_class_max_wait | predictive vs fifo | 0.99× | [0.90, 1.05] | **pass** |
| G3_no_lost_work | predictive vs – | – | – | **pass** |
| G4_throughput_vs_fifo | predictive vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| S1_vs_static_tuned_noninferior | predictive vs static_tuned | -14.4% | [-18.4%, -10.7%] | **pass** |
| S2_beats_static_misconfigured | predictive vs static_misconfigured | -36.8% | [-41.1%, -33.2%] | **pass** |
| S3_shadow_overhead | shadow vs fifo | -0.1% | [-0.2%, -0.0%] | **pass** |
| S4_zero_config_vs_fifo | predictive_producer vs fifo | -15.7% | [-17.1%, -14.6%] | **pass** |

**Gate (P1_mean_latency_vs_fifo, P2_short_class_p50_vs_fifo, G1_all_p99_vs_fifo, G2_long_class_max_wait, G3_no_lost_work, G4_throughput_vs_fifo): PASSED.**

## Per-class P50 / P99 latency (ms, median across seeds)

| Class | fifo | static_tuned | static_misconfigured | shadow | predictive | predictive_producer | oracle |
|---|---|---|---|---|---|---|---|
| deliver_webhook | 2944.1 / 20744.2 | 3012.8 / 20979.6 | 1529.1 / 18656.0 | 2939.8 / 20738.9 | 3945.7 / 21758.1 | 6257.3 / 24436.0 | 3524.0 / 21559.4 |
| export_report | 2235.7 / 19545.7 | 3138.4 / 24212.9 | 1908.6 / 18496.9 | 2234.5 / 19535.5 | 2038.8 / 20439.6 | 2100.5 / 21809.1 | 2058.0 / 20282.5 |
| generate_invoice | 4842.2 / 20455.5 | 5315.4 / 20768.9 | 2961.1 / 18539.0 | 4819.6 / 20451.0 | 19.1 / 11202.9 | 2894.1 / 18897.1 | 24.4 / 15231.3 |
| push_notification | 4005.1 / 20497.8 | 49.1 / 15477.5 | 8069.1 / 23756.1 | 3999.1 / 20492.0 | 93.5 / 16279.3 | 1876.9 / 18821.3 | 71.3 / 18877.0 |
| reindex_product | 449.2 / 20205.0 | 21.3 / 15497.9 | 3445.4 / 28829.5 | 446.4 / 20194.3 | 11.2 / 11148.6 | 79.4 / 18680.2 | 11.9 / 10992.3 |
| reserve_inventory | 5328.1 / 20480.5 | 77.4 / 15531.9 | 3710.2 / 18523.6 | 5318.9 / 20478.0 | 824.7 / 16343.8 | 3366.0 / 18827.1 | 37.8 / 16045.7 |
| resize_image | 685.6 / 20409.8 | 3178.1 / 25937.3 | 2086.7 / 23635.4 | 684.0 / 20404.8 | 132.4 / 16366.0 | 138.2 / 18758.8 | 250.1 / 21217.3 |
| send_order_email | 5066.6 / 20517.6 | 5400.8 / 20810.8 | 3384.8 / 18580.8 | 5002.5 / 20512.2 | 6510.8 / 21623.6 | 3196.8 / 18931.9 | 5637.9 / 21416.3 |
| send_sms | 4164.0 / 20600.6 | 4444.3 / 20880.4 | 8946.1 / 23782.6 | 4157.1 / 20593.7 | 6050.9 / 21690.9 | 2071.8 / 18826.3 | 5268.8 / 21492.6 |

## Mean latency by phase (ms, median across seeds)

| Phase | fifo | static_tuned | static_misconfigured | shadow | predictive | predictive_producer | oracle |
|---|---|---|---|---|---|---|---|
| steady | 47.6 | 43.0 | 60.6 | 47.7 | 40.4 | 40.0 | 39.2 |
| ramp | 242.7 | 157.7 | 474.4 | 244.4 | 128.7 | 125.4 | 126.6 |
| flash_sale | 12428.4 | 10084.0 | 13249.0 | 12423.2 | 8585.7 | 10593.0 | 8104.5 |
| recovery | 13050.6 | 11198.2 | 13850.9 | 13047.5 | 10099.1 | 11734.1 | 9777.4 |
| shifted | 650.7 | 340.1 | 912.9 | 646.8 | 282.9 | 321.9 | 279.4 |

## Predictor behaviour (predictive arm, median across seeds)

Lookups 34956, routed by prediction 34767; outcomes {'predicted': 34767, 'unready': 103, 'cold_key': 86, 'high_spread': 0, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 1.89 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.71; keys 9; parent fallbacks 0.

## Predictor behaviour (predictive_producer arm, median across seeds)

Lookups 34956, routed by prediction 22639; outcomes {'predicted': 22639, 'unready': 103, 'cold_key': 32, 'high_spread': 12281, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 1.74 µs; tier agreement 0.47; drift alerts 3; mean |log2 error| 1.15; keys 5; parent fallbacks 0.
