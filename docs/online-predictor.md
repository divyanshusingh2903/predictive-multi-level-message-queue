# Online processing-time predictors

Issue #5 provides an **offline delayed-feedback comparison**, built on the issue-#4
broker replay/export harness. It predicts successful handler duration in
milliseconds. A model-selection result does not establish a scheduling benefit:
classifier transport, shadow integration, checkpoint recovery, and predictive
activation remain later milestones.

## Research and design rationale

The following sources informed the implementation. Their findings motivate
comparisons; they do not establish that a particular model will win on Harbinger.

| Source | Relevant finding and implementation consequence |
|---|---|
| [Montiel et al., River, JMLR 2021](https://www.jmlr.org/papers/v22/20-1380.html) | Incremental models and transforms share a per-observation interface. River 0.22.0 is pinned and verified, while feature/feedback utilities stay dependency-free. |
| [Bottou, stochastic learning](https://leon.bottou.org/research/stochastic) | SGD updates parameters from individual observations with inexpensive state. Use regularization and numeric scaling; preserve chronological feedback order rather than adopting batch shuffling. |
| [Domingos & Hulten, Mining high-speed data streams, 2000](https://doi.org/10.1145/347090.347107) | The foundational Hoeffding tree is a classification algorithm. Its bounded-statistic split reasoning is not a guarantee of regression accuracy, optimal trees, or drift recovery for heavy-tailed durations. |
| [Ikonomovska, Gama & Džeroski, Learning model trees from evolving data streams, 2011](https://doi.org/10.1007/s10618-010-0201-y) | FIMT-DD directly addresses incremental regression and local adaptation. River's adaptive tree is an HAT regression adaptation, not an implementation of FIMT-DD. |
| [Bifet & Gavaldà, ADWIN, 2007](https://doi.org/10.1137/1.9781611972771.42), [HAT, 2009](https://doi.org/10.1007/978-3-642-03915-7_22) | Adaptive windows and alternate subtrees can respond to changed relationships. Add an adaptive tree and measure recovery; detector significance is not the probability a duration forecast is wrong. |
| [Grzenda, Gomes & Bifet, Delayed labelling evaluation, 2020](https://doi.org/10.1007/s10618-019-00654-y) | Label latency changes the evaluation setting. Their continuous re-evaluation is useful for revisable decisions; Harbinger instead scores the original ingress prediction because message routing is immutable. |
| [Duan, Smearing Estimate, 1983](https://doi.org/10.1080/01621459.1983.10478017) | Retransformation of a fitted log response generally does not give the conditional arithmetic mean. Score raw and log candidates in milliseconds; no smearing correction is assumed. |

For `z = log(1 + y) = m(x) + error`, the conditional mean is
`exp(m(x)) * E[exp(error) | x] - 1`, not simply `expm1(m(x))`.
Log targets may reduce heavy-tail sensitivity while increasing severe
underestimates. A correction estimated across unrelated groups or drift regimes
would need its own validation. `log1p` also does not turn an unbounded duration
distribution into a mathematically bounded one.

River HATR normalizes regression errors for ADWIN using an empirical-rule
assumption; heavy-tailed errors need not satisfy it. Its detector and alternate
subtree thresholds can delay adaptation. A label-based detector cannot react
to feedback that has not arrived. Neither its estimated memory budget nor a
depth limit guarantees a hard process-memory cap.

## Candidate implementations

The complexity order is frozen in
[`online-comparison-v1.json`](../ml_engine/configs/online-comparison-v1.json):

1. Global running arithmetic mean.
2. Fixed-vocabulary per-job arithmetic mean, with global-mean backoff.
3. Per-job exponentially weighted mean (`alpha=0.05`), with global-mean backoff.
4. Numeric-scaled online linear regression, raw duration.
5. The same linear regression with `log1p` targets and `expm1` output.
6. Mean-leaf Hoeffding regression tree, raw duration.
7. The same ordinary tree with log targets.
8. Mean-leaf adaptive Hoeffding regression tree, raw duration.

Means update with `mean += (label - mean) / count`. EWMA initializes with its
first eligible observation, then updates with
`value += alpha * (label - value)`. Job groups are bounded by the declared
vocabulary plus separate missing/unknown groups. They back off until 20 labels
have arrived for that group.

The linear model uses squared loss, SGD/intercept rates 0.01, L2 0.0001, zero
initialization and scalar loss-gradient clipping at 10. Clipping precedes feature
multiplication; it is not a bound on every parameter update. Numeric statistics
update once at eligible learning time. Categorical/missing/unknown indicators
remain unscaled. Missing numeric values use a zero placeholder and an explicit
missing indicator.

The model adapter reuses `features.encode()` and its structured namespaces.
It completes declared indicator coordinates with zeros, within a dimension cap.
This preserves inactive-vs-missing meaning for trees. Scaling an active-only
one-hot dictionary directly is incorrect: the scaler sees only ones, obtains
zero variance and can erase the category signal. Tree inputs are not passed
through changing standardization, which would change the meaning of old split
thresholds.

Trees use mean leaves, depth 4, grace period 100, split delta `1e-7`, tie threshold
0.05, minimum branch population 20, binary splits, estimated 8 MiB state and
memory checks every 100 labels. The adaptive tree additionally uses explicit
ADWIN settings, no bootstrap sampling, seed 42, and an alternate-tree comparison
threshold of 100 observations. All effective parameters are in the manifest.
Complementary one-hot features can tie in split merit; these conservative
delta/tau settings may need several thousand observations to resolve the tie.
Readiness at 100 labels therefore does **not** mean tree growth or convergence.

## Delayed replay and labels

`TemporalDataset` validates exact-time exports against their audit events. It
checks immutable ingress context, message/attempt associations, complete
terminal coverage, duplicate/conflicting events, source counts, schema validity,
and the train/evaluation views. Input JSON rejects duplicate keys and nonfinite
constants. Disk-backed indexing/sorting avoids loading the complete dataset
into model memory; per-message attempts and replay pending state are bounded.

Each independent broker run starts a fresh model. At timestamp `t`, all ingress
is predicted before feedback at `t` can train the model. The controller stores
features, prediction or fallback, update ordinal and reporting cohorts at
ingress. When an eligible successful Ack label arrives, it scores that stored
prediction **before** learning. It does not recompute an earlier prediction,
fit preprocessing on future observations, merge unrelated clocks, or use oracle
costs/identities/outcomes as model inputs.

The temporal export groups all attempts by message. The comparison learns only
train/evaluation labels admitted by that split; embargoed and unresolved labels
cannot be recovered from audit rows and quietly added to training. All evaluation
ingress is predicted, including messages with no eligible successful label.
Failed, censored, invalid or missing measurements remain separate audit
dimensions. Accepted settlement replays cannot supply a second update. Zero
runtime is valid integer-millisecond timer truncation, and participates in MAE,
RMSE and bucket metrics; only the severe-underestimate ratio excludes zeros.

Publication after writer admission is the issue-#4 availability proxy. It does
not measure durable-file or actual learner-consumption latency. Extra 10/100 ms
availability-delay replays provide declared sensitivity checks, not a claim
that these are production delay measurements.

## Readiness, fallback and bucket compatibility

- Readiness requires 100 unique eligible successful labels in that run.
- Cold start, absent complete features, unknown schema/preprocessing version,
  malformed snapshots, or nonfinite/negative decoded predictions produce an
  explicit default-priority fallback with no fabricated duration.
- An individually missing header is still a valid model input; unseen categories
  use the declared unknown coordinate. Category backoff is reported independently
  of routing fallback.
- The synthetic bucket policy is `synthetic-duration-3-10-v1`, three levels,
  boundaries `[3,10]` ms and default priority 1. Equality enters the next bucket.
  It is separate from the contract's illustrative `[10,100]` policy.
- A one-level policy requires empty boundaries and default priority 0. All policies
  validate 1–255 levels and exactly `levels - 1` finite, positive, increasing
  boundaries.
- Schema, preprocessing/model configuration and bucket policy have separate
  identities. Each model state version includes configuration/run-scope hashes
  and its update ordinal; selection
  identifies a configuration, not a shared trained checkpoint across independent
  experimental runs.

## Frozen collection and selection

The full matrix uses informative uniform/bimodal/capped-heavy-tail workloads,
uninformative bimodal/heavy-tail controls, and a heavy-tail relationship reversal
at message index 1400. Each cell has five seeds, two repeats and 2,000 messages,
for 60 initial trials / 120,000 offered messages. All use production-static
disabled routing, one worker, no TTL/failure injection, aging off and a 60-second
drain. Existing calibrated half-capacity arrival rates are fixed across candidates.
The host is not pinned or otherwise isolated, and costs are sleep-based.

Export at a 50% planned-arrival cutoff. Primary metrics come from the evaluation
half; earlier ingress provides cold-start/warm-up diagnostics. The shift cohort
has a stable evaluation prefix and a post-reversal suffix. Development fixtures
use seeds 101/202, distinct from final seeds 11/22/33/44/55.

Primary evidence requires five clean seeds, two repeats and at least 500 eligible
evaluation labels per run. Full drains, all offered/accepted/successful messages,
zero feedback/observation loss, healthy writer/consumer counters and a valid
learning export are required. `ml_engine.prepare` preserves invalid trials and
uses at most two coverage-only replacements per missing pair. The existing
runner indexes repeats from zero, so a replacement for repeat 1 also retains an
unused repeat 0; the matching repeat alone is admitted. Predictor quality is
never consulted for replacement admission.

Selection chooses the first candidate in complexity order passing **every**
required cell. Warm MAE and severe-underestimate point estimates must each be
no worse than both arithmetic baselines on identical eligible populations,
thereby enforcing the stronger baseline separately for each metric. Complete
warm prediction coverage is required: invalid forecasts cannot make a model
look better by removing difficult labels.

Paired uncertainty averages matching repeats within seed, then bootstraps the
five seed-level deltas with 2,000 resamples and seed 42. Point-estimate acceptance
does not assert statistical superiority. MAE/RMSE in ms, bucket confusion and
accuracy, per-job errors, warm-up populations, actual bucket support, fallback
coverage, delay sensitivity and shift windows remain visible for all candidates.
Unsupported populations are unevaluable, not passes.

Resource gates are warm end-to-end prediction P99 ≤ 1 ms and accounted model
state ≤ 32 MiB. Three separate profiling replays per candidate/run verify the
same numerical predictions/quality. Timing includes validation, encoding,
adapter, model and bucket/fallback handling, excluding disk/report I/O. Report
updates separately. Model memory uses cycle-safe Python ownership size plus a
serialized-state proxy for hidden native state; it is **not a hard allocator
bound**. Pending replay memory and process peak RSS (including imported libraries
and all candidates) are separate. Host timing and memory are empirical results,
not CI pass/fail speed tests.

The report also retains fixed 100-evaluation-ingress windows to expose adaptation
trajectories rather than only whole-run averages. Feedback deduplication and
completed-message identity sets are each bounded at four times the configured pending limit and are
accounted separately from model state; exceeding either replay limit fails the
comparison explicitly.

`selected`, `no_qualifier` and `incomplete_evidence` are distinct decision states.
A failed quality/resource gate is not waived by a favorable result in another
cell. A no-qualifier outcome leaves static routing as the justified operating
choice and records what to investigate next.

## Evidence and limitations

Versioned results and a checksum inventory of the raw archives (archives untracked) are published under
[`benchmarks/results/issue5-v1/`](../benchmarks/results/issue5-v1/).
They record the complete configuration, dependencies/runtime, dataset/schema
fingerprints, source hashes, candidate versions/parameters, comparisons and
decision. The retained v1 result is **`no_qualifier`**: all eight models passed
resource/coverage criteria, but each failed at least one required quality gate.
The clean admitted matrix includes 60 runs and 60,000 evaluation labels, with
coverage-only replacements of the lossy initial trials. No live model is selected.

Training is success-conditioned and measured on the benchmark host. Failures,
TTL censorship, other consumer hardware, real payload/metadata variation, and
durable ingestion delays can change the target population. The fixed 16-byte
payload contributes no varying size signal in these workloads. Synthetic
evidence therefore does not establish generalization to real workloads or
authorize predictive broker activation.
