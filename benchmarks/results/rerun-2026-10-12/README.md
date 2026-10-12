# Quiet-machine re-run, 2026-10-12

This re-runs the three wall-clock production-flow matrices on a quiet machine, with documented conditions, and
re-checks the trace simulations:
- [`v2-prodflow-1`](../v2-prodflow/README.md);
- [`v3-aging-prodflow-1`](../v3-aging/README.md);
- [`v3-sizebins-1`](../v3-sizebins/README.md).

The frozen configs and the pre-registered analysis are unchanged. The published results stay as they are; this
directory records how they hold up.

**Code and data.** The code was commit `45977b0`; the binary's sources are identical to it, and 8 unrelated paths
(drafts and report additions, committed afterwards in `4d6c1dc`) were uncommitted. Raw `messages.tsv` and `run.json` files (266
MB) are not committed; their hashes are in each `SHA256SUMS`.

## What changed

**Conclusion: no pre-registered verdict changed.**
- **Production flow:** every criterion that passed still passes, every one that failed still fails, and all three gate
  outcomes are the same (v2 passed; v3-aging and v3-sizebins did not).
- **Trace simulations:** all four reproduced byte for byte.
- **What moved:** two v2 effect sizes are smaller. The cause is CPU speed, not background load: today's runs had turbo
  off, and the published v2 run had a CPU about 1.6–1.8× faster.

## Conditions

| | |
|---|---|
| Machine | Intel Core i7-11800H laptop (8 cores / 16 threads), 14 GB RAM, Ubuntu 26.04.1, kernel 7.0.0-38, on AC power |
| CPU frequency | turbo off (`intel_pstate/no_turbo=1`), 2.3 GHz cap, performance profile, EPP performance |
| Code | commit `45977b0`, Release build; binary SHA-256 in `manifest.txt` |
| Duration | 06:45–13:01 UTC; v2 3 h 07 min, v3-aging 1 h 20 min, v3-sizebins 1 h 47 min, simulations 1.5 min |

750 samples, every 30 s (`system-samples.tsv`):

| Measure | Median | Extreme |
|---|---|---|
| 1-min load (16 threads) | 1.04 | max 2.38 (p95 1.48) |
| Available memory | 8,986 MB | min 8,529 MB |
| Mean CPU frequency | 1,545 MHz | max 2,152 MHz |
| Package temperature | 47 °C | max 53 °C |

**Other processes.** Ordinarily they used about 2% CPU each:
- `agy` (in the top 3 in 551 samples, median 2.1%);
- `claude-desktop` (median 2.3%);
- `gnome-shell`.

Single-sample bursts of 20% or more were rare. On 16 threads each is at most a few percent of capacity for a few
seconds:
- `agy` four times (21–44%);
- `claude` once (29%);
- `btop` once (40%);
- `soffice.bin` once (96%, at 12:24 UTC, during v3-sizebins).

(`ps` showing 100% is the sampler measuring itself.)

## Trace simulations: all identical

`v2-azure-sim`, `v3-aging`, `v3-traces-huawei2025-r5` and `v3-traces-azure2021` reproduced their published outputs
byte for byte, ignoring each run's own wall-clock duration. As expected: the simulator runs on simulated time and
cannot be affected by load.

## Production flow: published vs re-run

All 70 runs completed every message: no lost work, no DLQ, no submit errors.

### v2-prodflow-1 (gate: passed → passed)

| Criterion | Published | Re-run | Verdict |
|---|---|---|---|
| P1 mean vs FIFO | −27.9% [−31.9, −24.7] | −25.6% [−29.4, −21.8] | pass → pass |
| **P2 short-class P50 vs FIFO** | **−88.4%** [−99.0, −72.6] | **−57.7%** [−70.0, −45.3] | pass → pass |
| G1 all-message P99 vs FIFO | 1.04× [1.02, 1.06] | 1.02× [1.00, 1.05] | pass → pass |
| G2 export max wait vs FIFO | 1.02× [0.96, 1.05] | 0.99× [0.95, 1.03] | pass → pass |
| G3 no lost work | pass | pass | pass → pass |
| G4 throughput | 1.00× | 1.00× | pass → pass |
| S1 vs static_tuned | −10.9% | −11.1% | pass → pass |
| S2 vs static_misconfigured | −33.8% | −30.1% | pass → pass |
| S3 shadow overhead | −0.6% | +0.1% | pass → pass |
| **S4 producer-only keys vs FIFO** | **−15.4%** | **−10.5%** | pass → pass |

### v3-aging-prodflow-1 (gate: not passed → not passed)

Every criterion agrees within 0.2 points:
- G2, all-message P99 vs `predictive`: 1.12× in both, failing the 1.10 limit both times.
- S1, mean vs `predictive`: −42.0% → −41.9%.
- G4, mean vs FIFO: −56.9% → −56.7%.

This run was published with the same CPU speed as today.

### v3-sizebins-1 (gate: not passed → not passed)

| Criterion | Published | Re-run | Verdict |
|---|---|---|---|
| P1 sized vs predictive, mean | −1.5% [−5.1, +1.7] | +0.7% [+0.5, +0.9] | fail → fail |
| S1 resize mean | −4.4% | −2.8% | fail → fail |
| S2 resize, shifted phase | −5.1% [−14.3, +2.9] | −1.4% [−2.0, −0.6] | fail → fail |
| S3 invoice mean | +11.1% | +8.7% | fail → fail |
| S4 sized vs FIFO | −26.2% | −25.1% | pass → pass |
| S5 predictive vs FIFO | −25.1% | −25.6% | pass → pass |
| Guardrails G1–G5 | pass | pass | pass → pass |

**The re-run is much less noisy.** For example, G2's interval shrank from [−5.8, +0.6] to [−0.0, +0.5], and S2's from
[−14.3, +2.9] to [−2.0, −0.6]. The conclusion is unchanged and now firmer: size bins do not help on this workload.

## Why v2's effect sizes moved

Each run records how long its CPU kernels take (`calibration_ms_per_unit`):

| Run | Resize, ms/megapixel | Export, ms/chunk | Invoice, ms/item |
|---|---|---|---|
| v2 published | 8.11 | 21.4 | 0.048 |
| v3-sizebins published | 11.28 | 23.8 | 0.062 |
| v3-aging published | 13.16 | 33.5 | 0.113 |
| Re-run (all three) | 13.15 | 33.3 | 0.114 |

**The published v2 run had a CPU about 1.6–1.8× faster.** That matches turbo being on (this CPU turbos to about
4.6 GHz; the cap today was 2.3 GHz), or the different container v2's README describes.

The job sizes and arrival rates are fixed, so a slower CPU means a heavier load:

| Median across seeds | v2 published | Re-run |
|---|---|---|
| FIFO mean latency | 8.0 s | 13.1 s |
| FIFO short-job median | 6.0 s | 15.5 s |
| `predictive` short-job median | 98 ms | 6.2 s |

**Why the short-job gain shrinks.** At this heavier load the backlog runs well past the 5 s aging threshold, so
waiting work is promoted to the top level and short jobs queue behind it. That is the aging collapse `v3-aging`
studied. The mean gain barely moves (−27.9% → −25.6%), but the short-job median gain falls from −88% to −58%. S4 moves
for the same reason.

## What this means for the paper

1. **Verdicts are robust.** Every gate and criterion verdict reproduced on a quiet machine, and every simulation
   reproduced exactly.
2. **Effect sizes depend on load relative to CPU speed.** The paper should report:
   - the machine and its frequency settings;
   - a measure of load, such as FIFO mean latency or the calibration, next to every production-flow result;
   - not the bare rate scale.
3. **Which v2 numbers to cite.** Use the re-run's v2 numbers, with documented conditions, as the local result, and
   say the original run had a faster CPU. The AWS E1 run on a dedicated host is the independent replication, and its
   CPU will differ again.
4. **Disclose the remaining activity.** `agy` and Claude desktop were still running at about 2% CPU each, with a few
   brief bursts (listed above).

## Files

| Path | Contents |
|---|---|
| `manifest.txt` | machine, frequency settings, commit, binary hashes |
| `system-samples.tsv` | conditions every 30 s: load, available memory, CPU MHz, package temperature, top 3 processes |
| `progress.log` | the run's timeline |
| `<config>/report.md`, `<config>/summary.json` | output of `python -m benchmarks.prodflow_report benchmarks/configs/<config>.json <raw dir>` |
| `<config>/runs/seed-<seed>-<arm>.json` | each run's `run.json` |
| `<config>/SHA256SUMS` | hashes of every raw `messages.tsv` and `run.json` |
| `<config>/matrix-log.txt` | the runner log |
| `sims/reproduction.txt`, `sims/SHA256SUMS` | the simulation comparison and the hashes of the re-run outputs |

**Reproduce.** Build with `-DHARBINGER_BUILD_BENCHMARKS=ON` (Release), then for each config run
`PRODFLOW_CONFIG=benchmarks/configs/<config>.json benchmarks/run_prodflow.sh <binary> <raw dir> 11 22 33 44 55` and the
report command above. Absolute numbers depend on the CPU's speed; see "Why v2's effect sizes moved".
