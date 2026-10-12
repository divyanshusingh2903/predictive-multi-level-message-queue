# Production-flow benchmark (v2-prodflow-1)

Seeds per arm: 5, 5, 5, 5, 5, 5, 5 (fifo, static_tuned, static_misconfigured, shadow, predictive, predictive_producer, oracle). Values are medians across seeds; criteria use paired per-seed deltas with a bootstrap 95% interval.

| Arm | Mean ms | P50 ms | P95 ms | P99 ms | Short-class P50 ms | Export max wait s | Slowdown P50 / P99 | Throughput/s |
|---|---|---|---|---|---|---|---|---|
| fifo | 13073.4 | 15427.1 | 25957.9 | 27013.9 | 15462.4 | 25.86 | 834.2 / 8873.6 | 111.0 |
| static_tuned | 10957.2 | 10903.6 | 25563.2 | 28472.8 | 9600.7 | 29.37 | 604.3 / 7200.5 | 110.7 |
| static_misconfigured | 13930.1 | 15056.4 | 29075.1 | 33222.1 | 15505.7 | 24.73 | 889.8 / 11924.0 | 111.1 |
| shadow | 13132.9 | 15506.6 | 26025.6 | 27094.2 | 15520.6 | 25.92 | 838.2 / 8901.9 | 111.0 |
| predictive | 9593.6 | 9143.0 | 25152.8 | 27722.3 | 6235.5 | 26.64 | 496.4 / 5410.2 | 111.2 |
| predictive_producer | 11618.8 | 12909.2 | 24575.9 | 28068.3 | 12725.0 | 28.31 | 698.5 / 8001.8 | 111.0 |
| oracle | 9686.2 | 9487.1 | 24807.4 | 27589.8 | 6500.6 | 26.57 | 510.6 / 5301.9 | 111.3 |

## Pre-registered criteria

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| P1_mean_latency_vs_fifo | predictive vs fifo | -25.6% | [-29.4%, -21.8%] | **pass** |
| P2_short_class_p50_vs_fifo | predictive vs fifo | -57.7% | [-70.0%, -45.3%] | **pass** |
| G1_all_p99_vs_fifo | predictive vs fifo | 1.02× | [1.00, 1.05] | **pass** |
| G2_long_class_max_wait | predictive vs fifo | 0.99× | [0.95, 1.03] | **pass** |
| G3_no_lost_work | predictive vs – | – | – | **pass** |
| G4_throughput_vs_fifo | predictive vs fifo | 1.00× | [1.00, 1.00] | **pass** |
| S1_vs_static_tuned_noninferior | predictive vs static_tuned | -11.1% | [-13.4%, -8.7%] | **pass** |
| S2_beats_static_misconfigured | predictive vs static_misconfigured | -30.1% | [-34.3%, -26.0%] | **pass** |
| S3_shadow_overhead | shadow vs fifo | +0.1% | [-0.2%, +0.3%] | **pass** |
| S4_zero_config_vs_fifo | predictive_producer vs fifo | -10.5% | [-12.0%, -8.8%] | **pass** |

**Gate (P1_mean_latency_vs_fifo, P2_short_class_p50_vs_fifo, G1_all_p99_vs_fifo, G2_long_class_max_wait, G3_no_lost_work, G4_throughput_vs_fifo): PASSED.**

## Per-class P50 / P99 latency (ms, median across seeds)

| Class | fifo | static_tuned | static_misconfigured | shadow | predictive | predictive_producer | oracle |
|---|---|---|---|---|---|---|---|
| deliver_webhook | 15485.1 / 27295.3 | 14467.1 / 27424.6 | 13612.8 / 25170.2 | 15557.8 / 27372.7 | 16134.7 / 28364.7 | 18190.4 / 32708.2 | 15921.6 / 28130.1 |
| export_report | 18280.9 / 28159.0 | 21952.2 / 32067.4 | 16401.9 / 26701.6 | 18408.1 / 28192.4 | 18550.5 / 28915.8 | 19991.2 / 30379.8 | 18423.9 / 28781.1 |
| generate_invoice | 15456.2 / 26972.7 | 14594.3 / 27044.6 | 13587.9 / 24945.0 | 15513.7 / 27051.6 | 4957.3 / 17497.6 | 12775.1 / 25338.4 | 6086.7 / 22171.9 |
| push_notification | 15489.4 / 27006.0 | 9074.5 / 21713.0 | 19427.7 / 30181.4 | 15599.1 / 27087.8 | 10615.5 / 22599.4 | 12744.2 / 25328.5 | 9489.1 / 22777.6 |
| reindex_product | 14054.5 / 26647.6 | 8554.8 / 21730.9 | 23069.0 / 35292.3 | 14169.7 / 26722.6 | 4911.2 / 17489.3 | 12040.9 / 25087.7 | 4763.5 / 17323.4 |
| reserve_inventory | 15660.2 / 27000.0 | 9244.5 / 21773.4 | 13987.2 / 24903.5 | 15755.4 / 27083.9 | 5210.9 / 17578.8 | 12848.5 / 25335.5 | 6409.4 / 22215.5 |
| resize_image | 14179.4 / 26867.6 | 19097.9 / 32174.1 | 18072.4 / 30072.3 | 14290.5 / 26946.7 | 15244.3 / 27805.1 | 11890.4 / 25374.2 | 13392.0 / 27615.8 |
| send_order_email | 15533.0 / 27019.4 | 14634.8 / 27112.9 | 13845.7 / 24959.0 | 15639.4 / 27098.3 | 16324.6 / 27944.5 | 12809.0 / 25393.2 | 15812.9 / 27789.0 |
| send_sms | 15197.0 / 27067.9 | 14394.9 / 27165.5 | 19178.2 / 30158.2 | 15298.0 / 27154.1 | 15951.3 / 28005.0 | 12538.3 / 25346.6 | 15806.5 / 27871.8 |

## Mean latency by phase (ms, median across seeds)

| Phase | fifo | static_tuned | static_misconfigured | shadow | predictive | predictive_producer | oracle |
|---|---|---|---|---|---|---|---|
| steady | 91.9 | 70.9 | 142.6 | 91.5 | 67.1 | 63.9 | 61.9 |
| ramp | 744.0 | 470.0 | 1630.8 | 745.6 | 379.9 | 394.3 | 347.7 |
| flash_sale | 16597.8 | 13536.5 | 17472.3 | 16646.0 | 11635.8 | 14675.5 | 11688.7 |
| recovery | 22612.8 | 20150.9 | 23664.7 | 22697.9 | 18564.8 | 20934.4 | 18754.1 |
| shifted | 14366.6 | 11265.3 | 15666.3 | 14489.4 | 9757.7 | 12272.1 | 9958.2 |

## Predictor behaviour (predictive arm, median across seeds)

Lookups 34956, routed by prediction 34766; outcomes {'predicted': 34766, 'unready': 103, 'cold_key': 87, 'high_spread': 0, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 2.56 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.93; keys 9; parent fallbacks 0.

## Predictor behaviour (predictive_producer arm, median across seeds)

Lookups 34956, routed by prediction 24321; outcomes {'predicted': 24321, 'unready': 103, 'cold_key': 32, 'high_spread': 10594, 'censored': 0, 'stale_key': 0, 'invalid_key': 0}; lookup mean 2.47 µs; tier agreement 0.44; drift alerts 1; mean |log2 error| 1.16; keys 5; parent fallbacks 0.
