# Aging switched off on a real trace: pre-registration (`v4-replay-aging-1`) — DRAFT, NOT FROZEN

**Status: draft.** It becomes a pre-registration only when the commit that freezes it exists, before any registered run. After
freezing, changing anything requires a new version, and these results stay published.

| | |
|---|---|
| Issue | none |
| Configuration | [`benchmarks/configs/v4-replay-aging-huawei2025-r5.json`](../benchmarks/configs/v4-replay-aging-huawei2025-r5.json) |
| Parent | [`v4-replay-1`](v4-replay-preregistration.md): same harness, trace, plan, warm start, validity rules, report and gate rules |
| Harness | `harbinger_replay`, with the new arm flag `+no_aging` (`benchmarks/replay.cpp`); runner `benchmarks/run_replay.sh` |
| Analysis | `benchmarks/replay_report.py`; the criteria are inline in the configuration |
| Environment | AWS, one client and one broker host, **each `c8i.xlarge`** (4 vCPU), us-east-1, one placement group, the `harbinger-bench` image built from the freeze commit |

## Why this experiment

Predictive routing as built (gate A) fails on the traces. The paper's explanation is that the default aging, which promotes
a message after 5 s in the queue, erases the priority order when a backlog lasts hours. That explanation has indirect support
only: the oracle does not help either, the simulator found the aging threshold dominant for gate A (`v4-sensitivity-1`), and
aging switched off cut the mean by a further 37.4% on the production flow (`v4-baselines-1`, C1). None of that is a measurement
on a real trace through the real broker.

## Question

On Huawei Cloud R5 replayed through the real broker, does predictive routing with aging **off** pass gate A's rules in a cell
where predictive routing with the default aging does not?

## Design

Everything is as in `v4-replay-1` for Huawei, except:

| | `v4-replay-1` | here |
|---|---|---|
| Arms | seven | `fifo`, `predictive_warm`, `predictive_warm+no_aging` |
| Loads | 0.5, 0.8, 0.95 | **0.8 and 0.95** (gate A passed at 0.8 and failed at 0.95 in the simulator; at 0.5 it failed everywhere) |
| Repeats | 3 | 3 |
| Hosts | `c8i.2xlarge` client, `c8i.xlarge` broker | **`c8i.xlarge` client and broker**: all the quota that is free beside the two `v4-replay-1` pairs |
| Probe | FIFO at 0.1 | none |

`predictive_warm+no_aging` is `predictive_warm` with the broker's aging switched off: strict priority, nothing is promoted.
Because the hosts are smaller than the registered pairs, **this experiment carries its own `fifo` and `predictive_warm` arms**,
and every comparison is within it. Its numbers are not compared by test with `v4-replay-1`'s; the paper may set them side by side
and says the hosts differ. Run validity is as in `v4-replay-1` (all messages complete, no dead letter, no lost lease, producer
lateness p99 at most 10 ms in real time); the smaller client makes that check informative, and a run that fails it is kept as
`.failed` and repeated once.

## Criteria

Each of the following is evaluated per load with the rules of `v4-replay-1` (28 windows, block bootstrap with block length 4,
10,000 resamples, seed 2026, pooled over the repeats):

- **Gate A** (`A_P1`, `A_G1`, `A_G2`): `predictive_warm` against `fifo`: mean latency at most −15% with the interval below 0, all-message
  P99 at most 2.0×, long-class maximum wait at most 2.0× (floored, at least 30 long jobs per window).
- **Gate N** (`N_P1`, `N_G1`, `N_G2`): the same three rules for `predictive_warm+no_aging` against `fifo`.
- `N_vs_A_mean_latency`: `predictive_warm+no_aging` against `predictive_warm`, mean latency, passing if the interval is below 0.

If the single-repeat verdicts of a gate cell disagree with each other, the cell is reported as unstable and not counted as a
pass, as in `v4-replay-1`.

## Reading

For each load, the pair of verdicts (gate A, gate N):

| Reading | Rule |
|---|---|
| **aging costs gate A here** | gate N passes, gate A does not, and `N_vs_A_mean_latency` passes, in that load, stable |
| **both pass** | both gates pass: aging is not what stops gate A in this cell |
| **aging is not the cause here** | gate N does not pass (whatever gate A does) |
| **unstable** | any of the repeats disagree on either gate |

The headline for the paper is the reading at each load; it says "aging costs gate A on Huawei" only if that reading holds at at
least one load and gate N's guardrails (`N_G1`, `N_G2`) pass there, so that the gain is not paid for in starvation.

## What happens either way

- If aging costs gate A, the paper says the default aging threshold, not the predictor, is what fails the trace gate, and points
  to the aging threshold as the parameter to tune or to the opt-in weights and pausing design.
- If gate N does not pass, the paper says the explanation is not supported on this trace and removes it.
- Every cell is reported, including repeats and `.failed` runs.

## What has been seen before freezing

The simulator's Huawei gate A verdicts (`v3-traces-1`, and `v4-sensitivity-1`'s 28-window and aging-threshold results), the
production-flow aging result (`v4-baselines-1`) and the E5 result have been seen. **No cell of `v4-replay-1` has been read**
(the runs are in progress and only liveness has been checked), and no run of this experiment has been made except smoke runs
on a synthetic trace that read only that the arm completes. The loads, arms and readings were chosen from the simulator
results and the baselines, not from any replay result.

## Threats

- One trace, one region. Smaller hosts than the main replay; the smoke check and the lateness rule are the guard against a
  harness-limited result.
- Switching aging off removes the starvation protection; five-minute runs cannot show starvation, and a Huawei cell is about
  20 to 25 minutes, so the guardrail `N_G2` is the only evidence of its cost.
- `v4-replay-1`'s own cells are being read for the first time after this experiment is registered, so the two are not
  independent of the same analyst; the order of registration and reading is recorded here.
