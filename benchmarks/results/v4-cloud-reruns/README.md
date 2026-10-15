# Aging and size-bin production flows on AWS (`v4-cloud-reruns`)

Pre-registered in [docs/v4-cloud-reruns-preregistration.md](../../../docs/v4-cloud-reruns-preregistration.md) and frozen in
commit `d9e21cf`, before any registered run. These re-run the frozen `v3-aging-prodflow-1` (E5) and `v3-sizebins-1` (E6)
configurations on AWS, unchanged, to see whether their verdicts hold on cloud hardware.

| | |
|---|---|
| Code | commit `d9e21cf` (`manifest.txt`); image `harbinger-bench` built from it |
| Host | one `c8i.2xlarge` (8 vCPU, Xeon 6975P-C, 16 GiB), us-east-1d, alone; broker embedded over loopback (`host-client.txt`) |
| Run | 2026-10-14 22:22 to 2026-10-15 01:19 UTC; E5 then E6; 35 runs, about 5.1 minutes each |
| Debug pass | seed 7, from commit `b7214e1`, 2026-10-14; all four runs completed 7,066 of 7,066 messages; not analysed |

**Conclusion: both gates came out as they did locally, and every criterion's pass or fail matches the quiet-machine
re-run (RP1 and RP2 hold).** Cloud hardware changed no verdict.

## E5, aging (`v4-cloud-aging-1`): gate not passed, as locally

15 runs: `fifo`, `predictive`, `predictive_weighted` on seeds 11, 22, 33, 44, 55.

| Criterion | AWS | Local re-run | Same verdict |
|---|---|---|---|
| G1 long-class wait vs FIFO (at most 2.0) | 1.126 [1.097, 1.143] pass | 1.144 [1.115, 1.163] pass | yes |
| **G2 all-message P99 vs predictive (at most 1.1)** | **1.100 [1.091, 1.107] fail** | 1.125 [1.118, 1.132] fail | yes |
| G3 no lost work | pass | pass | yes |
| G4 v2 gain kept vs FIFO (at most −15%) | −57.5% pass | −56.7% pass | yes |
| G5 throughput vs FIFO (at least 0.98) | 1.000 pass | 1.000 pass | yes |
| S1 mean vs predictive | −38.4% [−41.9%, −34.7%] pass | −41.9% pass | yes |
| S2 predictive replicates v2 | −30.7% pass | −25.3% pass | yes |

The only failing criterion, G2, is close on AWS: the point estimate is 1.100 and only the interval's upper bound (1.107)
is over the limit of 1.1. The verdict replicates, but marginally.

## E6, size bins (`v4-cloud-sizebins-1`): gate not passed, as locally

20 runs: `fifo`, `predictive`, `predictive_sized`, `oracle` on the same five seeds.

| Criterion | AWS | Local re-run | Same verdict |
|---|---|---|---|
| **P1 mean vs predictive (at most −3%)** | **−1.5% [−4.5%, +0.4%] fail** | +0.7% [+0.5%, +0.9%] fail | yes |
| G1 all-message P99 vs predictive | 1.001 pass | 1.001 pass | yes |
| G2 unsized classes vs predictive (at most +5%) | −2.0% pass | +0.2% pass | yes |
| G3 no lost work | pass | pass | yes |
| G4 long-class wait vs FIFO | 0.994 pass | 0.996 pass | yes |
| G5 throughput vs FIFO | 1.000 pass | 1.002 pass | yes |
| S1 resize mean vs predictive (at most −10%) | −1.1% [−19.4%, +17.2%] fail | −2.8% fail | yes |
| S2 resize after the shift (at most −10%) | +33.7% [−25.3%, +99.8%] fail | −1.4% fail | yes |
| S3 invoice mean vs predictive (at most −10%) | **+9.5% [+4.4%, +15.9%] fail** | +8.7% fail | yes |
| S4 sized mean vs FIFO (at most −15%) | −31.7% pass | −25.1% pass | yes |
| S5 predictive replicates v2 | −30.6% pass | −25.6% pass | yes |

Three points to read with care (RP3, descriptive):
- The P1 point estimate is negative on AWS (−1.5%) and positive locally (+0.7%). The AWS interval includes zero and
  does not reach the bar, so the verdict is the same; the two runs agree that size bins do not beat job-type keys by 3%.
- The resize rows are very noisy on AWS (intervals of tens of percentage points, five seeds), so they are neither
  evidence for nor against size bins.
- The invoice class is about 9% worse under size bins in both places, with an interval that excludes zero on AWS.

## Files

| File | Content |
|---|---|
| `e5/`, `e6/` | `config.json`, `report.md`, `summary.json` (the pre-registered analysis), `run-log.txt`, `runs/` (every run's `run.json`), `SHA256SUMS` |
| `manifest.txt`, `progress.log`, `host-client.txt` | commit, image ID, timing and the host's conditions |

**Raw data.** The per-message `messages.tsv` files (about 3.9 MB per run, not committed) are in
`~/data/aws-run/registered/registered-d9e21cf-e5-e6/`. Each experiment's `SHA256SUMS` hashes every raw file (paths are
relative to that experiment's raw directory, under a `raw/` prefix) and was checked against them when this directory was
written: 32 files for E5 and 42 for E6, all matching. The archive's own hash (SHA-256 `3b52f5ef…51e1f9`) matched after
download. All 35 runs completed every message with no dead-lettered messages, submit errors or lost leases.

**After the run.** The host was stopped (not terminated) on 2026-10-15.
