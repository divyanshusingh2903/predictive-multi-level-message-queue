# Production-flow benchmark (v2-prodflow-1)

Seeds per arm: 5, 5, 5, 5, 5, 5, 5 (fifo, static_tuned, static_misconfigured, shadow, predictive, predictive_producer, oracle). Values are medians across seeds; criteria use paired per-seed deltas with a bootstrap 95% interval.

| Arm | Mean ms | P50 ms | P95 ms | P99 ms | Short-class P50 ms | Export max wait s | Slowdown P50 / P99 | Throughput/s |
|---|---|---|---|---|---|---|---|---|
| fifo | 8030.9 | 6032.8 | 21219.8 | 22287.5 | 6033.1 | 20.57 | 349.6 / 7370.5 | 115.9 |
| static_tuned | 6954.5 | 3702.1 | 21502.8 | 23933.0 | 1643.3 | 24.89 | 155.0 / 6300.1 | 115.8 |
| static_misconfigured | 8689.7 | 7498.9 | 24017.2 | 27567.7 | 7940.1 | 19.57 | 386.7 / 10684.6 | 115.9 |
| shadow | 7971.3 | 5915.9 | 21136.0 | 22277.0 | 5990.8 | 20.59 | 338.2 / 7362.5 | 115.9 |
| predictive | 6156.9 | 2133.7 | 20453.7 | 23403.9 | 97.7 | 21.79 | 97.6 / 4442.3 | 115.8 |
| predictive_producer | 6908.9 | 3951.0 | 19744.2 | 22947.9 | 3449.6 | 22.68 | 184.8 / 6553.8 | 115.8 |
| oracle | 6644.8 | 3662.9 | 21440.9 | 24511.0 | 1048.1 | 23.16 | 159.2 / 4295.6 | 115.9 |

## Pre-registered criteria

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| P1_mean_latency_vs_fifo | predictive vs fifo | -27.9% | [-31.9%, -24.7%] | **pass** |
| P2_short_class_p50_vs_fifo | predictive vs fifo | -88.4% | [-99.0%, -72.6%] | **pass** |
| G1_all_p99_vs_fifo | predictive vs fifo | 1.04× | [1.02, 1.06] | **pass** |
| G2_long_class_max_wait | predictive vs fifo | 1.02× | [0.96, 1.05] | **pass** |
| G3_no_lost_work | predictive vs – | – | – | **pass** |
| G4_throughput_vs_fifo | predictive vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| S1_vs_static_tuned_noninferior | predictive vs static_tuned | -10.9% | [-14.6%, -8.1%] | **pass** |
| S2_beats_static_misconfigured | predictive vs static_misconfigured | -33.8% | [-38.1%, -30.1%] | **pass** |
| S3_shadow_overhead | shadow vs fifo | -0.6% | [-1.2%, +0.0%] | **pass** |
| S4_zero_config_vs_fifo | predictive_producer vs fifo | -15.4% | [-18.0%, -13.2%] | **pass** |

**Gate (P1_mean_latency_vs_fifo, P2_short_class_p50_vs_fifo, G1_all_p99_vs_fifo, G2_long_class_max_wait, G3_no_lost_work, G4_throughput_vs_fifo): PASSED.**

## Per-class P50 / P99 latency (ms, median across seeds)

| Class | fifo | static_tuned | static_misconfigured | shadow | predictive | predictive_producer | oracle |
|---|---|---|---|---|---|---|---|
| deliver_webhook | 5562.2 / 22550.8 | 5684.4 / 23202.1 | 4327.7 / 20431.6 | 5350.0 / 22539.6 | 6929.7 / 23722.7 | 8496.6 / 26300.0 | 8930.1 / 24874.4 |
| export_report | 6125.4 / 22521.6 | 10735.8 / 26978.7 | 5419.0 / 20960.2 | 5893.4 / 22247.7 | 6671.1 / 22704.5 | 7555.7 / 23658.8 | 9966.4 / 24369.0 |
| generate_invoice | 6770.8 / 22251.2 | 7276.6 / 22994.8 | 5211.2 / 20308.2 | 6581.4 / 22240.3 | 27.0 / 13139.4 | 4409.3 / 20738.4 | 58.4 / 18560.1 |
| push_notification | 6332.1 / 22294.2 | 555.7 / 17652.9 | 11011.3 / 25521.9 | 6265.6 / 22293.6 | 1734.4 / 18259.0 | 3680.7 / 20709.6 | 3395.1 / 19815.6 |
| reindex_product | 4493.0 / 21987.6 | 64.3 / 17701.4 | 13868.9 / 30530.3 | 4279.9 / 21972.1 | 18.7 / 13132.7 | 1956.2 / 20504.9 | 21.8 / 14244.6 |
| reserve_inventory | 7193.5 / 22288.4 | 974.1 / 17690.9 | 4928.4 / 20286.8 | 7091.2 / 22277.0 | 1927.9 / 17408.1 | 4662.8 / 20706.0 | 1296.1 / 19198.7 |
| resize_image | 4745.5 / 22208.4 | 10477.3 / 28089.8 | 8991.8 / 25407.7 | 4623.0 / 22193.9 | 6091.0 / 22265.3 | 2285.7 / 20669.0 | 5746.6 / 24515.5 |
| send_order_email | 7111.8 / 22315.6 | 7482.4 / 23021.9 | 5162.9 / 20343.8 | 6975.3 / 22304.6 | 8769.8 / 23590.2 | 4524.1 / 20781.0 | 9611.4 / 24687.1 |
| send_sms | 7433.0 / 22403.5 | 7064.9 / 23101.3 | 11637.0 / 25550.9 | 7300.9 / 22391.6 | 8963.5 / 23674.6 | 4471.2 / 20685.1 | 9369.6 / 24774.6 |

## Mean latency by phase (ms, median across seeds)

| Phase | fifo | static_tuned | static_misconfigured | shadow | predictive | predictive_producer | oracle |
|---|---|---|---|---|---|---|---|
| steady | 55.2 | 47.4 | 69.2 | 55.6 | 44.6 | 45.8 | 43.4 |
| ramp | 279.5 | 198.0 | 637.7 | 285.5 | 149.4 | 167.6 | 163.2 |
| flash_sale | 13459.2 | 10707.7 | 14324.3 | 13265.3 | 9686.9 | 11457.3 | 8864.7 |
| recovery | 15506.8 | 14292.9 | 16517.1 | 15453.7 | 13214.9 | 14245.6 | 14331.6 |
| shifted | 4841.7 | 2830.0 | 6559.4 | 4600.1 | 2590.3 | 2888.1 | 2788.6 |

## Predictor behaviour (predictive arm, median across seeds)

Lookups 34956, routed by prediction 34767; outcomes {'predicted': 34767, 'unready': 103, 'cold_key': 86, 'high_spread': 0, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 2.74 µs; tier agreement 0.80; drift alerts 0.
