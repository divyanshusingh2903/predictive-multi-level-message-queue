# Issue #5 — online predictor comparison v1

**Decision: `no_qualifier`.** All eight frozen candidates passed the measured
resource budgets and supplied complete warm predictions, but none passed both
quality criteria in every required workload. No production model/checkpoint is
selected and predictive activation remains false.

The [research/design guide](../../../docs/online-predictor.md) describes the
literature, algorithms and limitations. Reproduction commands live in
[`analysis/README.md`](../../../analysis/README.md). This is a duration-predictor
comparison, not evidence of predictive scheduling benefit.

## Protocol and complete coverage

- Configuration: [`online-comparison-v1`](../../../analysis/configs/online-comparison-v1.json).
  Schema `synthetic-job-v1`; adapter `numeric-scale-indicator-zero-v1`; policy
  `synthetic-duration-3-10-v1`, boundaries `[3,10]` ms, readiness at 100 unique
  eligible labels and per-job global backoff below 20 category labels.
- Six cells, five seeds (11/22/33/44/55), two repeats and 2,000 messages per run.
  Every candidate starts fresh on each independent broker clock.
- The 60 initial broker trials all accepted and drained their 120,000 messages.
  Forty-seven had complete telemetry; thirteen were invalidated by 15 lost
  feedback records. These originals are preserved, not silently discarded.
- Coverage-only preparation executed 20 additional broker trials: 15 matching
  replacement attempts plus five unused repeat-0 trials produced while replacing
  repeat 1. Two matching replacement attempts also lost telemetry and needed
  their second allowed attempt. All 80 raw broker trials drained; the admitted
  final matrix contains 60 clean trials and 120,000 messages.
- The clean export has 240,000 feedback events, 120,000 eligible successful Acks,
  59,994 train labels, 60,000 evaluation labels and six embargoed message groups.
  All 60,000 evaluation ingress remains present. There is no admitted telemetry
  loss, observation loss, missing ingress/terminal, or attempt gap;
  `valid_for_learning=true`.
- Dataset fingerprint:
  `a6ea99d5f7591530532b681c6c319441b7b87f1b74637260dec6213bfde875a4`.
- Comparison retains 480 candidate/run reports, 1,440 separate resource profiling
  replays and 960 additional 10/100 ms availability-delay reports. Scoring uses
  the stored ingress forecast before its eligible feedback trains the model.

## Selected observations

The table reports equal-weight means of run MAEs in milliseconds. Each cell has
10 runs / 10,000 warm evaluation labels. These descriptive averages do not replace
the per-cell, per-metric paired comparisons in [`decision.json`](decision.json).

| Candidate | Informative uniform | Informative bimodal | Relationship-shift heavy tail |
|---|---:|---:|---:|
| Global mean | 4.769 | 5.358 | 1.892 |
| Per-job mean | 1.601 | 0.306 | 1.943 |
| Per-job EWMA | 1.617 | 0.305 | 1.279 |
| Adaptive tree, raw | 4.769 | 0.568 | 1.696 |

Per-job means substantially help where metadata carries stable context, but fail
the global-mean comparator on shift and slightly on uninformative bimodal MAE.
EWMA improves shift adaptation, but fails other stationary/control criteria.
Raw/log linear and ordinary/adaptive tree candidates also have required-cell
quality failures. The conservative trees' uniform-workload result exposes their
warm-up/split limitations; online updates do not imply convergence at readiness.

All candidates have 100% warm eligible prediction coverage. The largest measured
profile P99 is **0.076555 ms**, versus the 1 ms budget. Maximum accounted model
state is **86,867 bytes**, versus 32 MiB. Model memory is Python ownership size
plus a serialized-state proxy, not a hard allocator cap. Whole-process peak RSS
is **193,296 KiB**, including imported libraries and the comparison process;
it is not the per-model budget denominator.

Bucket support is deliberately reported. In informative bimodal evaluation,
7,948 labels are in bucket 0, only two in bucket 1 and 2,050 in bucket 2. Almost
100% bucket accuracy there does not establish middle-bucket reliability. Duration
error and severe-underestimate gates remain authoritative.

The acceptance rule uses point estimates no worse than the stronger arithmetic
baseline separately for MAE and severe underestimates in every required cell.
Bootstrap intervals average repeats within seed, then resample five seed-level
deltas (2,000 resamples, seed 42). Small nonzero regressions remain failures under
this frozen rule; the decision is not a claim that all failures are statistically
significant. Budgets were not relaxed after observing results.

## Evidence inventory

| Path | Contents |
|---|---|
| `manifest.json` | Config/schema/dataset/source hashes, dependency/runtime versions, audit dimensions and availability assumptions |
| `decision.json` | Every candidate's failures and paired intervals, frozen parameters and explicit no-qualifier decision |
| `summary.json` | All 48 candidate/workload descriptive summaries, including bucket support and resource maxima |
| `runs.jsonl` | All candidate/run quality, per-job, warm-up, shift/window and resource reports |
| `delay-sensitivity.jsonl` | Extra-label-delay diagnostics; these are sensitivity assumptions, not measured production delays |
| `preparation.json` | All matching coverage admission decisions and selected raw source paths |
| `collection/` | Compact initial trial/manifest/uncertainty summaries, retaining all invalid trials |
| `raw/` | Hash inventory and checksums for the original collection, replacement collections, clean temporal export and complete comparison archives. The archives themselves are not tracked in git |

The collection revision is the fetched issue-#4 merge with a dirty working tree;
source fingerprints and the complete comparison configuration identify the
implementation used. Dependencies are exactly pinned in
[`requirements-models.txt`](../../../analysis/requirements-models.txt). Golden
evidence consistency is checked in CI, and archive fingerprints are checked when
the (untracked) archives are present; CI does not rerun this full timing matrix.

Final review added a regression fix for retry audit events published after a
terminal Ack. Such late non-training events are accepted without re-learning;
missing-ingress eligible labels still fail. The same retained dataset and frozen
configuration were replayed after this fix, reproducing the numerical quality
decision and refreshing the host resource measurements and archive fingerprints.

## Interpretation

`no_qualifier` should be read narrowly. The frozen rule requires a point estimate no
worse than both arithmetic baselines on both metrics in every cell, with no
tolerance, so noise-level differences fail a model (for example the per-job mean
misses the uninformative control by +0.0014 ms MAE, with an interval spanning zero).
The only feature is a 3-value category with a fixed payload, so a per-job mean is
close to the best possible predictor and richer models had little to learn. The
per-job mean cuts MAE by about 3x on informative workloads. This result compares
duration predictors only. The issue #4 baselines also show that static priority with
ground-truth classes lowers median latency but not P95/P99, which bounds what any
predictor can gain under these budgets. Follow-up work uses a pre-registered v2 matrix
(see the repository README, "Design direction").

## Limitations and operating decision

Costs are capped, sleep-based, integer-millisecond consumer observations on an
unpinned shared host. Payloads are fixed at 16 bytes. Labels are success-conditioned;
failures/censorship are tested separately, not injected into the clean selection
matrix. Publication after writer admission approximates feedback availability;
durable ingestion delay is not measured. No real-workload generalization,
production allocation bound, broker-latency gain, or predictive activation is
claimed. The justified current operating choice remains static routing.

> The offline tools used here lived in `ml_engine/` when these results were produced; the folder is now `analysis/` (same code), so manifests record the old paths.
