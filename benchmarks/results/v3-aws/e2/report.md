# Production-flow benchmark (v3-cloud-e2-1)

Seeds per arm: 2, 2, 2, 2 (fifo, static_tuned, shadow, predictive). Values are medians across seeds; criteria use paired per-seed deltas with a bootstrap 95% interval.

| Arm | Mean ms | P50 ms | P95 ms | P99 ms | Short-class P50 ms | Export max wait s | Slowdown P50 / P99 | Throughput/s |
|---|---|---|---|---|---|---|---|---|
| fifo | 42818.2 | 28772.2 | 123767.3 | 132211.5 | 29751.8 | 133.05 | 1483.0 / 44257.0 | 116.6 |
| static_tuned | 41012.2 | 25730.0 | 120866.8 | 130773.7 | 24516.4 | 138.44 | 1288.9 / 42294.9 | 116.6 |
| shadow | 42717.8 | 28704.7 | 123619.3 | 132034.1 | 29588.1 | 132.88 | 1472.1 / 44206.6 | 116.6 |
| predictive | 39926.9 | 24013.9 | 118918.4 | 129316.9 | 22196.2 | 134.14 | 1195.1 / 40333.7 | 116.6 |

## Pre-registered criteria

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| C1_mean_vs_fifo | predictive vs fifo | -6.1%, -7.4% | each seed | **fail** |
| C2_short_p50_vs_fifo | predictive vs fifo | -19.7%, -31.8% | each seed | **fail** |
| C3_p99_vs_fifo | predictive vs fifo | -1.9%, -2.5% | each seed | **pass** |
| C4_export_max_wait_vs_fifo | predictive vs fifo | +1.2%, +0.5% | each seed | **pass** |
| C5_shifted_phase_mean_vs_fifo | predictive vs fifo | -14.7%, -23.1% | each seed | **fail** |
| C6_vs_static_tuned | predictive vs static_tuned | -2.2%, -3.1% | each seed | **pass** |
| C7_shadow_overhead | shadow vs fifo | -0.3%, -0.2% | each seed | **pass** |
| C8_throughput_vs_fifo | predictive vs fifo | -0.0%, -0.0% | each seed | **pass** |
| G3_no_lost_work | predictive vs – | – | – | **pass** |
| G3b_no_lost_work_fifo | fifo vs – | – | – | **pass** |
| G3c_no_lost_work_static_tuned | static_tuned vs – | – | – | **pass** |
| G3d_no_lost_work_shadow | shadow vs – | – | – | **pass** |

**No gate: this configuration is descriptive.**

## Per-class P50 / P99 latency (ms, median across seeds)

| Class | fifo | static_tuned | shadow | predictive |
|---|---|---|---|---|
| deliver_webhook | 24471.8 / 133426.0 | 23970.6 / 133047.5 | 24291.6 / 133265.0 | 25046.3 / 134175.5 |
| export_report | 11969.1 / 131355.8 | 14986.8 / 136420.7 | 11851.6 / 131203.0 | 11516.2 / 131772.9 |
| generate_invoice | 33740.9 / 132048.4 | 33960.6 / 132098.8 | 33644.9 / 131872.1 | 23593.9 / 121614.1 |
| push_notification | 30257.5 / 131900.8 | 24084.7 / 126371.7 | 30132.7 / 131734.4 | 25006.7 / 127176.1 |
| reindex_product | 12071.4 / 131544.3 | 5792.0 / 126304.2 | 11742.3 / 131375.4 | 3061.5 / 121967.1 |
| reserve_inventory | 33707.4 / 132211.8 | 27981.8 / 126436.9 | 33624.1 / 132039.2 | 28146.5 / 124243.9 |
| resize_image | 16759.7 / 131903.3 | 20704.1 / 137422.2 | 16440.1 / 131737.2 | 15270.1 / 129888.3 |
| send_order_email | 33245.5 / 132069.5 | 33173.2 / 132175.9 | 33059.4 / 131896.6 | 34387.1 / 133049.9 |
| send_sms | 31773.5 / 132181.5 | 31952.3 / 132199.2 | 31685.0 / 132009.0 | 32863.9 / 133157.6 |

## Mean latency by phase (ms, median across seeds)

| Phase | fifo | static_tuned | shadow | predictive |
|---|---|---|---|---|
| steady | 59.2 | 50.3 | 59.2 | 45.8 |
| ramp | 471.1 | 284.1 | 465.9 | 215.4 |
| flash_sale | 67719.6 | 64867.0 | 67733.0 | 62897.0 |
| recovery | 91573.2 | 89178.2 | 91274.3 | 87677.7 |
| shifted | 12593.1 | 10907.9 | 12366.9 | 10324.7 |

## Predictor behaviour (predictive arm, median across seeds)

Lookups 209946, routed by prediction 209763; outcomes {'predicted': 209763.0, 'unready': 104.5, 'cold_key': 79.0, 'high_spread': 0.0, 'censored': 0.0, 'stale_key': 0.0, 'invalid_key': 0.0}; lookup mean 1.48 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.67; keys 9; parent fallbacks 0.
