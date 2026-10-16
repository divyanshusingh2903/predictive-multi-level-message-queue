# Aging switched off on a real trace (`v4-replay-aging-1`)

Pre-registered in [docs/v4-replay-aging-preregistration.md](../../../docs/v4-replay-aging-preregistration.md) and frozen in
commit `304e61b`, before any registered run. The run used that commit and the `harbinger-bench` image built from it, on a client
and a broker host, each a `c8i.xlarge` (4 vCPU), us-east-1d, 2026-10-15 19:04 to 2026-10-16 02:43 UTC
(`manifest.txt`, `host-*.txt`). Huawei Cloud 2025 R5 replayed through the real broker at loads 0.8 and 0.95, three arms, three
repeats: 18 runs of 483,183 messages. Every run counted: all messages complete, no dead letter, no lost lease, producer lateness
well inside the bound. The raw `messages.tsv` and `run.json` files (297 MB packed, hashed in `SHA256SUMS`) are not committed;
`runs/` holds every `run.json`. `replay_report.md` and `replay_summary.json` are the pre-registered analysis, unchanged.

**Conclusion: aging costs gate A here, at both loads.** The registered reading holds in each: gate N (aging off) passes, gate A
(default aging) does not, the direct mean comparison passes, the three repeats agree, and gate N's guardrails pass.

| | Load 0.8 | Load 0.95 |
|---|---|---|
| Gate A (`predictive_warm` vs `fifo`), mean change, 28 windows, block bootstrap | −11.0% [−15.1, −1.7] **fail** | −8.5% [−11.7, −3.2] **fail** |
| Gate N (`predictive_warm+no_aging` vs `fifo`), mean change | **−37.4%** [−66.0, −4.4] **pass** | **−45.4%** [−73.6, −16.2] **pass** |
| N against A, mean change | −34.1% [−64.4, −2.5] pass | −41.8% [−71.2, −12.9] pass |
| Gate N all-message P99 against FIFO (limit 2.0×) | 1.07× pass | 1.12× pass |
| Gate N long-class maximum wait against FIFO (limit 2.0×) | 1.16× pass | 1.14× pass |
| Repeats agree | yes, all three | yes, all three |

Whole-run figures (medians across the three repeats):

| Load 0.8 | `fifo` | `predictive_warm` | `predictive_warm+no_aging` |
|---|---|---|---|
| Mean latency | 17.7 s | 14.8 s | **1.7 s** |
| Median latency | 9.1 s | 4.9 s | **57 ms** |
| Short-job median | 9.0 s | 4.8 s | **49 ms** |
| All-message P99 | 60.4 s | 56.3 s | 52.4 s |

At load 0.95 the means are 41.8 s, 37.8 s and 7.2 s, and the medians 31.4 s, 29.0 s and 95 ms. The per-window mean change
(table above) is smaller than the whole-run change (−90% and −83%) because it averages each window's relative change, and quiet
windows change little; the pre-registered gate uses the per-window figure.

## What the cost of switching aging off is

Long-class maximum wait is 14 to 16% above FIFO and P99 7 to 12% above, well inside the gate's limits. Cells last 20 to 25
minutes, so this cannot show starvation over hours; it shows that over this length the protection aging gives is not needed.

## The simulator, on this trace

Not part of the registration. For `predictive_warm` the replay and the simulator agree on the whole-run mean change to under a
point (−16.5% against −16.9% at load 0.8, −9.5% against −9.8% at 0.95; ratios 0.97). An **unregistered** run of the simulator's
own `+no_aging` arm on the same trace (`exploratory/`) gives −90.0% and −82.9%, against −90.5% and −82.8% measured. So on Huawei
the simulator reproduces real-broker effects, including this one, to about a point, where it overstated the production flow by
about 40%. The replay's handlers sleep for the trace's duration and do no CPU work, so it removes the contention that the
production flow has; the agreement shows the simulator's queueing model is right, and does not show that a real application with
CPU-bound handlers would match it. The gate A verdict differs from the simulator's only because its 7-window figure was −15.0%
at the −15% bar (replay −14.3%).

## Notes

- The hosts are smaller than `v4-replay-1`'s, so this experiment carries its own `fifo` and `predictive_warm` arms and none of
  its numbers is tested against `v4-replay-1`.
- Two bugs in `replay_report.py` were found and fixed while analysing this run, before its numbers were read as final: it
  assumed every entry in the simulator's output is an arm (the `compress` factor sits beside them), and it reported a gate that
  exists only in the replay as a simulator failure. Both have tests.

## Files

| File | Content |
|---|---|
| `replay_report.md`, `replay_summary.json` | the pre-registered analysis (`python -m benchmarks.replay_report benchmarks/configs/v4-replay-aging-huawei2025-r5.json <raw>`, with the `v3-traces-1` simulation files for the comparison) |
| `runs/<arm>_load-<load>_rep-<n>.json` | every run's `run.json` |
| `SHA256SUMS` | hashes of every raw file, paths relative to the raw `aging/` directory |
| `exploratory/` | the unregistered simulator run of the `+no_aging` arm |
| `manifest.txt`, `host-*.txt`, `progress.log` | commit, image, hosts and run log |
