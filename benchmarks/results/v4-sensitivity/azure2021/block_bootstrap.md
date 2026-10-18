# Block-bootstrap re-estimation (v4-sensitivity-1-w28)

28 windows; moving blocks of 1, 2, 4 windows, 10000 resamples. Block 4 is primary.

- A_P1_mean_latency_vs_fifo @ 0.50; block 1: -1.0% [-1.9%, -0.3%] fail; block 2: -1.0% [-1.9%, -0.2%] fail; block 4: -1.0% [-2.0%, -0.3%] fail
- B_P1_mean_latency_vs_fifo @ 0.50; block 1: -8.4% [-13.1%, -4.3%] fail; block 2: -8.4% [-12.9%, -4.0%] fail; block 4: -8.4% [-12.1%, -4.4%] fail
- A_P1_mean_latency_vs_fifo @ 0.80; block 1: -1.6% [-2.8%, -0.7%] fail; block 2: -1.6% [-3.1%, -0.6%] fail; block 4: -1.6% [-3.1%, -0.6%] fail
- B_P1_mean_latency_vs_fifo @ 0.80; block 1: -15.8% [-24.1%, -8.6%] pass; block 2: -15.8% [-24.8%, -8.4%] pass; block 4: -15.8% [-25.1%, -9.1%] pass
- A_P1_mean_latency_vs_fifo @ 0.95; block 1: -2.3% [-3.8%, -1.1%] fail; block 2: -2.3% [-4.0%, -0.9%] fail; block 4: -2.3% [-4.0%, -1.1%] fail
- B_P1_mean_latency_vs_fifo @ 0.95; block 1: -17.9% [-26.6%, -10.2%] pass; block 2: -17.9% [-27.2%, -10.3%] pass; block 4: -17.9% [-27.5%, -11.7%] pass

Cells whose verdict differs from v3-traces-1 (block 4):
- load 0.50 gate B_overload_design: pass -> fail

