# Parameter sensitivity (v4-sensitivity-1, huawei2025-r5)

18 variants at load 0.8; default variant `default`.

| Variant | Parameter | Value | A P1 | B P1 |
|---|---|---|---|---|
| default | default | all defaults | -15.0% [-27.0%, -4.2%] pass | -46.5% [-78.3%, -13.3%] pass |
| n_min-5 | n_min | 5 | -14.1% [-25.1%, -4.2%] fail | -46.5% [-78.3%, -13.3%] pass |
| n_min-100 | n_min | 100 | -14.7% [-26.6%, -4.1%] fail | -46.1% [-77.8%, -13.2%] pass |
| kappa-4 | kappa | 4 | -11.8% [-20.9%, -3.7%] fail | -42.5% [-74.7%, -13.0%] pass |
| kappa-64 | kappa | 64 | -15.3% [-27.6%, -4.2%] pass | -46.4% [-77.9%, -13.2%] pass |
| kappa-inf | kappa | inf | -15.9% [-28.6%, -4.3%] pass | -46.7% [-77.8%, -13.2%] pass |
| decay-0.98 | decay | 0.98 | -12.8% [-22.4%, -4.2%] fail | -43.4% [-74.0%, -13.0%] pass |
| decay-0.999 | decay | 0.999 | -12.8% [-22.9%, -3.4%] fail | -43.3% [-77.9%, -13.0%] pass |
| decay-1.0 | decay | 1.0 | -10.6% [-17.9%, -3.0%] fail | -36.8% [-66.1%, -10.3%] pass |
| tau-5min | tau | 5min | -15.1% [-27.3%, -4.2%] pass | -46.5% [-78.2%, -13.2%] pass |
| tau-2h | tau | 2h | -15.0% [-27.0%, -4.2%] pass | -46.5% [-78.3%, -13.3%] pass |
| tau-off | tau | off | -15.0% [-27.0%, -4.2%] pass | -46.5% [-78.3%, -13.3%] pass |
| aging_threshold-1s | aging_threshold | 1s | -3.3% [-6.0%, -0.9%] fail | -45.4% [-77.0%, -13.2%] pass |
| aging_threshold-30s | aging_threshold | 30s | -40.7% [-67.8%, -13.1%] pass | -45.1% [-78.3%, -13.0%] pass |
| weights-4-2-1 | weights | 4-2-1 | -15.0% [-27.0%, -4.2%] pass | -46.2% [-78.2%, -13.2%] pass |
| weights-16-4-1 | weights | 16-4-1 | -15.0% [-27.0%, -4.2%] pass | -46.8% [-78.4%, -13.3%] pass |
| levels-2 | levels | 2 | -4.4% [-8.6%, -0.8%] fail | -18.6% [-38.3%, -4.1%] pass |
| levels-4 | levels | 4 | -18.3% [-33.1%, -4.8%] pass | -48.0% [-78.3%, -13.3%] pass |

Variants passing P1: {'A_P1_mean_latency_vs_fifo': 10, 'B_P1_mean_latency_vs_fifo': 18}

**A_P1_mean_latency_vs_fifo**
- n_min: sensitive (R2), flips at 5, 100; default on a cliff (R3)
- kappa: sensitive (R2), flips at 4; default on a cliff (R3)
- decay: sensitive (R2), flips at 0.98, 0.999, 1.0; default on a cliff (R3)
- tau: robust (R1)
- aging_threshold: sensitive (R2), flips at 1s; default on a cliff (R3)
- weights: robust (R1)
- levels: sensitive (R2), flips at 2; default on a cliff (R3)

**B_P1_mean_latency_vs_fifo**
- n_min: robust (R1)
- kappa: robust (R1)
- decay: robust (R1)
- tau: robust (R1)
- aging_threshold: robust (R1)
- weights: robust (R1)
- levels: robust (R1)

