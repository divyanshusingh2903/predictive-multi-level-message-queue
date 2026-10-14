# Simulator validation: pre-registration (`v4-simvalidation-1`)

**Frozen 2026-10-14, before any registered run.** The simulator has not been run on the registered measured runs. Changing anything below after results exist requires a new version, and these results stay published, as in the earlier registrations. The freeze commit is the one that adds this file's `frozen_on` date.

| | |
|---|---|
| Issue | TBD (create before freezing) |
| Configuration | [`benchmarks/configs/v4-simvalidation.json`](../benchmarks/configs/v4-simvalidation.json) |
| Simulator | `tools/trace_sim.cpp`, with the options listed under *Required changes* |
| Measured data | `v2-prodflow-1` local quiet-machine re-run (`rerun-2026-10-12/v2-prodflow`) and its AWS replication (`v3-aws/e1`) |
| Analysis | `benchmarks/sim_validation.py` |

## Why this experiment

Every trace result in the paper comes from `trace_sim`, not from the broker. Nothing so far shows that the simulator
reproduces what the real broker did. This experiment replays the production-flow workload, whose real outcome we
have, through the simulator and compares the two. It tests the simulator, not the policy.

## Question

On a workload where both exist, does the simulator reproduce the real broker's **effects** (the change from FIFO
under each policy), its **ordering** of the policies, and its **per-phase direction**?

Absolute latencies are not expected to match. The real broker adds its own overhead (its FIFO was 11% slower than
RabbitMQ's in `v3-brokers`), and the real handlers contend for CPU. The simulator has neither. Effects relative to
FIFO are what the trace conclusions rest on, so they are the primary criteria.

## Inputs

For each registered seed (11, 22, 33, 44, 55) and each measured run:
- the **arrival time** and **measured handler duration** of every message, taken from that run's `messages.tsv`
  (hash-checked against its `SHA256SUMS`; the raw trees are expected under `~/data/rerun-2026-10-12` and `~/data/aws-run`, to be confirmed before freezing);
- the **key** of every message (producer service + job type), as the broker's predictor saw it;
- the **planned job cost** class used by the real `oracle` arm.

The simulator is told the real run's settings: 4 workers, 3 levels, default level 1, aging 5000/500 ms, no
compression of arrivals. Durations are the measured ones, so the simulator sees the same work.

Two measured sets are used, local and AWS. A criterion must hold in both to pass.

## Arms compared

| Arm | Real counterpart | Simulator arm |
|---|---|---|
| FIFO | `fifo` | `fifo` |
| Predictive | `predictive` | `predictive_cold` (empty predictor, as in the real arm) |
| Hand-tuned static | `static_tuned` | `static_map` with the real tiers |
| Misconfigured static | `static_misconfigured` | `static_map` with the real tiers |
| Oracle | `oracle` | `oracle_bounds` with the real planned-cost terciles |

## Criteria

All are computed per seed and averaged over the five seeds. The effect of an arm is the paired relative change in
**mean latency** against FIFO, as in equation (1) of the paper, for the real run (`Δ_real`) and the simulator
(`Δ_sim`).

| ID | Criterion | Passes if |
|---|---|---|
| **V1** (primary) | effect agreement, per arm | \|`Δ_sim` − `Δ_real`\| ≤ 10 percentage points, for each of predictive, static tuned, static misconfigured and oracle |
| **V2** (primary) | per-phase direction | predictive's change against FIFO has the same sign in the simulator and in the real run in each of the five phases (steady, ramp, flash sale, recovery, shifted) |
| **V3** (primary) | group order | the arms fall in the same order of groups in the simulator as in the real runs: {predictive, oracle} faster than static tuned, faster than FIFO, faster than static misconfigured, with every arm of a group faster than every arm of the next; order inside a group is not compared; holds in at least 4 of 5 seeds |
| V4 (reported) | absolute error | mean latency of each arm in the simulator against the real run, as a ratio |
| V5 (reported) | short-class P50 | `Δ_sim` against `Δ_real` for the short-class median under predictive |

**Gate:** V1, V2 and V3 pass, in both the local set and the AWS set.

The 10-point margin was chosen from the size of effects in the paper (predictive −26% local, −31% AWS), so that
agreement means the simulator gets the size of the effect roughly right, not only the sign. It was chosen without
running the simulator on this data.

## What happens either way

- **If the gate passes,** the paper reports it and notes that the simulator reproduces this workload's effects, with
  V4 as the caveat about absolute latency.
- **If it fails,** the paper reports the gaps by criterion and says that the trace results are not validated against
  the broker. The trace section is then described as showing what a policy would do in an idealised simulator.
- **The simulator is not tuned to pass.** No parameter, overhead constant or rule is adjusted after seeing a result
  under this label. Any adjustment is a new version (`v4-simvalidation-2`) and both are reported.

## Required changes before freezing (code, not run)

Done, on the working tree and not yet committed:

1. `trace_sim` replay mode (`"replay": true`): arrivals keep their real spacing, no history split, results labelled
   `replay`. An empty history takes the fixed class bounds in place of terciles. Configs without the new options
   reproduce the published results: the Huawei `v3-traces-1` simulation was re-run with the new binary and matches
   the committed `simulation.json` exactly (apart from the timing field).
2. `trace_sim` arms `oracle_bounds` (ranks the trace's size column, here the planned cost, against fixed bounds) and
   named static maps (`static_maps`), plus an optional per-message dump (`per_message_dir`).
3. `analysis/trace_format.py`: a `prodflow` converter from one measured run (`messages.tsv` + `run.json`), which
   refuses a run with lost work.
4. `benchmarks/sim_validation.py`: the V1 to V5 analysis. It reuses `prodflow_report.metrics` for both the real and the
   simulated runs, so the metric definitions are identical. (This replaces the `sim_report.py` section named in the
   header table.)
5. `benchmarks/tests/test_sim_validation.py`: tests on **synthetic** runs only, including hand-computed latencies for
   FIFO, a static map and the fixed-bounds oracle. The registered measured runs have not been read by any test.

`benchmarks/configs/v4-simvalidation.json` now holds the criteria, margins, arm mapping and static maps, and
`sim_validation.py` reads them from it. The raw trees were checked on 2026-10-14 against their committed `SHA256SUMS`: all 70 local files and all 73 AWS
files match, with no extra files on disk, and the AWS archive matches its own `.sha256`. Nothing else was needed to freeze.

## Decisions taken before freezing

- **V3 compares groups, not a full ranking.** In the published real runs predictive and the oracle are within 6% of
  each other in every seed (0.1% to 5.8%), so their order is noise and a faithful simulator could swap them. The
  groups above are the order the real runs show in all ten seed-sets (five seeds, two measured sets). This used the
  real results only, which are published; no simulator output has been seen. The closest pair of different groups in
  the real runs is FIFO against the misconfigured map (5.2% apart at the smallest), so V3 still asks the simulator to
  keep that ordering.
- **Retries are not modelled, and that is the decision.** In the local run 64 of 34,956 messages were retried (the
  `deliver_webhook` jobs that fail on purpose). The simulator replays each message once, with its final attempt's
  handler time, so the work of failed attempts and the re-queueing are missing. Modelling them would need the
  failed attempts' durations, which `messages.tsv` does not record. The report states the gap, and V1 to V3 are judged
  as pre-registered despite it.

## What has been seen before freezing

All five seeds' real results are published (they are the paper's own results). The simulator has **not** been run on
this workload. The 10-point margin and the criteria above were written before it was.

## Threats

- The real arrivals were generated by the benchmark, not a trace; the simulator replays them exactly, so arrival-process
  error is zero here and the check is more favourable than a trace would be.
- The real broker's queue, locks, and lease logic are modelled only through the simulator's level and aging rules.
- Passing here shows the simulator is faithful on a synthetic, near-deterministic workload. It does not show it is
  faithful on heavy-tailed traces with long backlogs.
