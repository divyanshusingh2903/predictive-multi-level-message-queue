# Real traces through the real broker: pre-registration (`v4-replay-1`)

**Frozen 2026-10-15, before any registered run.** Changing anything below after results exist requires a new version, and
these results stay published, as in the earlier registrations. The freeze commit is the one that sets `frozen_on` in both
configs.

**Scope of this version: Huawei Cloud 2025 R5 and Azure Functions 2021 only.** DAS2-fs2-2003 and LPC-EGEE-2004 appear below
in the design text (their time scales, arms and aging constants) but are **not run under `v4-replay-1`**: their time scales
of 300 and 400 put the broker's aging constants at 1 to 17 ms, where scheduler jitter and the broker's 100 ms pull wake-up
are comparable to them, so they need a calibration run first and would be a separate, exploratory registration.

| | |
|---|---|
| Issue | none |
| Configurations | [`v4-replay-huawei2025-r5.json`](../benchmarks/configs/v4-replay-huawei2025-r5.json), [`v4-replay-azure2021.json`](../benchmarks/configs/v4-replay-azure2021.json) |
| Harness | `harbinger_replay` (`benchmarks/replay.cpp`), run by `benchmarks/run_replay.sh`; the broker is the real `harbinger_server_lib` |
| Traces | Huawei R5 and Azure 2021 of `v3-traces-1`, same converted files and hashes (checked by the runner against the configs) |
| Environment | AWS, two broker + client pairs, `c8i.xlarge` + `c8i.2xlarge`, one placement group per pair, as in `v3-cloud` |
| Analysis | [`benchmarks/replay_report.py`](../benchmarks/replay_report.py) |

## Why this experiment

`v4-simvalidation-1` showed that the trace simulator reproduces the direction of every effect and the order of the
policies, but overstates predictive routing's mean-latency reduction by about 40%. The paper's trace results come from
that simulator. This experiment replaces the simulator as the paper's evidence for real traces: the same traces, the
same gates, but through the real broker, with the real predictor and real aging, over gRPC.

## Question

On real traces replayed through the real broker, do the two gates of `v3-traces-1` pass where the simulator said they
would, and by how much?

- **Gate A:** predictive routing as built (strict priority, today's aging) against FIFO.
- **Gate B:** the opt-in design (worker-time weights `[8, 3, 1]`, pausing aging) against FIFO.

## What is replayed, and what is not

The handlers **sleep** for the trace's recorded duration; they do no work. The arrivals, durations and keys are the
trace's. So the experiment tests the broker, its predictor and its aging against real arrival and duration
structure. It does not test a real application's handlers, and it is not a production deployment.

## Time and load

Offered load is set exactly as in `v3-traces-1`: arrivals are compressed by a factor `c` per trace and load, so that the
mean offered load over the replayed span is 0.5, 0.8 or 0.95 of 64 workers (the compress factors are those in each
`simulation.json`). On top of that, everything is **time-scaled by a factor `f`** to fit the time budget:

- arrival offsets are divided by `c * f` and durations by `f`, so load is unchanged;
- **every time constant of the broker is divided by `f`**, so the system behaves as it would at real speed: the aging
  threshold and interval, the predictor's staleness limit and idle half-life;
- the delivery lease and time-to-live are set above the longest scaled job, so no lease expires and no message is
  retried; recovery is not what this experiment asks about;
- all reported times are multiplied back by `f`, so latencies are in trace time.

| Trace | Replayed span | `f` | Aging threshold / interval | Staleness / idle half-life | Real time per arm (3 loads) |
|---|---|---|---|---|---|
| Huawei Cloud 2025 R5 | the evaluation half, 12 h | 1 | 5000 / 500 ms | 30 min / 10 min | about 83 min |
| Azure Functions 2021 | the evaluation half, 7 days; loads 0.5, 0.8, 0.95 with `arms_by_load` | 4.5 | 1111 / 111 ms | 6.7 min / 2.2 min | 7.94 + 4.96 + 4.18 h per arm (61.2 h for the registered cells) |
| DAS2-fs2-2003 | the evaluation half, about 6 months | 300 | 17 / 2 ms | 6 s / 2 s | about 2.4 h |
| LPC-EGEE-2004 | the last 30 days of the evaluation half | 400 | 13 / 1 ms | 4.5 s / 1.5 s | about 5 h (to be computed from the data before freezing) |

Aging constants round to whole milliseconds, which the broker's configuration requires; the DAS2 interval (1.67 ms
rounded to 2) and the LPC interval (1.25 ms rounded to 1) are therefore off by 20% from exact scaling. LPC-EGEE is
replayed over a sub-span, because its full evaluation half would take months; the broker's predictor is warm-started
from everything before the sub-span, and the queue starts empty.

**Short jobs.** After scaling, jobs that are too short for the broker's per-message overhead cannot be measured. In Azure
about a quarter of jobs last 1 ms or less, which is 0.2 ms or less at `f` = 4.5. Mean latency, which is dominated by
waits of seconds to minutes, is not affected much; short-job medians for Azure are therefore **reported but not
gated**, and an overhead probe (below) quantifies the floor.

## Arms

| Arm | Routing | Level selection | Aging | Traces |
|---|---|---|---|---|
| `fifo` | one level | – | – | all |
| `predictive_warm` | predictive, predictor warm-started on the history half | strict | today's | all |
| `predictive_warm+weights+pausing` | predictive, warm | weights `[8, 3, 1]` | pausing | all |
| `oracle` | the true duration's tier (tercile bounds of the history half, as in the simulator) | strict | today's | all |
| `oracle+weights+pausing` | same | weights `[8, 3, 1]` | pausing | all |
| `static_history` | each key's history median to a tier | strict | today's | Huawei only |
| `predictive_cold` | predictive, empty predictor | strict | today's | Huawei only |

The warm start loads the history half through the predictor offline (with the scaled time constants) and gives the
broker the resulting snapshot, using the broker's existing snapshot mechanism.

## Repeats

A trace is a fixed input, so repeats measure only run-to-run noise. Huawei is run **three times** per cell. Azure,
DAS2 and LPC-EGEE are run **once** per cell for time. Huawei's three repeats give an estimate of that noise, which the
paper reports beside the other traces' single runs.

## Analysis and criteria

- **Windows and intervals.** The replayed span is cut into 28 windows of equal trace time. The primary interval is a
  moving-block bootstrap over the paired per-window changes (block length 4 windows, 10,000 resamples, seed 2026), as in
  `v4-sensitivity-1` Part B. The `v3-traces-1` interval (seven windows, independent) is reported alongside.
- **Metrics and classes.** As in `v3-traces-1`: mean, P99, long-class maximum wait, with the same class bounds in trace
  time, the same 30-long-job minimum per window for the wait guardrail, and the same wait floor.
- **Gates.** Gate A and gate B each need three criteria: P1 mean latency against FIFO, point at most −15% and interval
  below 0; G1 all-message P99 against FIFO, interval upper bound at most 2.0; G2 long-class maximum wait against FIFO,
  interval upper bound at most 2.0 (floored, at least 30 long jobs per window). Secondary criteria S1 to S7 as in
  `v3-traces-1`, for the arms each trace has.
- **Comparison with the simulator.** For each cell, the paper reports whether the replay verdict equals the simulator's
  and the ratio of the replay's mean-latency change to the simulator's. This is descriptive.

**Run validity.** A run counts only if every message completes with none lost or in the dead-letter queue, no lease
expires, and the producer's 99th-percentile lateness (actual against scheduled submission, in scaled time) is at most 10
ms. A run that fails these is repeated once and both are reported.

**Overhead probe (reported, not gated).** For each trace, FIFO at offered load 0.1 for 10 minutes of scaled time, to measure the
latency the broker adds to a job of a given length. The paper states these floors next to the Azure and HPC short-job
results.

## What happens either way

- Gate verdicts are reported for every cell, whichever way they fall. A cell that passed in the simulator and fails here
  is reported as a simulator over-estimate; a cell that failed in the simulator and passes here is reported too.
- The simulator's trace results then appear in the paper only as a comparison column and in the validation section.
- If the Azure replay at `f` = 4.5 disagrees with Huawei's at `f` = 1 in a way that overhead explains, the paper says so
  and does not claim agreement across the two.

## Required changes before freezing (code, not run)

Status 2026-10-14 (uncommitted). Nothing below has been run on a registered trace.

1. **Replay binary: done for the Huawei arms** (`benchmarks/replay.cpp`, target `harbinger_replay`, built with
   `-DHARBINGER_BUILD_BENCHMARKS=ON`). `run` replays the evaluation half: producer threads submit each message at its
   scaled arrival deadline, 64 consumers sleep for the scaled duration, and per message the client records the
   scheduled, submit, start and end times. Roles `all`, `broker` and `client` as in `harbinger_prodflow`; a split run
   joins with `benchmarks/merge_split_run.py` after `broker.json` is copied beside `client.json`. The broker's time
   constants (aging, staleness, idle half-life) are divided by `time_scale` in whole milliseconds, and the delivery lease
   is 1.25 times the longest scaled job plus 60 s, with TTL off.
2. **Warm-start tool: done** as `harbinger_replay prepare`, which also writes the tier boundaries, the compress factor per
   load, the `static_history` map and `plan.json`. The history half is fed through the broker's own `PerKeyPredictor` on a
   clock that ends at the evaluation start (as the simulator's `predictive_warm` does), with each duration truncated to
   whole milliseconds after scaling, as the consumer reports it. Saved with `save()`, loaded by the broker's snapshot
   mechanism from a per-run copy. On Huawei it reproduces the simulator's tier boundaries (22.9 / 31.0 ms) and compress
   factors (18.59, 29.74, 35.32), and holds all 426 history keys.
3. **`benchmarks/replay_report.py`: done** (`benchmarks/tests/test_replay_report.py`, 11 tests, and checked end to end on
   real `harbinger_replay` output from the synthetic trace). It reads `<arm>/<load>/<repeat>/{run.json,messages.tsv}`,
   applies the validity rules, cuts the span into 28 windows of equal trace time, and evaluates the registered criteria
   with the block bootstrap (primary) and the seven-window independent interval (alongside), reusing `sim_report.evaluate`.
   Repeats: the per-window metrics are averaged over the repeats valid for every arm (pooled), and each repeat's verdicts
   are listed too (decision 1). It also writes the replay-against-simulator table
   (ratio of mean-latency changes, gate verdicts side by side) and each cell's run-to-run spread. It also reports the overhead probe
   (`_probe`, not gated).
4. **Tests: done on a synthetic trace** (`benchmarks/tests/test_replay.py`, ctest `replay_python_tests`): plan equals the
   simulator's rule, every arm completes every message, scheduling lateness, FIFO order, oracle order, priority arms beat
   FIFO on a hand-built backlog, time scale divides the handler duration, bad arms and overwrites are refused. A split
   broker/client run was also checked locally. **Open:** the smoke run on AWS.
5. **Runner: done** (`benchmarks/run_replay.sh debug|registered CONFIG RESULTS PREP TRACE`). Lays runs out as
   `<arm>/<load>/rep-<n>`, rotates arm order by repeat and load, resumes by skipping finished cells, starts a fresh
   broker per run (split or embedded), joins split runs, and checks every run with `replay_report check`: a run that does
   not count is kept as `rep-<n>.failed` and run once more, so both are reported. `registered` refuses overrides, an unfrozen
   config, a dirty tree and a trace whose sha256 is not the config's. The overhead probe (config `probe`: FIFO, load 0.1,
   600 s) goes to `_probe`. Tested with the real binary on a synthetic trace: embedded, split over a stand-in ssh, the
   retry path, the refusals, and the report on its output. **The Docker path was also run locally** (2026-10-15, image built from the working tree on top of `a8a9a99`): the
   smoke kit, embedded and split with the broker as a separate container, all 7 arms and the probe, every run complete with
   no lost lease or dead letter, the warm snapshot reaching the broker container, no stray containers. **Not yet exercised:
   real ssh between two hosts and the cross-host image check**, which the AWS smoke run covers.
   The Huawei and Azure configs are `benchmarks/configs/v4-replay-{huawei2025-r5,azure2021}.json` (`frozen_on` null until the
   freeze commit). The AWS runbook (Lane R of `benchmarks/cloud/RUNBOOK-v4.md`) is written. **Not in this registration:**
   DAS2-fs2 and LPC-EGEE, whose time scales (300 and 400) and 1 to 17 ms aging constants need a calibration run first; they
   are dropped from this version and would be a separate, exploratory registration.
6. **Partly done:** the broker accepts whole-millisecond scaled aging constants and a lease above the longest job
   (checked at `time_scale` 1 and 2). The DAS2 and LPC constants (13 to 17 ms) are not checked.

### Azure (`benchmarks/configs/v4-replay-azure2021.json`)

- **Loads 0.5, 0.8 and 0.95, with the gate arms only at the edges** (`arms_by_load`). All five arms at 0.8; `fifo`,
  `predictive_warm` and `predictive_warm+weights+pausing` at 0.5 and 0.95, which is all that gates A and B need (the oracle
  arms feed only the secondary criteria S5 and S6, which are therefore evaluated at 0.8 alone). One run per cell. Why: gate B
  passed at all three Azure loads in the simulator, but at 0.5 by the narrowest margin (−15.4% against the −15% bar), so that
  cell is the one most likely to change under replay and dropping it would have removed the most informative result.
- **Time**, from `prepare` on the converted trace (936,382 evaluation rows, 1,044,569 history rows, 424 keys), at
  `time_scale` 4.5: 7.94 h per arm at 0.5 (compress 4.73), 4.96 h at 0.8, 4.18 h at 0.95. Eleven cells and the probe: 61.2 h,
  34.2 h and 27.0 h on the two shards (`SHARD=0/2`, `1/2`).
- **Overhead probe:** FIFO at load 0.1 for 133 s of real time, which is 10 minutes of trace time. `drain_s` is 6 h of real
  time, since the last day of the trace holds 62% of the work.
- **A finding that changed the harness.** Azure's keys are 129 characters and the broker rejects job keys over 128 bytes,
  so unmodified every Azure message would have been unpredicted and routed to the default tier (the first `prepare` warmed
  0 keys). Keys now travel on the wire as `k<index>` in order of first appearance, in `prepare`, the static map and `run`
  alike. The simulator passed raw keys to the predictor and never met this limit. A deployment whose job-type labels exceed
  128 bytes silently gets no prediction; the paper should say so.

### Decisions (2026-10-15, made before any registered run)

1. **Repeats.** The primary analysis pools the repeats (the per-window metrics averaged over the repeats valid for every
   arm). Each repeat's verdicts are also reported. **If the single-repeat verdicts of a gate cell disagree with each other,
   the cell is reported as unstable and not counted as a pass**, whatever the pooled verdict. Azure, DAS2 and LPC have one
   repeat, so the rule cannot apply to them; the paper says so when it sets Huawei's stability beside them.
2. **Lateness.** The validity bound (producer lateness p99 at most 10 ms) is in **real time**. Lateness in trace time (real
   time times the time scale) is also reported for every run.
3. **Latency** is measured from the **scheduled arrival**, as in the simulator, so a slow Submit at the broker counts
   against the broker. The report also gives the largest relative difference in mean latency if it were measured from the
   actual submit, as a sensitivity check, not as a second analysis.
4. **Azure loads** as above.

### Other choices made while writing it

- **Latency** is the handler's end on the client minus the scheduled arrival, times `time_scale` (decision 3). The Ack round
  trip is not included, as in the simulator.
- **Run validity.** The binary reports producer lateness (p50, p99, max) in real milliseconds; decision 2 fixes the bound.
- **Producer.** Eight threads, message `i` on thread `i mod 8`, one connection each. Two messages less than one round trip
  apart can swap order on the way in; the test allows two such swaps in 60.
- **Key.** The trace key is sent as the `job_type` header and is the whole predictor key (not producer-scoped), so the
  warm snapshot's keys do not depend on a broker-assigned producer id.
- **Lead-in.** Arrivals start 3 s after the consumers are started; the warm predictor's key ages are therefore 3 s older
  than in the simulator, which is negligible against a 30-minute staleness limit.
- **Millisecond rounding on the wire.** The consumer reports whole milliseconds, truncated, so the predictor sees
  durations slightly below the true ones; at Huawei's 23 to 31 ms boundaries this can move a job across a boundary. The
  simulator used unrounded durations. This is a real difference between the two and is part of what the experiment
  measures.

## What has been seen before freezing

Data only: the Azure, DAS2 and LPC-EGEE duration and work distributions, and each trace's real-time replay time, were
computed from the converted files. The published simulator results (`v3-traces-1`, `v3-traces-2`) and the validation
(`v4-simvalidation-1`) have been seen. **No replay of any arm of a registered trace has been run.** The scaling factors were
chosen from the time budget and the data checks, not from any replay result.

**Smoke runs.** Only on synthetic traces (`benchmarks/make_replay_smoke.py`): locally with the binary and with the Docker
image, and on 2026-10-15 on AWS, one pair (`c8i.2xlarge` client and `c8i.xlarge` broker, us-east-1d) from commit `96f2dc7`,
over real ssh with the broker as a separate container: all seven arms and the probe on a 20,127-message trace, every run
complete with no dead letter, lost lease or submit error, producer lateness p99 about 0.2 ms, the same image ID on both hosts,
and the warm arms predicting every message from the first. Only these pipeline facts were read; the smoke's latencies and
gates were not.

The decisions under *Decisions* above, and the Azure load and arm choice, were made after reading the simulator's Azure
results (gate B at 0.5 was −15.4%, the narrowest margin) and before any registered replay.

## Threats

- Sleeping handlers: no CPU contention or real work, so absolute latencies are lower bounds on what a real application
  would see.
- Time scaling changes the ratio of broker overhead to job length; the overhead probe and the scaled-time table above
  state how much, and short-job medians for Azure are not gated.
- Azure holds 62% of its evaluation work in one day, so its result is dominated by that day; one run per cell cannot
  say how stable it is.
- The warm start uses the history half of each trace, as in the simulator; a cold broker is tested only on Huawei.
- Whole-millisecond aging constants differ from exact scaling for DAS2 and LPC-EGEE.
- AWS, one region, two pairs of hosts; the two pairs may differ slightly, so the pair assignments are fixed in the
  configs and reported.
