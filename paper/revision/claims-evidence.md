# Claims to evidence (Phase 0 of the paper revision)

Working file, not part of the paper build. Every headline claim in the abstract, the experiment behind it, its
pre-registration, where the numbers come from, and what to watch. Numbers are from `benchmarks/results/**/summary.json`
via `paper/gen_numbers.py`. "CI" is the 95% bootstrap interval over five seeds (or seven windows for traces).

| # | Claim (abstract) | Experiment | Pre-registration | Result file | Evidence strength / watch |
|---|---|---|---|---|---|
| 1 | Predictive routing cuts mean latency vs FIFO on AWS (-30.8%) | E1, production flow on AWS | `v3-cloud-preregistration.md` (replicates `v2-preregistration.md`) | `v3-aws/e1` | CI [-35.3, -27.1], excludes 0. Synthetic workload designed by the author, keys near-deterministic. |
| 2 | Short-job median cut (-94.8%) | E1 | same | `v3-aws/e1` | CI [-99.0, -86.5]. Short class is the cheapest tier; the oracle is near the predictor, so the ceiling is easy. |
| 3 | All-message P99 at 1.03x of FIFO | E1 guardrail G1 | same | `v3-aws/e1` | Ratio 1.034, CI [1.018, 1.05]: slightly WORSE than FIFO and the CI excludes 1. The abstract says this plainly; keep it. |
| 4 | Beats hand-tuned static map (-14.4%) | E1 criterion S1 | same | `v3-aws/e1` | Pre-registered as non-inferiority, but the CI [-18.4, -10.7] supports "beats". |
| 5 | Sidecar on RabbitMQ cuts RabbitMQ mean latency (-57.6%) | E3 criterion S3 | `v3-brokers-preregistration.md` | `v3-aws/e3` | CI [-58.6, -56.8]. Larger than the gain inside Harbinger; explanation (aging) is NOT measured. |
| 6 | Mid-cost jobs get worse at the median (+29% to +45%) | E1 per-class | descriptive | `v3-aws/e1` per-class | Wide whiskers over 5 seeds; direction only. |
| 7 | Default aging erases the order under hours-long backlogs | 30-min run (E2); Azure/Huawei gate A | `v3-cloud`, `v3-traces` | `v3-aws/e2`, `v3-traces*` | E2: 3 of 8 checks failed, 2 repetitions, no CIs. Gate A passed 1 of 6 cells. Mechanism supported by the oracle also not helping, not by an aging ablation. |
| 8 | Weighted + pausing aging cuts mean latency on Azure (-22.8% to -25.3%) and Huawei (-46.5% to -49.9%) at loads 0.8-0.95 | Gate B | `v3-aging`, `v3-traces` | `v3-traces`, `v3-aging` | Gate B passed 5 of 6 cells. Azure is IN-SAMPLE for the design (see `tab:insample`). Huawei is out of sample. |
| 9 | On one research log it raised mean latency (+88.8%, CI includes 0) | DAS2-fs2 gate B | `v3-traces-2` | `v3-traces-2` | Seven windows, autocorrelated; CI likely too narrow. Diagnosis after the fact, not pre-registered. |
| 10 | Keys that rarely recur give the predictor little to learn from | LPC-EGEE | `v3-traces-2` | `v3-traces-2` | Prediction coverage numbers are descriptive. |
| 11 | All evidence synthetic or trace-driven, one region; no tail or production claim | whole paper | n/a | n/a | Consistent with limitations. |

## Claims in the body that need care

- "Zero configuration" was softened to "no per-endpoint configuration" (title, abstract, intro, related work). Parameters are in `tab:params`; none was swept.
- "Beats Redis/RabbitMQ FIFO" (E3 P1/P3, -21.7% / -25.5%) is partly Harbinger's own FIFO being 11% slower than RabbitMQ's (S1 equivalence missed), so the margin includes a broker overhead effect.
- The Cobham analysis is correct (algebra checked) but classical; it explains, it does not test.
- "Oracle is near the predictor" is a property of the synthetic workload, not evidence about real queues.

## Gaps still open (Phase 2+)

- Simulator validation (`v4-simvalidation-1`, 2026-10-14): ran, gate FAILED on V1 (effect size, predictive 11.4/11.8 points off a 10-point limit); V2 and V3 passed. Reported in the limitations; the simulator overstates reductions by about 40% on this workload.
- No ablation of predictor components and no parameter sensitivity.
- No per-type fair-queuing or simple-statistic baselines on the production flow.
- Aging is not isolated in the RabbitMQ comparison.
- Trace statistics treat adjacent windows as independent.


## Updates after the v4 experiments (2026-10-18)

Results directories: `v4-baselines`, `v4-sensitivity`, `v4-replay`, `v4-replay-aging` (each with a README stating its conclusion). Claims 7 to 11 above are superseded or extended as follows.

| # | Claim now in the paper | Experiment | Result file | Watch |
|---|---|---|---|---|
| 7 | Default aging erases the order under backlogs; switching it off restores it | `v4-baselines` C1 to C6, `v4-replay-aging-1` | `v4-baselines/summary.json`, `v4-replay-aging/replay_summary.json` | Cells last 5 to 25 minutes, so starvation over hours is not tested; the aging replay ran on smaller hosts and carries its own fifo and predictive arms |
| 8 | Weights and pausing aging pass gate B at loads 0.8 and 0.95 on both replayed traces, fail at 0.5 | `v4-replay-1` | `v4-replay/{azure2021,huawei2025-r5}/replay_summary.json` | Azure ran once per cell and its load-0.8 cell passes by 0.3 points; handlers sleep (no CPU contention) |
| 9 | Predictive routing as built fails gate A in all six replayed cells | `v4-replay-1` | same | Gate A passed one cell in the simulator with seven windows; 28 windows and the replay agree on none |
| 12 | The simulator matches the real broker to about a point on both traces, though it overstated the production flow by about 40% | `v4-replay-1` against `v3-traces` simulation; `v4-simvalidation-1` | `v4-replay/*/replay_summary.json` (`simulator_comparison`) | Agreement tests queueing, not CPU contention |
| 13 | A queue per job type with no prediction beats the predictor on mean latency on the production flow, at a worse P99 | `v4-baselines` B1 | `v4-baselines/summary.json` | One workload, that design's best case; not testable on traces (hundreds of keys) |
| 14 | A static map from history matches or beats the live predictor on Huawei | `v4-replay-1` S2 | `v4-replay/huawei2025-r5/replay_summary.json` | Huawei keys are stable within a day |
| 15 | No predictor component shows a benefit on the production flow | `v4-baselines` ablation | `v4-baselines/summary.json` | Shifted-phase intervals inconclusive; this workload cannot show what the components are for |
| 16 | Gate B is robust to every parameter on Huawei and all but two on Azure; gate A as built is fragile; the aging threshold dominates | `v4-sensitivity-1` | `v4-sensitivity/*/sim/sensitivity.json` | Simulator only; stage 2 (real-broker confirmation) was not run |

Open and disclosed: two Huawei cells lost the broker and were re-run once (cause unknown); the HPC logs are simulated, not replayed; the paper's release tag still names `v0.1.1-preprint`.
