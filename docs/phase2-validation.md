# Phase 2 validation plan

Status: issue #4 implements [real-broker synthetic workloads/baselines and temporal feedback export](../benchmarks/README.md), with [baseline evidence and full experiment history](../benchmarks/results/issue4-v1/README.md) and [frozen v1 numerical budgets](../benchmarks/configs/budgets-v1.json) retained in the repository. No predictive benefit or activation is claimed. [Issue #10](https://github.com/divyanshusingh2903/predictive-multi-level-message-queue/issues/10) owns the candidate comparison and rollout decision. See the [ADR](adr/0001-phase2-ml-contract.md) and [ML contract](ml-contract.md).

## Hypothesis and scope

Prioritizing short predicted handler runtimes should reduce latency on mixed workloads. Measure whether scheduling benefits exceed inference overhead without hiding long-job starvation, incomplete messages, or expirations. A nonpreemptive broker cannot interrupt a long handler already running, so model quality alone cannot guarantee tail improvement.

The minimum synthetic comparison is part of Phase 2's activation gate. Real trace replay and comparisons against external brokers remain Phase 3. Keep the default disabled/static until the gate passes and an operator explicitly opts in.

## Baselines

| Policy | Definition |
|---|---|
| FIFO | One logical FIFO; no inference |
| Static priority | Same number of FIFO levels with explicit fixed metadata-based assignments and declared aging; no inference |
| Round-robin | Broker cycles across nonempty FIFO levels, serving one message per selected level; same static assignments and declared aging |
| Disabled production policy | Existing all-at-`default_priority` routing; separates new telemetry effects from scheduling |
| Shadow | Same production dispatch decisions, with inference and feedback collection |
| Predictive | Valid predicted-duration buckets; static fallback on failure |
| Oracle control | Benchmark-only mapping from true synthetic handler costs; no production interface or learner receives that truth |

Public Submit has no priority selector. Baseline multi-tier assignments must be implemented in benchmark tooling/internal scheduler adapters, never by leaking tier knowledge to consumers. Specify a fixed job-type-to-tier map before results; include an uninformative map for the control scenario. FIFO ignores tier assignment. Report disabled production policy separately so a weak static assignment cannot masquerade as a broad win.

The implemented adapter is isolated in a separately compiled benchmark broker
library. FIFO assigns L0, metadata-static and round-robin share the frozen class
map, and disabled retains production default priority. Every policy uses the
same queue/settlement/expiry/aging code; no production RPC or scheduler flag is
added. Benchmark monotonic sidecars supply exact transition/dispatch times;
feedback v1 remains unchanged. Its UTC timestamps/publication order alone cannot
support precise delayed-learning chronology.

Use identical arrival traces, latent handler costs, consumer counts, CPU conditions, TTL/retry/lease settings, and declared aging configuration for paired runs. Inference and feedback consume measured resources. Use separate runs/ports with clean state. Baseline telemetry is enabled when comparing ML scheduling, and also run a telemetry-off production control to quantify collection overhead.

## Workload matrix

| Dimension | Required cases |
|---|---|
| Duration distribution | Uniform, bimodal short/long mix, heavy-tailed with a declared finite cap |
| Feature informativeness | Duration-correlated job metadata and uninformative metadata control |
| Offered load | Below, near, and above measured baseline capacity; proposed utilization points 0.5, 0.9, and 1.1 |
| Worker count | Single consumer and a declared multiple-consumer configuration |
| Model state | Cold/unready, warmed, and restored checkpoint |
| Drift | Seeded change in job mix or feature-to-cost relationship at a declared arrival index |
| Maintenance | TTL-off primary latency runs; separate TTL/retry/lease stress runs, including abandoned deliveries |
| Classifier health | Healthy, unavailable, slow, malformed/incompatible, and concurrency-saturated |
| Aging | Explicit production aging configuration for fairness gates; aging-off sensitivity runs |

Generate finite seeded traces independent of scheduling. Use at least five paired seeds/trials. Replay open-loop arrival schedules so a slower policy does not silently reduce offered load; report missed/late submissions and generator bottlenecks. Fix workload sizes, burst patterns, cost cap, runtime-realization method (CPU/sleep), warm-up length, and post-arrival drain deadline before the final matrix. Prefer sufficient completed messages for meaningful P99 estimates; report actual sample sizes.

`benchmarks/configs/baselines-v1.json` freezes the initial feasibility matrix:
five seeds, two repeats, sleep realization, three distributions, below/near/above
finite-backlog disabled capacity, informative/control metadata, relationship
shift, broker cold/warm cohorts, one/four workers, aging sensitivity, and separate
TTL/retry/lease stress. Learner readiness/checkpoints and classifier-health cases
remain planned. Its 200-message trials are feasibility evidence; freeze larger
candidate-comparison samples/configuration before claiming a P99 activation gate.

Estimate capacity from disabled/static baseline runs before choosing absolute arrival rates. All policies replay the same absolute rate for a paired cell; do not rescale by each policy's achieved capacity. Above-capacity results characterize overload and finite-drain behavior, not a claim of steady-state bounded latency.

Model evaluation uses time-ordered delayed feedback. Predict on ingress, evaluate the stored prediction when its label arrives, then learn. Do not use future feedback, actual synthetic runtime, or retry outcomes as ingress features. Group attempts by message when splitting/exporting data; report failed/censored/missing labels and success-only selection bias. Fit no preprocessing on future evaluation labels.

## Metrics

| Metric | Definition |
|---|---|
| First-dispatch wait | Broker's first Pull dispatch time minus original arrival time |
| Handler runtime | Consumer callback `steady_clock` measurement; integer milliseconds in current settlement RPCs |
| Terminal latency | Final successful Ack or DLQ transition time minus original arrival, including retry waits |
| Successful completion latency | Successful Ack latency alone, reported alongside all-terminal outcomes |
| P50/P95/P99 | Empirical nearest-rank quantiles: sorted sample at index `ceil(p × N)`, with `p` in `(0,1]` |
| Throughput | Successful messages per second over the common declared measurement window; also report terminal transitions/s and full-drain completion time |
| Outcome rates | Success, TTL DLQ, retry-limit DLQ, abandoned/unfinished, and rejected submission counts divided by offered/accepted counts as appropriate |
| Prediction error | MAE and RMSE in milliseconds on eligible labels, plus per-job-type errors and sample counts |
| Bucket quality | Confusion matrix/accuracy using frozen policy boundaries on true eligible durations |
| Severe underestimate rate | Fraction of eligible positive durations whose prediction is below half the measured runtime; zero-duration labels excluded only from this ratio |
| Inference overhead | Submit latency percentiles, RPC duration, concurrency occupancy, fallbacks, and throughput loss versus disabled control |
| Starvation | First-dispatch wait above `2 × num_levels × aging.threshold`; requires enabled aging and reports per-class counts/rates |
| Longest wait | Maximum observed first-dispatch wait by class, plus censored ages for never-dispatched messages |

For MAE/RMSE compare predictions against the strongest simple statistical baseline (global running mean or declared per-job-type statistics) on the same eligible events. Do not measure error using predictions recomputed after training. Report missing predictions, fallback coverage, invalid labels, telemetry loss, and sample counts; prediction metrics on only a small favorable subset cannot establish readiness.

Terminal quantiles include DLQ transitions but also report success-only quantiles and outcome rates, since rapid expiry can falsely improve aggregate latency. Unfinished messages have censored ages at the common drain cutoff; never invent a terminal time or omit their counts. For fairness, a never-dispatched message older than the starvation threshold counts as starved; younger censored messages are reported separately. Primary gate runs use TTL off and require complete drains, so differential drops/censoring cannot create a latency win.

## Frozen v1 pass/fail budgets

The original design proposals below are adopted unchanged as `phase2-budgets-v1`,
frozen on 2026-10-03 in the machine-readable budget manifest before predictive
runs. They are criteria, not observed predictive results. Baseline feasibility
evidence does not waive a gate. Any later budget change requires a new version,
published rationale, and new comparisons; never tune to observed predictive results.

| Gate | Frozen v1 criterion |
|---|---|
| Primary benefit | At least 10% P95 terminal-latency improvement versus static priority on warmed, informative bimodal and heavy-tailed workloads below saturation |
| Latency guardrail | No more than 5% P50/P95/P99 regression versus each FIFO/static/round-robin comparator in the declared warmed, below-saturation gate cells |
| Throughput guardrail | At least 95% of each comparator's successful throughput in the same gate cells |
| Shadow overhead | At most 5% throughput loss and at most 5 ms added P99 Submit latency versus disabled production routing with matching telemetry |
| Failure overhead | Same overhead limits under the declared unavailable/slow/invalid/saturated classifier runs |
| Prediction quality | MAE and severe-underestimate rate no worse than the stronger simple statistical predictor on the same held-out delayed labels |
| Fairness | Starvation-rate increase no more than 1 percentage point versus each baseline in finite, below-saturation aging-enabled gate cells; report all per-class and maximum waits |
| Longest-wait guardrail | In each required warmed, below-saturation, aging-enabled cell, each declared class's maximum first-dispatch wait exceeds each FIFO/static/round-robin comparator's class maximum by no more than `max(100 ms, 10% of that comparator maximum)` |
| Correctness/data validity | Existing and ML integration checks pass; gate runs fully drain, have no unexpected message loss, and have no feedback loss that invalidates prediction comparisons |

For the longest-wait gate, declare job classes independently of predicted buckets in the experiment manifest. For every paired seed/class/comparator, compute `predictive_max_wait - baseline_max_wait - max(100 ms, 0.10 × baseline_max_wait)`; apply the paired confidence procedure below with passing values at or below zero. Publish raw class maxima and counts for every trial, not only the aggregate. The absolute allowance handles small/zero baseline waits; the relative allowance constrains large regressions. Classes with no observations are unevaluable, not automatic passes; ensure the frozen traces cover required classes. Required runs must fully drain. This is an experimental fairness guardrail, not a production waiting-time guarantee. Baseline variance may justify refining the proposed allowance, but only before predictive evaluation with a versioned rationale.

Uniform/uninformative workloads are regression controls. Cold-start, drift, and overload runs must appear in the report with readiness/fallback/recovery analysis; define additional budgets for them explicitly before claiming them as gated deployment conditions. An oracle miss explains opportunity limits but does not waive a failed gate. A best-case predictive cell cannot compensate for a failed guardrail in another required cell.

Use paired per-seed deltas with 95% confidence intervals, with the estimator/resampling method and trial count frozen in advance. Proposed decision rule: the confidence interval must lie entirely on the passing side of the gate (including the improvement target). If evidence is inconclusive, collect more prespecified paired trials or report inconclusive and remain in shadow; do not silently call it a pass. Report absolute values when a ratio denominator is zero; predeclare an absolute tolerance or mark the ratio gate unevaluable, rather than dividing by zero.

## Reproducibility and freeze procedure

1. Implement the baseline harness, collect capacity/variance evidence, and verify outcome/latency accounting.
2. Publish the experiment manifest: repository revision, machine/OS/runtime details, dependencies, feature/model/policy versions, configurations, seeds, arrival/cost traces, warm-up/drain rules, baseline assignments, final budgets, confidence method, and candidate-selection protocol.
3. Select/tune model candidates using separate time-ordered development streams (#5). Freeze model configuration before the final comparison; online updates during evaluation follow the declared delayed-learning rule.
4. Run the paired matrix, retain machine-readable records/aggregates, and publish every required cell, failure, and uncertainty interval.
5. Any later budget/matrix change gets a new version and rationale plus a new evaluation; preserve old results.

## Activation and rollback

Predictive mode is eligible for explicit opt-in only when all frozen required gates pass. Default stays disabled. If a gate fails or telemetry is inadequate, keep static/shadow routing and file follow-ups that explain the failed condition.

The completion report must document service/broker startup, header/schema configuration, boundaries, deadlines/fallbacks, readiness, retention/loss behavior, checkpoint recovery, and returning to disabled/static routing. A broker restart loses existing broker messages; do not present restart as lossless rollback. Switching mode at runtime, if implemented later, affects new ingress only; queued/in-flight messages keep their original routing context and priority policy.
