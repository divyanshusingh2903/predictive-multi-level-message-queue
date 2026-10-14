# Simulator validation (`v4-simvalidation-1`)

Pre-registered in [docs/v4-simvalidation-preregistration.md](../../../docs/v4-simvalidation-preregistration.md) and
frozen in commit `cdcc20b`, before any registered run. The run used that commit and
`python benchmarks/sim_validation.py` with the two measured sets named in the pre-registration. The configuration is
[`benchmarks/configs/v4-simvalidation.json`](../../configs/v4-simvalidation.json); the analysis code and its
synthetic-run tests were committed with the freeze, and no change was made after the result.

**Conclusion: the gate failed in both sets.** V1 (the size of the effect against FIFO) failed; V2 (direction in each
phase) and V3 (group order) passed.

| | Local | AWS |
|---|---|---|
| V1 effect agreement (limit 10 points) | fail: predictive 11.4, oracle 12.9 | fail: predictive 11.8, oracle 13.1 |
| V2 phase direction | pass, 5 of 5 phases | pass, 5 of 5 phases |
| V3 group order | pass, 5 of 5 seeds | pass, 5 of 5 seeds |

The simulator reproduces the direction of every effect and the order of the policies, but it overstates the
mean-latency reduction of every reordering policy; for predictive routing it reports about 40% more reduction than the
real broker achieved. Its absolute latencies are 0.31 to 0.41 times the measured ones. It was not tuned or changed in
response.

| File | Content |
|---|---|
| `report.md` | V1 to V5 for each measured set |
| `summary.json` | the same, machine-readable, including each seed's ordering |

**Inputs.** The raw `messages.tsv` and `run.json` files of the local re-run (`../rerun-2026-10-12/v2-prodflow`) and the
AWS replication (`../v3-aws/e1`) were checked against their committed `SHA256SUMS` on 2026-10-14 (70 and 73 files, all
matching) before the run.

**Known gap.** The simulator replays each message once, with its final attempt's handler time. The 64 retried messages
of the local run (the `deliver_webhook` jobs that fail on purpose) therefore lose the work of their failed attempts.
