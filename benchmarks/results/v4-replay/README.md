# Real traces through the real broker (`v4-replay-1`)

Pre-registered in [docs/v4-replay-preregistration.md](../../../docs/v4-replay-preregistration.md) and frozen in commit `608dd02`,
before any registered run. The run used that commit and the `harbinger-bench` image built from it (`sha256:52c97bb0…`, the same on
all four hosts), on two pairs of a `c8i.2xlarge` client and a `c8i.xlarge` broker in us-east-1d, 2026-10-15 07:14 to 2026-10-17
21:02 UTC. Each experiment was split across the pairs with `SHARD=0/2` and `1/2`; the shards do not overlap (manifests in each
trace directory). Huawei Cloud 2025 R5 at f = 1 and Azure Functions 2021 at f = 4.5 (loads 0.5, 0.8, 0.95; the gate arms at 0.5 and
0.95, all five at 0.8), replayed through the real broker with handlers that sleep for the trace's duration.

**All 76 cells ran and all 76 counted** (Azure 12, Huawei 64, including one overhead probe each). The raw `messages.tsv` and
`run.json` files (5.8 GB, hashed in each trace's `SHA256SUMS`) are not committed; `runs/` holds every `run.json`. `replay_report.md` and
`replay_summary.json` in each trace directory are the pre-registered analysis, unchanged.

**Two cells lost the broker and were re-run once, as registered** (both attempts are kept). On Huawei, `oracle` at load 0.8 (repeat 3,
pair 1) and `predictive_warm+weights+pausing` at load 0.5 (repeat 3, pair 2): the broker died within seconds of starting, no
`broker.json` was written, 482,770 and 482,663 submits failed after 413 and 520 completed messages, and the client waited out its
drain limit. The retries are fully valid. `failed-attempts/` holds each failed attempt's `client.json`. The cause is not known: the
hosts' kernel and Docker logs show nothing and memory was not short, and the broker containers were removed before their logs
could be read. It is 2 of 66 Huawei cells and 0 of 12 Azure cells.

## Conclusion

| Gate | Azure 2021 | Huawei R5 |
|---|---|---|
| **A**: predictive routing as built (default aging) | fails at 0.5, 0.8, 0.95 | fails at 0.5, 0.8, 0.95 |
| **B**: weights and pausing aging | **passes at 0.8 and 0.95**, fails at 0.5 | **passes at 0.8 and 0.95**, fails at 0.5 |

**Gate A passes in 0 of 6 cells and gate B in 4 of 6**, with the three Huawei repeats agreeing on every cell. That is exactly what the
simulator said once its intervals were re-estimated with 28 windows and a block bootstrap (`v4-sensitivity-1`), and two cells fewer
than it said with the original seven independent windows (`v3-traces-1`: gate A 1 of 6, gate B 5 of 6): Huawei gate A at 0.8 and
Azure gate B at 0.5 do not hold up on the real broker.

Mean-latency change against FIFO, 28 windows, block bootstrap (block 4), pooled over repeats (Huawei) or one run (Azure):

| | Load | Gate A: `predictive_warm` | Gate B: `predictive_warm+weights+pausing` |
|---|---|---|---|
| Huawei | 0.5 | −2.5% [−4.6, +0.0] fail | −2.4% [−4.4, +0.0] fail |
| | 0.8 | −10.9% [−14.8, −1.6] fail | **−37.0%** [−66.0, −4.0] pass |
| | 0.95 | −8.4% [−11.5, −3.2] fail | **−47.1%** [−74.8, −17.9] pass |
| Azure | 0.5 | −0.9% [−1.9, −0.2] fail | −8.0% [−11.5, −4.3] fail |
| | 0.8 | −1.6% [−3.0, −0.6] fail | **−15.3%** [−23.9, −9.0] pass (the bar is −15%) |
| | 0.95 | −2.2% [−3.9, −1.1] fail | **−17.2%** [−25.8, −11.6] pass |

Gate B's guardrails pass in every cell: all-message P99 at most 1.12× FIFO's and long-class maximum wait at most 1.14×. Whole-run mean
latency on Huawei at load 0.8 is 17.9 s for FIFO, 15.0 s for `predictive_warm` and **1.7 s** for gate B's design (median 9.3 s, 5.1 s
and 65 ms); at 0.95 it is 42.1 s, 38.1 s and 7.1 s. On Azure the weighted design's whole-run gain is small (−7.9% at 0.8, −9.3% at 0.95),
because Azure's last day holds most of the work and the backlog there dominates.

**On the real broker the predictor does not beat a static map built from the same history.** `predictive_warm` against `static_history`
(S2, non-inferiority within +10%) fails on Huawei at 0.8 (+8.2% [+0.1, +17.2]) and 0.95 (+28.6% [+0.5, +58.2]); at 0.5 they are equal.
A cold predictor (S4) is nearly as good as a warm one.

## The simulator, on these two traces

Not part of the gates. The whole-run mean-latency change against FIFO from the replay and from the simulator agree to about a point in
nearly every cell: Huawei `predictive_warm` −16.3% against −16.9% at 0.8 and −9.5% against −9.8% at 0.95, the weighted design −90.4%
against −89.7% and −83.2% against −82.8%, the weighted oracle −61.8% against −61.7%; Azure's weighted oracle −52.8% against −52.8% and the
weighted design −7.9% against −7.7%. The roughly 40% overstatement found on the production flow (`v4-simvalidation-1`) does not appear
here. The replay's handlers do no CPU work, which removes the contention the production flow has, so this shows that the simulator's
queueing model is right, not that a real application with CPU-bound handlers would match it. The verdicts differ from the original
simulator verdicts only where the original seven-window figure sat at a bar.

## Overhead probe (FIFO at load 0.1, not gated)

The broker adds to a job's latency, in trace milliseconds, a median of 0.52 ms and a P99 of 1.27 ms on Huawei (54,547 messages), and a median
of 4.07 ms and a P99 of 6.80 ms on Azure (311 messages; at f = 4.5 this is about 0.9 and 1.5 ms of real time). Measuring from the actual submit
instead of the scheduled arrival changes mean latency by at most 0.06%.

## Notes

- Azure ran once per cell, so the instability rule (a gate cell is unstable if its repeats disagree) can apply only to Huawei, where no cell was
  unstable. Azure's gate B at load 0.8 passes by 0.3 points and rests on one run.
- Two bugs in `replay_report.py` were fixed after `v4-replay-aging-1` and before these results were analysed (see that README).
- Short-job medians for Azure are not gated: about a quarter of Azure's jobs last 1 ms or less, which is 0.2 ms at f = 4.5.

## Files

| File | Content |
|---|---|
| `<trace>/replay_report.md`, `replay_summary.json` | the pre-registered analysis (`python -m benchmarks.replay_report benchmarks/configs/v4-replay-<trace>.json <merged raw tree> --simulation … --simulation-summary …`) |
| `<trace>/runs/<arm>_load-<load>_rep-<n>.json` | every counted run's `run.json` (`probe_…` for the overhead probe) |
| `<trace>/failed-attempts/` | `client.json` of each failed attempt (Huawei only) |
| `<trace>/SHA256SUMS` | hashes of every raw file, paths relative to the merged raw tree |
| `<trace>/manifest-p{1,2}.txt`, `progress-p{1,2}.log` | commit, config and trace hashes, image, shard, and each pair's run log |
