# Production-flow benchmark (v4-baselines-1)

Seeds per arm: 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10 (fifo, predictive, static_tuned, predictive_mean, predictive_p75, per_type_fair, predictive_noaging, ablate_decay, ablate_spread, ablate_stale, ablate_nmin). Values are medians across seeds; criteria use paired per-seed deltas with a bootstrap 95% interval.

| Arm | Mean ms | P50 ms | P95 ms | P99 ms | Short-class P50 ms | Export max wait s | Slowdown P50 / P99 | Throughput/s |
|---|---|---|---|---|---|---|---|---|
| fifo | 6927.7 | 4809.1 | 20279.2 | 21099.4 | 4965.1 | 20.79 | 239.6 / 6956.3 | 116.1 |
| predictive | 4779.7 | 657.1 | 17678.2 | 21753.9 | 39.3 | 21.70 | 23.8 / 3814.7 | 116.1 |
| static_tuned | 5607.7 | 1606.9 | 19142.9 | 21538.5 | 450.3 | 26.14 | 87.9 / 5657.5 | 116.1 |
| predictive_mean | 4896.9 | 854.0 | 17993.8 | 21643.1 | 42.0 | 21.61 | 34.9 / 3803.2 | 116.1 |
| predictive_p75 | 4904.5 | 859.0 | 18036.2 | 21682.6 | 41.3 | 21.64 | 35.0 / 3802.5 | 116.1 |
| per_type_fair | 2726.4 | 29.3 | 24510.6 | 31954.9 | 16.1 | 2.88 | 2.9 / 1183.5 | 116.1 |
| predictive_noaging | 2924.8 | 25.7 | 20642.0 | 24025.7 | 14.7 | 23.90 | 3.0 / 1343.3 | 116.1 |
| ablate_decay | 4676.1 | 596.6 | 18109.1 | 21925.3 | 38.5 | 21.91 | 19.6 / 3617.1 | 116.1 |
| ablate_spread | 4816.4 | 674.6 | 18140.0 | 21771.4 | 38.8 | 21.72 | 24.0 / 3781.9 | 116.1 |
| ablate_stale | 4794.2 | 661.9 | 17741.1 | 21782.2 | 38.9 | 21.72 | 23.8 / 3828.8 | 116.1 |
| ablate_nmin | 4777.5 | 644.2 | 17627.7 | 21672.3 | 39.0 | 21.63 | 24.4 / 3842.3 | 116.1 |

## Pre-registered criteria

| Criterion | Arm vs baseline | Point | 95% CI | Result |
|---|---|---|---|---|
| B1_predictive_vs_per_type_fair | predictive vs per_type_fair | +74.1% | [+64.5%, +83.9%] | **predictive worse** |
| B2_predictive_vs_static_tuned | predictive vs static_tuned | -15.5% | [-17.8%, -13.3%] | **predictive better** |
| B3_predictive_mean_vs_predictive | predictive_mean vs predictive | +3.6% | [+1.4%, +6.1%] | **variant worse** |
| B4_predictive_p75_vs_predictive | predictive_p75 vs predictive | +3.8% | [+1.6%, +6.3%] | **variant worse** |
| R_ptf_short_p50_vs_predictive | per_type_fair vs predictive | -60.1% | [-74.2%, -47.2%] | **reported** |
| R_ptf_p99_vs_predictive | per_type_fair vs predictive | +51.7% | [+45.9%, +58.1%] | **reported** |
| R_ptf_long_max_wait_vs_predictive | per_type_fair vs predictive | -83.3% | [-88.4%, -76.9%] | **reported** |
| R_mean_short_p50_vs_predictive | predictive_mean vs predictive | +36.5% | [-8.7%, +114.3%] | **reported** |
| R_mean_p99_vs_predictive | predictive_mean vs predictive | -0.5% | [-0.9%, -0.2%] | **reported** |
| R_mean_long_max_wait_vs_predictive | predictive_mean vs predictive | -0.7% | [-1.1%, -0.3%] | **reported** |
| R_p75_short_p50_vs_predictive | predictive_p75 vs predictive | +30.6% | [-8.7%, +95.9%] | **reported** |
| R_p75_p99_vs_predictive | predictive_p75 vs predictive | -0.4% | [-0.8%, -0.1%] | **reported** |
| R_p75_long_max_wait_vs_predictive | predictive_p75 vs predictive | -0.6% | [-1.0%, -0.2%] | **reported** |
| Q1_predictive_replicates_v2 | predictive vs fifo | -32.1% | [-34.4%, -29.6%] | **pass** |
| AB_decay_overall | ablate_decay vs predictive | +0.0% | [-2.1%, +2.3%] | **not supported here** |
| AB_decay_shifted | ablate_decay vs predictive | +1.0% | [-3.6%, +6.1%] | **inconclusive** |
| AB_spread_overall | ablate_spread vs predictive | +0.1% | [-0.4%, +0.6%] | **not supported here** |
| AB_spread_shifted | ablate_spread vs predictive | +1.0% | [-1.5%, +3.9%] | **inconclusive** |
| AB_stale_overall | ablate_stale vs predictive | +0.4% | [-0.2%, +1.1%] | **not supported here** |
| AB_stale_shifted | ablate_stale vs predictive | +2.1% | [-0.8%, +6.1%] | **inconclusive** |
| AB_nmin_overall | ablate_nmin vs predictive | +0.2% | [-1.1%, +2.0%] | **not supported here** |
| AB_nmin_shifted | ablate_nmin vs predictive | +1.7% | [-0.7%, +4.1%] | **inconclusive** |
| C1_noaging_vs_predictive | predictive_noaging vs predictive | -37.4% | [-39.8%, -35.1%] | **aging costs mean latency** |
| C2_noaging_mean_vs_fifo | predictive_noaging vs fifo | -57.6% | [-58.1%, -57.1%] | **reported** |
| C3_noaging_p99_vs_fifo | predictive_noaging vs fifo | +13.6% | [+12.6%, +14.6%] | **reported** |
| C4_noaging_long_max_wait_vs_fifo | predictive_noaging vs fifo | +13.6% | [+12.0%, +14.7%] | **reported** |
| C5_noaging_p99_vs_predictive | predictive_noaging vs predictive | +10.1% | [+9.6%, +10.5%] | **reported** |
| C6_noaging_long_max_wait_vs_predictive | predictive_noaging vs predictive | +12.2% | [+9.3%, +17.3%] | **reported** |

## Readings that combine criteria

| Group | Members | Reading |
|---|---|---|
| ablate_decay | AB_decay_overall, AB_decay_shifted | **inconclusive** |
| ablate_spread | AB_spread_overall, AB_spread_shifted | **inconclusive** |
| ablate_stale | AB_stale_overall, AB_stale_shifted | **inconclusive** |
| ablate_nmin | AB_nmin_overall, AB_nmin_shifted | **inconclusive** |
| C1_aging | C1_noaging_vs_predictive | **aging costs mean latency** |

**No gate: this configuration is descriptive.**

## Per-class P50 / P99 latency (ms, median across seeds)

| Class | fifo | predictive | static_tuned | predictive_mean | predictive_p75 | per_type_fair | predictive_noaging | ablate_decay | ablate_spread | ablate_stale | ablate_nmin |
|---|---|---|---|---|---|---|---|---|---|---|---|
| deliver_webhook | 4226.7 / 21271.0 | 4961.3 / 22139.3 | 3848.5 / 21167.1 | 4839.1 / 22013.8 | 4871.2 / 22055.4 | 8004.6 / 34550.4 | 5216.1 / 24398.0 | 4883.8 / 22311.1 | 4908.5 / 22156.7 | 4942.4 / 22169.5 | 4896.4 / 22072.4 |
| export_report | 3243.1 / 22371.4 | 2870.2 / 23152.6 | 6317.6 / 27768.7 | 2823.7 / 23065.7 | 2833.4 / 23101.3 | 1688.8 / 4333.6 | 2894.4 / 25350.8 | 2814.7 / 23313.7 | 2827.1 / 23177.0 | 2836.2 / 23188.1 | 3288.3 / 23081.7 |
| generate_invoice | 5657.5 / 20992.6 | 21.0 / 11634.1 | 5621.3 / 20907.1 | 18.6 / 11519.7 | 18.5 / 11560.2 | 11.2 / 130.3 | 10.3 / 74.2 | 20.8 / 11830.9 | 21.2 / 11653.3 | 21.2 / 11663.6 | 20.5 / 11547.0 |
| push_notification | 4998.7 / 21053.6 | 528.9 / 16664.7 | 68.0 / 15723.0 | 473.0 / 16556.1 | 481.3 / 16593.8 | 24.6 / 338.3 | 21.1 / 132.9 | 729.7 / 16855.1 | 527.9 / 16686.3 | 526.9 / 16696.0 | 543.4 / 16580.0 |
| reindex_product | 1745.9 / 21092.7 | 14.1 / 11595.0 | 26.7 / 15694.9 | 12.5 / 11485.5 | 12.2 / 11530.4 | 8.2 / 141.4 | 7.5 / 86.9 | 13.6 / 11773.3 | 13.9 / 11613.2 | 14.1 / 11624.4 | 13.0 / 11512.6 |
| reserve_inventory | 5909.5 / 21013.1 | 45.9 / 15050.1 | 139.2 / 15752.3 | 813.0 / 16559.3 | 823.9 / 16597.8 | 17.8 / 203.0 | 16.5 / 106.7 | 45.3 / 14114.9 | 47.1 / 14967.9 | 46.8 / 15104.8 | 67.0 / 16375.3 |
| resize_image | 2656.6 / 21062.7 | 1910.0 / 19281.1 | 6759.8 / 26239.6 | 2796.7 / 21817.9 | 2853.1 / 21853.8 | 33.3 / 375.6 | 368.6 / 24191.2 | 669.5 / 19961.3 | 1876.6 / 21448.1 | 1898.6 / 19388.1 | 1866.7 / 19691.4 |
| send_order_email | 5651.1 / 21127.5 | 6952.6 / 21968.2 | 5683.1 / 21040.6 | 6642.8 / 21881.5 | 6620.4 / 21916.1 | 4001.0 / 27618.2 | 7420.9 / 24248.6 | 6814.9 / 22145.9 | 6921.2 / 21988.3 | 7003.7 / 21998.1 | 6697.3 / 21904.0 |
| send_sms | 5042.0 / 21070.7 | 6175.1 / 21934.9 | 5052.3 / 21049.1 | 6018.6 / 21836.0 | 6045.0 / 21874.7 | 181.4 / 3714.3 | 6465.5 / 24218.8 | 6342.2 / 22119.6 | 6135.3 / 21955.5 | 6184.8 / 21967.5 | 5969.1 / 21858.0 |

## Mean latency by phase (ms, median across seeds)

| Phase | fifo | predictive | static_tuned | predictive_mean | predictive_p75 | per_type_fair | predictive_noaging | ablate_decay | ablate_spread | ablate_stale | ablate_nmin |
|---|---|---|---|---|---|---|---|---|---|---|---|
| steady | 53.5 | 44.0 | 49.1 | 44.5 | 44.4 | 43.6 | 44.0 | 44.0 | 44.0 | 44.1 | 44.0 |
| ramp | 324.0 | 172.3 | 252.4 | 176.3 | 177.3 | 155.9 | 178.8 | 177.2 | 171.4 | 171.3 | 164.0 |
| flash_sale | 11342.7 | 6885.7 | 8880.7 | 7528.3 | 7515.5 | 5149.6 | 4850.8 | 7152.4 | 6866.5 | 6905.0 | 7089.3 |
| recovery | 14466.5 | 11113.7 | 12587.6 | 11408.5 | 11446.1 | 4544.4 | 5826.9 | 10754.1 | 11135.5 | 11148.7 | 11435.8 |
| shifted | 1762.7 | 839.3 | 1081.9 | 843.1 | 850.4 | 401.4 | 707.8 | 828.3 | 832.1 | 836.1 | 851.2 |

## Predictor behaviour (predictive arm, median across seeds)

Lookups 34984, routed by prediction 34800; outcomes {'predicted': 34799.5, 'unready': 102.5, 'cold_key': 81.0, 'high_spread': 0.0, 'censored': 0.0, 'stale_key': 0.0, 'invalid_key': 0.0}; lookup mean 1.64 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.65; keys 9; parent fallbacks 0.

## Predictor behaviour (predictive_mean arm, median across seeds)

Lookups 34984, routed by prediction 34800; outcomes {'predicted': 34799.5, 'unready': 102.5, 'cold_key': 81.0, 'high_spread': 0.0, 'censored': 0.0, 'stale_key': 0.0, 'invalid_key': 0.0}; lookup mean 1.64 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.67; keys 9; parent fallbacks 0.

## Predictor behaviour (predictive_p75 arm, median across seeds)

Lookups 34984, routed by prediction 34800; outcomes {'predicted': 34799.5, 'unready': 102.5, 'cold_key': 81.0, 'high_spread': 0.0, 'censored': 0.0, 'stale_key': 0.0, 'invalid_key': 0.0}; lookup mean 1.78 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.78; keys 9; parent fallbacks 0.

## Predictor behaviour (predictive_noaging arm, median across seeds)

Lookups 34984, routed by prediction 34800; outcomes {'predicted': 34799.5, 'unready': 102.5, 'cold_key': 81.0, 'high_spread': 0.0, 'censored': 0.0, 'stale_key': 0.0, 'invalid_key': 0.0}; lookup mean 1.68 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.65; keys 9; parent fallbacks 0.

## Predictor behaviour (ablate_decay arm, median across seeds)

Lookups 34984, routed by prediction 34800; outcomes {'predicted': 34799.5, 'unready': 102.5, 'cold_key': 81.0, 'high_spread': 0.0, 'censored': 0.0, 'stale_key': 0.0, 'invalid_key': 0.0}; lookup mean 1.71 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.66; keys 9; parent fallbacks 0.

## Predictor behaviour (ablate_spread arm, median across seeds)

Lookups 34984, routed by prediction 34800; outcomes {'predicted': 34799.5, 'unready': 102.5, 'cold_key': 81.0, 'high_spread': 0.0, 'censored': 0.0, 'stale_key': 0.0, 'invalid_key': 0.0}; lookup mean 1.68 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.65; keys 9; parent fallbacks 0.

## Predictor behaviour (ablate_stale arm, median across seeds)

Lookups 34984, routed by prediction 34800; outcomes {'predicted': 34799.5, 'unready': 102.5, 'cold_key': 81.0, 'high_spread': 0.0, 'censored': 0.0, 'stale_key': 0.0, 'invalid_key': 0.0}; lookup mean 1.66 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.65; keys 9; parent fallbacks 0.

## Predictor behaviour (ablate_nmin arm, median across seeds)

Lookups 34984, routed by prediction 34882; outcomes {'predicted': 34882.5, 'unready': 102.5, 'cold_key': 1.0, 'high_spread': 0.0, 'censored': 0.0, 'stale_key': 0.0, 'invalid_key': 0.0}; lookup mean 1.68 µs; tier agreement 0.80; drift alerts 0; mean |log2 error| 0.65; keys 9; parent fallbacks 0.
