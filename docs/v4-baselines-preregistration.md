# Baselines, predictor ablation and aging isolation: pre-registration (`v4-baselines-1`) — DRAFT, NOT FROZEN

**Status: draft.** It becomes a pre-registration only when the commit that freezes it exists, before any registered
run. After freezing, changing anything requires a new version, and these results stay published.

| | |
|---|---|
| Issue | none |
| Configuration | [`benchmarks/configs/v4-baselines.json`](../benchmarks/configs/v4-baselines.json) |
| Harness | `benchmarks/prodflow.cpp` with the arms listed below; `benchmarks/run_prodflow.sh` |
| Runner | `benchmarks/cloud/run_all.sh`, experiment `e7` |
| Analysis | `benchmarks/prodflow_report.py` (readings, report-only criteria and groups) |
| Environment | AWS, one `c8i.2xlarge` alone, us-east-1, Ubuntu 24.04, broker embedded over loopback, in the `harbinger-bench` image built from the freeze commit; the same conditions as `v3-cloud-e1` and `v4-cloud-reruns`. About 9.4 h (11 arms × 10 seeds × about 5.1 min). |

## Why this experiment

A reviewer's three fair objections to the paper are that (1) it compares against FIFO and static maps but not against
the simple alternatives an operator would try, (2) it never shows which parts of the predictor matter, and (3) its
explanation of why the predictor gains more as a RabbitMQ sidecar than inside Harbinger, that Harbinger's aging gives
part of the gain back, is "an explanation and not a measurement". This experiment answers all three on the production
flow, where real handlers run through the real broker.

## Workload

`v2-prodflow-1` unchanged: nine job types, five services, 4 workers, 300 s of arrivals at rate scale 1.8, 180 s drain,
aging 5000/500 ms, and the mid-run change in job costs.

**Seeds:** ten, **11, 22, 33, 44, 55, 66, 77, 88, 99, 110**. The first five are those of `v2-prodflow-1`. Arm order is
rotated per seed, as before. Ten seeds, not five, because bootstrap intervals over five are unreliable.

## Part A: baselines

| Arm | What it is | Needs |
|---|---|---|
| `fifo` | one level (reference) | exists |
| `predictive` | the paper's predictor (reference, re-run on all ten seeds) | exists |
| `static_tuned` | the informed hand-tuned map (reference) | exists |
| `predictive_mean` | per-key decayed **mean** instead of the median | arm for `DurationSummary::Mean` (enum exists) |
| `predictive_p75` | per-key 75th percentile | exists |
| `per_type_fair` | **no prediction**: one level per job type (9 levels), equal worker-time weights, aging off | new arm |

`per_type_fair` is the practical alternative the paper lacks: one queue per job type, served by the broker's own
weighted-time rule with equal weights, so each type gets an equal share of worker time. It needs no cost knowledge but
does need an operator to create one queue per type. It uses existing mechanisms (the static policy, the virtual-clock
rule of `level_weights`, expected cost from each level's running average). Its worker-time shares are not tuned.

Arms deliberately **not** included: a global median (it routes every message to one level and so is FIFO in effect),
and per-producer keys (already measured as `predictive_producer`).

## Part C: aging isolation

| Arm | What it is |
|---|---|
| `predictive_noaging` | `predictive` with aging off: strict priority, nothing is ever promoted |

Everything else is `predictive`. This tests whether Harbinger's aging costs mean latency on this workload, the paper's
explanation for the sidecar gap. It also measures what aging buys: without it, the longest-waiting jobs may starve, which
the long class's maximum wait and the all-message P99 show.

## Part B: predictor ablation

Each arm is `predictive` with one component removed. All use the broker's own configuration fields.

| Arm | Change from `predictive` | Component tested |
|---|---|---|
| `ablate_decay` | `decay = 1.0`, `time_half_life = 0` | forgetting old durations (matters after the cost shift) |
| `ablate_spread` | `max_spread_ratio = ∞` | the 99th-percentile spread check |
| `ablate_stale` | `stale_after = 0` (disabled) | the staleness rule |
| `ablate_nmin` | `min_samples = 1` | waiting for 20 observations before predicting |

**The censored-share check is not ablated.** This workload has no failure injection or lease overruns, so removing
the check cannot change anything. It stays untested here; the paper says so.

The spread check and staleness may also show nothing on this workload (its costs are stable and its keys active).
A null result is a result: the paper reports it and does not claim these components help on this workload.

## Criteria

All are paired per seed on **mean latency** unless stated, with the bootstrap interval over the ten seeds (10,000 resamples,
seed 2026, 95%). Nothing here is a gate for the paper's main claim: the configuration has no gate, only readings.

**How a reading is made.** For the relative change of an arm against its baseline, with margin 3%:
**up** if the whole interval is above 0 and the point is at least +3%; **down** if the whole interval is below 0 and the
point is at most −3%; **tied** if the whole interval lies within ±3%; otherwise **inconclusive**. (The draft's earlier wording
read an interval that crosses 0 outside ±3% as "worse"; "inconclusive" is the honest reading and replaces it.) The 3% margin
is small against the 15% mean-latency effect that the production-flow gate demands, so "within ±3%" means practically
equal. It was chosen here before any run, not from the data.

**Part A** (the change is the first arm named relative to the second, so a negative value favours the first arm):

| ID | Comparison | up / tied / down |
|---|---|---|
| B1 | `predictive` vs `per_type_fair` | predictive worse / tied / predictive better |
| B2 | `predictive` vs `static_tuned` | same (extends S1 of `v2-prodflow-1` to ten seeds) |
| B3 | `predictive_mean` vs `predictive` | variant worse / tied / variant better |
| B4 | `predictive_p75` vs `predictive` | same |

For each variant, short-class P50, all-message P99 and the long class's maximum wait against `predictive` are **reported
without a reading** (`R_*` criteria), so that a variant that wins the mean by wrecking the tail is visible.

**Part B.** For each ablation, the change against `predictive`, overall (`AB_*_overall`) and in the shifted phase
(`AB_*_shifted`), and the reading of the two together (the config's `groups`):

| Reading | Rule |
|---|---|
| component **supported** | removing it reads **up** overall or in the shifted phase, and neither reads **down** |
| component **not supported here** | both read **tied** |
| component **harmful here** | removing it reads **down** in either, and neither reads **up** |
| **mixed** | one reads up and the other down |
| **inconclusive** | anything else |

**Part C.**

| ID | Comparison | Reading |
|---|---|---|
| C1 | `predictive_noaging` vs `predictive`, mean latency | **aging costs mean latency** if it reads down; **aging does not change the mean** if tied; **aging helps the mean** if up |
| C2 to C4 | `predictive_noaging` vs `fifo`: mean latency, all-message P99, long-class maximum wait | reported, no reading |
| C5, C6 | `predictive_noaging` vs `predictive`: P99, long-class maximum wait | reported, no reading (the cost of removing aging) |

C2 is shown beside the RabbitMQ sidecar's −57.6% (E3) for context only: E3 ran on a broker host and a client host with
RabbitMQ's FIFO as the baseline, this run on one host with Harbinger's, so the two numbers are not compared by a test.

**Quiet check (Q1).** `predictive` against `fifo` should land near the AWS production-flow results of E1, E5 and E6 (−30.8%,
−30.7%, −30.6%) and meets the −15% bar. If it does not, this run is a different condition and is reported as such.

## What happens either way

- If `per_type_fair` matches or beats `predictive`, the paper says so: the benefit then comes from separating job types
  in time, and the prediction's advantage is that it needs no per-type queues, not that it schedules better.
- If an ablated component is not supported, the paper says its justification rests on the traces or on reasoning, not
  on this workload.
- If `predictive_noaging` reads "aging costs mean latency", the paper says the sidecar gap is at least partly Harbinger's
  aging and says what it costs in tail terms; if "tied", the explanation is not supported on this workload and the paper
  says the sidecar gap is not explained by aging; if "aging helps the mean", it says so.
- Every arm's result is reported, including variants that win.

## Required changes before freezing (code, not run)

Status 2026-10-15.

1. **Done:** arms `predictive_mean`, `per_type_fair`, `predictive_noaging`, `ablate_decay`, `ablate_spread`, `ablate_stale`,
   `ablate_nmin` in `prodflow.cpp`. The embedded broker accepts nine levels with the static policy and equal `level_weights`
   and aging switched off; each new arm started and completed a short unregistered run locally (seed 7, 8 s).
2. **Done:** `v4-baselines.json` (ten seeds, 11 arms, readings, report-only criteria and groups) and the report's readings,
   report-only criteria and groups, with tests.
3. **Done:** `e7` in `benchmarks/cloud/run_all.sh`. **Open:** the AWS debug pass of all 11 arms on seed 7 (60 s each), reading
   only that no work is lost; then freeze.
4. The quiet check above, after the registered run.

## What has been seen before freezing

The `predictive`, `fifo` and `static_tuned` results on seeds 11 to 55 are published (local and, for `predictive` and `fifo`,
AWS). The RabbitMQ sidecar result of E3 has been seen. **No result of any new arm has been seen**, and none of the new
seeds (66 to 110) has been run. The ±3% margin, the readings above and the aging reading were written before any new arm
ran at a registered seed; the only runs of the new arms were 8-second local checks on seed 7 that read only that they
completed.

## Threats

- One workload, designed by the author, with near-deterministic costs per job type. `per_type_fair` is likely to do
  well on exactly such a workload; that is the point of the test, and it is not evidence about workloads where costs
  vary within a type.
- Ten seeds is still small. The report gives each per-seed change and how many seeds share its sign.
- One AWS host type, one region; effect sizes depend on CPU speed (see the paper's re-run discussion).
- `predictive_noaging` tests aging inside Harbinger. It does not show that aging is the whole difference from the sidecar:
  the sidecar also differs in the broker, the protocol and the host layout.
