# Parameter sensitivity (v4-sensitivity-1, azure2021)

18 variants at load 0.8; default variant `default`.

| Variant | Parameter | Value | A P1 | B P1 |
|---|---|---|---|---|
| default | default | all defaults | -0.9% [-2.0%, -0.1%] fail | -22.8% [-37.2%, -9.3%] pass |
| n_min-5 | n_min | 5 | -1.0% [-2.2%, -0.2%] fail | -24.6% [-39.4%, -10.3%] pass |
| n_min-100 | n_min | 100 | -0.8% [-2.0%, -0.1%] fail | -13.7% [-21.8%, -5.7%] fail |
| kappa-4 | kappa | 4 | -0.5% [-1.0%, -0.1%] fail | -17.4% [-29.8%, -6.1%] pass |
| kappa-64 | kappa | 64 | -1.0% [-2.3%, -0.2%] fail | -23.3% [-38.1%, -9.3%] pass |
| kappa-inf | kappa | inf | -1.2% [-2.6%, -0.2%] fail | -24.0% [-39.3%, -9.7%] pass |
| decay-0.98 | decay | 0.98 | -0.8% [-2.0%, -0.1%] fail | -21.8% [-35.4%, -9.0%] pass |
| decay-0.999 | decay | 0.999 | -0.9% [-2.1%, -0.1%] fail | -22.8% [-36.9%, -9.5%] pass |
| decay-1.0 | decay | 1.0 | -0.9% [-2.1%, -0.1%] fail | -23.0% [-36.8%, -9.9%] pass |
| tau-5min | tau | 5min | -0.8% [-1.9%, -0.1%] fail | -21.1% [-34.6%, -8.3%] pass |
| tau-2h | tau | 2h | -0.9% [-2.1%, -0.1%] fail | -24.2% [-41.0%, -9.4%] pass |
| tau-off | tau | off | -1.1% [-2.3%, -0.2%] fail | -26.2% [-43.9%, -10.2%] pass |
| aging_threshold-1s | aging_threshold | 1s | -0.2% [-0.5%, -0.0%] fail | -22.5% [-36.3%, -9.4%] pass |
| aging_threshold-30s | aging_threshold | 30s | -3.4% [-6.8%, -0.8%] fail | -22.0% [-36.6%, -8.5%] pass |
| weights-4-2-1 | weights | 4-2-1 | -0.9% [-2.0%, -0.1%] fail | -22.0% [-36.4%, -8.7%] pass |
| weights-16-4-1 | weights | 16-4-1 | -0.9% [-2.0%, -0.1%] fail | -23.1% [-37.2%, -9.8%] pass |
| levels-2 | levels | 2 | -0.7% [-1.6%, -0.1%] fail | -10.1% [-15.9%, -4.6%] fail |
| levels-4 | levels | 4 | -1.1% [-2.7%, -0.0%] fail | -24.3% [-38.5%, -11.4%] pass |

Variants passing P1: {'A_P1_mean_latency_vs_fifo': 0, 'B_P1_mean_latency_vs_fifo': 16}

**A_P1_mean_latency_vs_fifo**
- n_min: robust (R1)
- kappa: robust (R1)
- decay: robust (R1)
- tau: robust (R1)
- aging_threshold: robust (R1)
- weights: robust (R1)
- levels: robust (R1)

**B_P1_mean_latency_vs_fifo**
- n_min: sensitive (R2), flips at 100; default on a cliff (R3)
- kappa: robust (R1)
- decay: robust (R1)
- tau: robust (R1)
- aging_threshold: robust (R1)
- weights: robust (R1)
- levels: sensitive (R2), flips at 2; default on a cliff (R3)

