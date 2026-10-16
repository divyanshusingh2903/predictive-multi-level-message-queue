# Parameter sensitivity and block-bootstrap intervals (`v4-sensitivity-1`)

Pre-registered in [docs/v4-sensitivity-preregistration.md](../../../docs/v4-sensitivity-preregistration.md) and frozen in commit
`ef10a2a`, before any registered run. The simulator was `harbinger_trace_sim` built from that commit (binary SHA-256
`8c16cc6f…`), on the converted Azure 2021 and Huawei R5 traces of `v3-traces-1`. Configurations:
[`benchmarks/configs/v4-sensitivity/`](../../configs/v4-sensitivity/).

**Everything here is simulator output.** `v4-simvalidation-1` found the simulator overstates the mean-latency reduction of
reordering policies by about 40% on the production flow. These results are read for whether a verdict changes, not for the size
of an effect; the real-broker replay (`v4-replay-1`) is the evidence for sizes.

The pre-registered first check passed: the 28-window runs reproduce `v3-traces-1`'s aggregate metrics exactly on both traces.

## Part A: one parameter at a time, load 0.8, 7 windows (the `v3-traces-1` rule)

P1 is the gate's mean-latency criterion (at most −15%, interval below 0). Variants passing P1, out of 18:

| | Gate A (predictive routing as built) | Gate B (weights and pausing aging) |
|---|---|---|
| Azure 2021 | **0** (default −0.9%; best −3.4%, aging threshold 30 s) | **16** (default −22.8%) |
| Huawei R5 | **10** (default **−15.0%, exactly at the bar**) | **18** (default −46.5%; lowest −18.6%, 2 levels) |

- **Gate B is robust** on Huawei to every parameter, and on Azure to every parameter except two: the minimum observations
  `n_min` at 100 (−13.7%) and two levels (−10.1%), each one grid step or more from the default (R2, R3).
- **Gate A on Huawei sits on a cliff.** Its default passes by a hair and flips (R2, R3) under `n_min` 5 and 100, `κ` 4, all
  three other decay values, aging threshold 1 s, and two levels. Gate A on Azure never passes under any variant.
- **The aging threshold is the dominant parameter for gate A.** On Huawei, gate A's P1 is −3.3% at 1 s, −15.0% at the default 5 s
  and **−40.7% at 30 s**; on Azure −0.2%, −0.9% and −3.4%. The default was never tuned, and this is what the paper's "default
  aging erases the order" explanation predicts; it is still a simulator result.
- Staleness `τ` and the level weights do not move any verdict.

Full tables: `<trace>/sim/sensitivity.md`; machine-readable `sensitivity.json`.

## Part B: 28 windows, moving-block bootstrap (block 4), against the seven-window rule

Two of the 12 gate cells change verdict (T2):

| Cell | v3-traces-1 | Here |
|---|---|---|
| Azure, load 0.5, gate B | pass (−15.4%) | **fail (−8.4%, bar −15%)** |
| Huawei, load 0.8, gate A | pass (−15.0%) | **fail (−11.6%)** |

So gate A passes **0 of 6** cells (was 1 of 6) and gate B **4 of 6** (was 5 of 6). The point estimates move because a window's
relative change is averaged over windows, and finer windows weight the busy ones differently; the narrow passes were
fragile to the choice of 7 windows. The intervals widen with the block length, as the critique expected: Huawei gate B at load
0.8 is [−52.8%, −21.5%] with independent windows and [−65.5%, −4.1%] with blocks of 4.

Per trace: `<trace>/block_bootstrap.md` (every P1 cell at block 1, 2 and 4, and the verdict changes).

## What follows

Part A found verdict changes (R2 and R3) on both traces, so the pre-registration's Part C applies: a separate registration,
`v4-sensitivity-2`, is written **before any replay**, to confirm on the real broker at Huawei load 0.8 the variants whose gate
verdict differs from the default's. It needs per-variant overrides in `harbinger_replay`, which do not exist yet.

## Files

| File | Content |
|---|---|
| `<trace>/sim/<variant>.json` | the simulator's output for each of the 18 variants and `w28.json` |
| `<trace>/sim/sensitivity.{md,json}` | Part A: every variant's P1 verdicts and the R1 to R3 readings |
| `<trace>/block_bootstrap.{md,json}`, `report.md`, `summary.json` | Part B |
