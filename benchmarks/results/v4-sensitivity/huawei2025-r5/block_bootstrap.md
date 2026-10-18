# Block-bootstrap re-estimation (v4-sensitivity-1-w28)

28 windows; moving blocks of 1, 2, 4 windows, 10000 resamples. Block 4 is primary.

- A_P1_mean_latency_vs_fifo @ 0.50; block 1: -2.5% [-6.8%, -0.2%] fail; block 2: -2.5% [-4.9%, -0.1%] fail; block 4: -2.5% [-4.6%, -0.0%] fail
- B_P1_mean_latency_vs_fifo @ 0.50; block 1: -2.3% [-6.2%, -0.2%] fail; block 2: -2.3% [-4.4%, -0.0%] fail; block 4: -2.3% [-4.2%, -0.0%] fail
- A_P1_mean_latency_vs_fifo @ 0.80; block 1: -11.6% [-18.2%, -6.0%] fail; block 2: -11.6% [-18.1%, -3.8%] fail; block 4: -11.6% [-16.5%, -1.7%] fail
- B_P1_mean_latency_vs_fifo @ 0.80; block 1: -36.8% [-52.8%, -21.5%] pass; block 2: -36.8% [-58.8%, -13.7%] pass; block 4: -36.8% [-65.5%, -4.1%] pass
- A_P1_mean_latency_vs_fifo @ 0.95; block 1: -8.6% [-12.9%, -5.1%] fail; block 2: -8.6% [-12.5%, -4.2%] fail; block 4: -8.6% [-11.8%, -3.2%] fail
- B_P1_mean_latency_vs_fifo @ 0.95; block 1: -46.0% [-61.4%, -30.8%] pass; block 2: -46.0% [-66.7%, -24.3%] pass; block 4: -46.0% [-74.0%, -16.9%] pass

Cells whose verdict differs from v3-traces-1 (block 4):
- load 0.80 gate A_predictive_routing: pass -> fail

