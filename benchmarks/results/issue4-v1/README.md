# Issue #4 baseline evidence and budget freeze

This directory publishes real loopback-gRPC baseline feasibility evidence from
the issue #4 implementation. Numerical predictive budgets are frozen in
`../../configs/budgets-v1.json`; no predictive model has been measured or enabled.

The compact `calibration/` and `baselines/` artifacts retain every trial summary,
per-class counts/maxima, configuration/environment/source/trace hashes, and
paired uncertainty. The [raw archive guide](raw/README.md) indexes compressed
snapshots of full feedback, per-message sidecars, dataset exports, and earlier
development runs. Original observations are retained alongside their hashes;
rerunning the recorded configurations/seeds reproduces offered traces, while
host-dependent measured times will vary.

## Environment and reproducibility

Measured on Linux 7.0.0 x86-64/glibc 2.43, Intel Core i7-11800H, GCC 15.2.0,
gRPC 1.71.0, Protobuf 5.29.3, and Python 3.13.9 (Anaconda). CMake Release uses
`-O3 -DNDEBUG`, warnings-as-errors, and no sanitizer. Runs were sequential and
unpinned, with the recorded CPU affinity; host scheduling noise remains.

The checkout was based on `16c40e26fa2b720c80c6f528ae770e759a78bddb` with
uncommitted issue #4 changes. Manifests record the dirty flag and individual
runtime source/executable hashes rather than claiming that revision alone
contains the implementation. Every paired cell retains the same trace hash and
all worker/TTL/retry/lease/aging settings. Policy order is seeded and randomized.

Reproduce from repository root, using a fresh output path:

```bash
python3 -m benchmarks.evaluate --config benchmarks/configs/calibration-v1.json \
  --runner build-benchmarks/benchmarks/harbinger_synthetic_benchmark \
  --output /tmp/harbinger-calibration-v1
python3 -m benchmarks.evaluate --config benchmarks/configs/baselines-v1.json \
  --runner build-benchmarks/benchmarks/harbinger_synthetic_benchmark \
  --output /tmp/harbinger-baselines-v1
```

`manifest.json` preserves configuration and source versions; `trials.jsonl`
retains every trial's metrics, class maxima/counts, losses, and raw sidecar
hashes. `summary.json` is a convenience median view, not a pooled quantile or
confidence interval. `uncertainty.json` contains paired-seed deltas/ratios,
bootstrap intervals, zero-denominator flags, and insufficient-pair indicators.

## Capacity calibration and frozen rates

Twenty disabled-production trials (five seeds per distribution/worker cell)
fully drained with complete telemetry. Each offered 200 messages at 1,000/s;
costs are capped at 20 ms. The capacity estimator is successful messages divided
by whole-run full-drain time, taking the median across seeds. This is a finite
backlog estimate, not a steady-state capacity guarantee.

| Workload / workers | Median full-drain messages/s | Rounded reference |
|---|---:|---:|
| Uniform / 1 | 86.39 | 86 |
| Bimodal / 1 | 174.99 | 175 |
| Heavy-tail / 1 | 281.62 | 282 |
| Bimodal / 4 | 699.79 | 700 |

Single-worker below/near/overload rates are the reference times 0.5/0.9/1.1,
rounded to the nearest integer (half upward): uniform **43/77/95**, bimodal
**88/158/193**, heavy-tail **141/254/310** messages/s. The four-worker cell uses
350/s. These rates were frozen after calibration, before baseline comparisons;
none was adapted to a policy's own performance.

## Baseline matrix

The declared 15 cases × 5 seeds × 2 repeats × 5 policies yielded **750 trials**,
each offering 200 messages. All 750 fully drained. The 700 TTL-off trials
completed every offered message successfully; stress trials retained explicit
TTL/retry-limit outcomes. No dispatch/outcome observation was dropped.

**707 trials had complete telemetry coverage; 43 were invalidated by feedback
admission loss.** Fourteen invalid trials were TTL-off and 29 were stress runs.
The lossy trials remain in `trials.jsonl`; they are excluded from complete-coverage
paired intervals. Nonblocking feedback admission is an intentional broker safety
boundary, and its measurable loss prevents treating the entire dataset as clean
training evidence. It does not imply delivery loss. Stress cells have only 2–4
valid feedback-enabled trials per policy; their comparative confidence evidence
is insufficient and must not be called a passing gate.

### Descriptive P95 terminal latency

Values are medians of valid **per-trial** P95 values in milliseconds. They are
descriptive and do not replace paired intervals or class fairness data. Warm
trials measure 180 messages after a 20-message prefix; cold trials measure 200.

| Case | FIFO | Static | Round-robin | Disabled |
|---|---:|---:|---:|---:|
| Uniform low | 19.59 | 19.58 | 19.59 | 19.52 |
| Uniform near | 31.84 | 32.87 | 34.16 | 32.72 |
| Uniform overload | 175.88 | 188.25 | 449.21 | 170.38 |
| Bimodal low | 20.57 | 20.80 | 20.81 | 20.63 |
| Bimodal near | 84.42 | 83.37 | 161.62 | 90.21 |
| Bimodal overload | 196.25 | 182.52 | 341.38 | 196.32 |
| Heavy-tail low | 15.39 | 15.49 | 15.29 | 15.45 |
| Heavy-tail near | 26.84 | 30.25 | 25.28 | 26.81 |
| Heavy-tail overload | 64.08 | 82.99 | 97.85 | 61.75 |
| Uninformative bimodal | 68.74 | 112.36 | 117.06 | 85.53 |
| Heavy-tail relationship shift | 26.86 | 40.35 | 26.34 | 26.76 |
| Uniform cold | 31.71 | 34.39 | 35.55 | 33.41 |
| Bimodal four workers | 19.45 | 19.46 | 19.48 | 19.52 |
| Bimodal aging off | 80.51 | 93.79 | 131.02 | 88.27 |

No baseline is a universal winner. Metadata-static loses its advantage on the
uninformative and relationship-shift controls, and level-based round-robin can
increase terminal tails under overload. Class maxima/starvation and raw sample
counts remain available for every trial. Primary successful throughput over the
common window is equal when every message drains; full-drain timing is reported
separately rather than interpreting that equality as equal sustainable capacity.

## Feedback dataset verification

The all-static export from the complete matrix retained 28,752 eligible Ack
observations, 737 failed-attempt observations, and 1,233 TTL-censored observations
(audit dimensions can overlap). Its temporal split produced 16,783 train labels
and 11,335 evaluation labels, embargoing 884 message groups. It reports 17 lost
feedback records across 11 invalid static runs and 7 unresolved groups, so
`valid_for_learning=false`. These records are audit evidence, not a clean
activation dataset.
All 12,000 evaluation ingress records remain present, including messages with
censored or missing labels, rather than selecting prediction coverage by eventual
successful completion.

An individual clean run, `heavy-low-s11-r0-disabled`, validated all 400 feedback
events for 200 messages, with 120 train labels and 80 delayed evaluation labels,
no cross-partition message overlap, no losses/gaps, and `valid_for_learning=true`.
The pipeline tests also cover reordered log publication, duplicate-ID replay,
feature privacy, and embargo of a deliberately delayed pre-cutoff label.

## Budget freeze and limits

`phase2-budgets-v1` adopts the validation document's proposed numerical gates
unchanged: 10% warmed informative P95 benefit against static, at most 5% latency
regression, at least 95% comparator throughput, 5%/5 ms overhead limits,
1 percentage-point starvation allowance, and per-class longest-wait allowance
`max(100 ms, 10% of baseline maximum)`. Confidence uses matching repeats within
seed and 2,000 paired-seed percentile-bootstrap resamples (seed 42, 95% interval).
Undefined ratios and insufficient pairs remain unevaluable.

These short sleep-based runs establish harness feasibility, variance, and
baseline trade-offs. They do not establish P99 readiness, CPU-bound behavior,
model accuracy, durable feedback/learner-consumption delay, classifier health,
or predictive benefit. Final candidate experiments need a prespecified larger
matrix with valid telemetry and model/readiness configurations. Any budget or
matrix refinement requires a new version and rationale before predictive runs.
Production remains static; issue #10 owns the eventual activation decision.
