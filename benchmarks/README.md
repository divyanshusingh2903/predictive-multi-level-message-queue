# Harbinger benchmarks

## Phase 2 synthetic baselines (issue #4)

The harness replays finite seeded workloads through a real embedded loopback gRPC
broker. Python 3.10+ orchestration/export uses only the standard library. Build:

```bash
cmake -B build-benchmarks -DCMAKE_BUILD_TYPE=Release \
  -DHARBINGER_BUILD_BENCHMARKS=ON -DHARBINGER_WARNINGS_AS_ERRORS=ON \
  -DHARBINGER_REQUIRE_PYTHON_TESTS=ON
cmake --build build-benchmarks --parallel
ctest --test-dir build-benchmarks --output-on-failure
python3 -m benchmarks.evaluate --config benchmarks/configs/smoke-v1.json \
  --runner build-benchmarks/benchmarks/harbinger_synthetic_benchmark \
  --output /tmp/harbinger-smoke
python3 -m benchmarks.export_feedback --run /tmp/harbinger-smoke \
  --output /tmp/harbinger-dataset
```

Output roots must be fresh. The complete matrix uses `configs/baselines-v1.json`;
`configs/calibration-v1.json` measures finite-backlog disabled capacity first.
Absolute rates are frozen from that evidence, shared across paired policies,
and never rescaled by each policy's throughput. Expect the full sequential
matrix to take tens of minutes. [Baseline evidence and experiment history](results/issue4-v1/README.md)
and [frozen numerical budgets](configs/budgets-v1.json) are retained in the repository;
the final candidate comparison/activation decision belongs to issue #10.

### Policies and isolation

| Policy | Broker behavior |
|---|---|
| `fifo` | Assign everything to L0; one logical FIFO ignores metadata tiers |
| `static` | Frozen `class0→0`, `class1→1`, `class2→2` map; strict-priority FIFO levels |
| `round_robin` | Same map; start cursor at L0, scan cyclically for a nonempty level, serve one head, advance cursor |
| `disabled` | Existing all-at-`default_priority` production routing |
| `disabled_off` | Same disabled control without persistent feedback; features/timing instrumentation remain enabled |

All policies share queue nodes, TTL indexes, aging, lease/settlement/retry/DLQ
code, and client RPCs. Priority assignment occurs only in `route_message()`.
Retries reset to original priority; aging resets placement time; neither
reclassifies. Cancellation front restoration does not rewind the round-robin
cursor or concurrent deliveries. Consumers never select or receive tiers.

The alternate server library compiles the same sources with target-scoped
`HARBINGER_BENCHMARK_SUPPORT`. Only that library exposes benchmark options and
bounded observation hooks. Ordinary server/demo/client binaries have no policy
RPC/CLI or benchmark configuration. Never link both server variants into one
executable. The queue's strict selection remains its default. CI runs the fast
correctness/pipeline tests, not a benchmark timing gate or the full matrix.

### Workloads and replay

`workloads.generate()` produces versioned `trace-v1` TSV columns:
`sequence, arrival_us, cost_us, job, failures, abandons`. This is an **oracle
artifact**, consumed only by benchmark workers, never a learner input. The
generator chooses latent job context first, then samples a conditional cost:
uniform integer bands retain a uniform marginal; bimodal is 80% short
`[min, 2×min]` and 20% long `[max(2×min+1, 0.8×cap), cap]`; heavy-tail is Pareto
shape 1.4 with a finite cap, conditional on three equal-probability latent
quantile bands. Class metadata carries context, not a measured runtime. Shift
reverses the feature-to-cost relationship at a declared index. Uninformative
metadata uses an independent random stream and retains identical arrivals/costs.
Failure/abandonment draws are independent; optional bursts are open-loop.

RPC payloads are always 16 zero bytes. Only `job` is allowlisted in
`synthetic-job-v1`; the fixed sequence header supports worker-side oracle lookup
and never enters features. Costs use sleep realization; actual callback time
includes host scheduling noise, is measured in nanoseconds, and is reported in
integer milliseconds to settlement. This measures sleep-based broker scheduling,
not CPU-bound capacity. Each run has a clean broker and an ephemeral port.

A bounded Submit pool preserves planned arrival times independently of RPC
completion. A full pending queue marks an offered message missed; it never
silently slows the trace. Results include offered/actual times, lateness above
1 ms, maximum lateness, Submit quantiles, errors, and ambiguous response loss.
Submit errors are not retried as new messages. Worker failure plans count local
handler invocations per message; raw runtime `handler_invocation` is distinct
from the broker's authoritative delivery ordinal in feedback/observations.
Settlement retries echo the same request, without running the handler again.

Warm-up is an explicit workload prefix excluded from measured cohorts, while
its backlog can affect later messages. Zero warm-up is a cold broker run;
learner readiness/restored-checkpoint experiments await the learner. The cutoff
is last planned arrival plus declared drain duration. Pending Pulls are cancelled
at cutoff; handlers/settlement finish during cleanup, but later outcomes remain
censored for that run. Shutdown seals feedback after RPC quiescence and joins
maintenance. No worker is detached.

### Metrics, artifacts, and uncertainty

The finite preallocated observation sink captures ingress, committed dispatch,
actual Ack/retry/DLQ (including queued expiry and lease reclamation), monotonic
times, and internal ordinals. Each publisher reserves a unique slot without
blocking. Snapshot it only after publishers quiesce. Observation/feedback loss
is counted and invalidates complete-coverage comparisons; delivery remains
unchanged. Accepted settlement replay/cancelled Pull emits no duplicate outcome
or dispatch. Sidecars contain no settlement tokens or arbitrary failure text.

Per-run `run.json`, `messages.jsonl`, TSV observations/submissions/runtimes,
sealed feedback, and top-level `manifest.json`/`results.json` preserve:

- nearest-rank P50/P95/P99 and sample counts for first dispatch, measured
  handler runtime, all-terminal latency, successful latency, and Submit;
- offered/accepted outcome rates, structured TTL/retry-limit DLQ, missed,
  rejected/unconfirmed arrivals, unfinished counts and censored ages;
- successful/terminal throughput over the common window from the first
  measured scheduled arrival to cutoff, plus whole-run full-drain elapsed time;
- per-class longest observed wait, longest never-dispatched censored age,
  starvation counts/rates, and younger censored cases. Enabled-aging threshold
  is `2×levels×aging.threshold`; aging-off requires an explicit threshold.
  Never-dispatched terminal messages use their disposition age, not the later
  cutoff, and are included in fairness accounting;
- whole-trace conservation (including warm-up), revision/dirty flag, source,
  executable/config/trace hashes, machine/runtime/dependency/build versions,
  explicit broker settings, seeds, and losses.

No terminal timestamp is invented for unfinished work. Queue/in-flight snapshots
are approximate concurrent diagnostics; observation-based message inventories
are the cutoff accounting authority. Primary gate runs require complete drains.
The small published feasibility matrix reports actual P99 sample sizes, not a
claim that these samples suffice for final candidate activation.

Policies run in seeded randomized order, sequentially on the same host. Pair
matching repeats within each seed, average repeat deltas, then compute seeded
percentile-bootstrap 95% intervals over seed means (2,000 resamples, seed 42).
Report insufficient paired seeds and undefined zero-denominator ratios as
unevaluable. All trials, including invalid ones, remain in raw artifacts;
invalid trials are excluded from complete-coverage intervals. Class maxima and
starvation deltas have the same paired procedure as latency and throughput.

Compact publication keeps every raw trial summary, trace/raw-artifact hashes,
class maxima/counts, manifest, and intervals, without committing feedback logs:

```bash
python3 -m benchmarks.evaluate --publish-from /tmp/harbinger-baselines-v1 \
  --output benchmarks/results/issue4-v1/baselines
```

`benchmarks/results/issue4-v1/` is versioned, including every trial summary and
compressed snapshots of raw experiments and dataset exports. See the
[archive guide](results/issue4-v1/raw/README.md) for checksums and extraction.
Other generated result directories and `benchmarks/output/` remain ignored;
publish subsequent evidence in a new explicitly versioned directory. Evidence
consistency, generated pipeline, and scheduler correctness checks run in CI.

### Leakage-safe static feedback export

`export_feedback --run` selects **disabled production static-mode collection**,
validates sealed v1 records, deduplicates event IDs, verifies routing consistency,
and joins exact observation times. SQLite supplies bounded-memory indexing/sort.
Group all attempts by broker-instance/message; queued expiry remains message-level.
Source schemas, trace/sidecar hashes, telemetry coverage, and missingness are
recorded. Successful `eligible` Ack labels alone enter train/evaluation label
streams. Failed, TTL-censored, negative/outlier, unusable, and missing observations
remain explicit audit dimensions; these can overlap.

The split defaults to 60% of the declared arrival trace. Training requires both
ingress and **all observed outcome availability** strictly before the cutoff;
boundary-crossing message groups are embargoed. Evaluation ingress and delayed
labels are separate streams ordered within each independent run's clock scope.
Ingress precedes equal-time feedback;
availability uses broker publication after the writer's admission attempt, not
the earlier state-transition time. Actual durable-file/learner consumption delay
is not modeled and must be measured when that pipeline exists. Lossy admission
invalidates complete-coverage learning comparisons. Benchmark transition and
publication times are both retained in audit records;
future learners must evaluate the stored ingress prediction before learning and
consume only strictly earlier labels. Model inputs contain immutable ingress
features only, never outcome/retry/identity/oracle fields. Unknown chronology is
unresolved, and success-only selection bias is reported.
Evaluation ingress remains present even when its eventual label is missing,
censored, or unfinished; uncertain label times are excluded from learning and
reported independently, never used to select a favorable ingress population.

For older static logs, register schemas and the recorded lease explicitly:

```bash
python3 -m benchmarks.export_feedback --feedback /path/to/sealed-feedback \
  --schema /path/to/schema.json --lease-ms 30000 --output /tmp/feedback-audit
```

This produces audit views only, with `valid_for_learning=false`: feedback v1's
whole-second UTC and asynchronous publication cannot establish exact global
delayed-label chronology. Active/suspect files are excluded; malformed complete
records, incomplete tails, conflicting IDs, unsupported versions, and unapproved
features are errors, not silently skipped training data.

## Phase 1 cleanup measurements

Build explicitly; this target is excluded by default and has no CI timing gate:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DHARBINGER_BUILD_BENCHMARKS=ON
cmake --build build --target harbinger_cleanup_benchmark --parallel
./build/benchmarks/harbinger_cleanup_benchmark queue 100000 50000 4 young
./build/benchmarks/harbinger_cleanup_benchmark queue 100000 50000 4 mixed
./build/benchmarks/harbinger_cleanup_benchmark queue 100000 50000 4 dense
./build/benchmarks/harbinger_cleanup_benchmark queue 100000 50000 4 off
./build/benchmarks/harbinger_cleanup_benchmark delivery 1000 65536 64 4
```

Queue workers dequeue and replace work to maintain backlog; reported latency is
one dequeue/placement pair. `young` uses a one-hour threshold, `mixed` backdates
one placement in 100, and `dense` backdates every placement. Backdated placements
use front restoration, exercising timestamp-order exceptions. Due populations
change as aging promotes work; these are workload labels, not measured fixed
due ratios. The minimum observed depth and final depth show whether backlog was
maintained. Aging-disabled runs provide a contention comparison. Small runs may
finish before an aging interval: use enough iterations to observe several scans.

Delivery mode uses an embedded loopback broker, a single producer and the chosen
number of consumers. Payloads are generated before timing with seed 42. Latencies
measure from Submit start to handler entry, including queue wait; throughput is
confirmed Ack outcomes divided by elapsed time. Handlers add no simulated delay.
The requested application header count excludes producer/sequence metadata.
Client registration/setup and shutdown are outside the measured interval.

Compare identical Release builds, compiler/dependency versions, machine load,
parameters, and several repetitions before and after changes. Try queue backlogs
1k/10k/100k and delivery payloads 0/1KiB/64KiB/1MiB, headers 0/8/64, workers 1/4.
Record all results, including regressions; wall-clock speed is not a correctness
assertion. `/usr/bin/time -v` can give coarse whole-process memory usage. This
harness does not isolate allocations or lock hold times and is not a substitute
for Phase 2/3 scheduler comparisons or production trace benchmarks.

## Local comparison for issue #14

Measured on Linux x86-64 with GCC 15.2.0, Release (`-O3`), the same Conda
gRPC/Protobuf installation, and three sequential repetitions per scenario.
Baseline used broker/queue code at `efb3638` plus this harness; comparison used
the #14 working-tree changes. Queue parameters were `100000 50000 4 PATTERN`;
delivery parameters were `1000 65536 64 4`. Queue depth stayed between 99,996
and 100,000 in every run. Values below are medians of the three run summaries,
not percentiles computed across pooled samples.

| Scenario | Baseline outcomes/s | After outcomes/s | Baseline P50 / P99 (µs) | After P50 / P99 (µs) |
|---|---:|---:|---:|---:|
| Queue, young | 605,301 | 681,836 | 3.425 / 24.004 | 4.076 / 24.043 |
| Queue, mixed | 576,206 | 560,933 | 3.647 / 24.320 | 3.621 / 24.292 |
| Queue, dense | 540,243 | 593,032 | 3.476 / 25.528 | 4.575 / 26.329 |
| Queue, aging off | 722,300 | 695,556 | 3.508 / 24.675 | 3.916 / 23.214 |
| Loopback delivery | 2,387.63 | 2,409.94 | 630.733 / 840.420 | 600.403 / 808.892 |

Throughput ranges across baseline/after runs, respectively: young
569,796–623,375 / 633,352–701,007; mixed 570,875–587,053 /
520,136–564,870; dense 535,223–551,906 / 578,198–596,742; aging-off
702,788–730,346 / 680,699–720,768; delivery 2,386.57–2,416.81 /
2,384.94–2,423.56.

These short, unpinned runs are evidence of workload-dependent trade-offs, not
statistically established speedups. In particular, mixed throughput regressed,
dense P50/P99 increased, and the aging-off control also varied. Young-level
skips reduce scanning work by construction (covered by deterministic visit-count
tests), but do not guarantee lower latency on every workload. Delivery throughput
was essentially unchanged; its measured latency improved modestly. Use longer,
repeated runs on a controlled machine before making capacity claims.

## Production-flow benchmark

`harbinger_prodflow` (`benchmarks/prodflow.cpp`) runs an e-commerce background-job queue: five services as separate
producers, nine job types doing real CPU work plus simulated network calls, four workers, an embedded broker. Arms:
`fifo`, `static_tuned`, `static_misconfigured`, `shadow`, `predictive`, `predictive_p75`, `predictive_producer`,
`predictive_sized` (size-binned keys from the `size_hint` header that `resize_image`, `generate_invoice` and
`export_report` producers send) and `oracle`. Run a frozen matrix with
`PRODFLOW_CONFIG=benchmarks/configs/<config>.json benchmarks/run_prodflow.sh build/benchmarks/harbinger_prodflow <raw dir> <seeds>`
and analyse it with `python -m benchmarks.prodflow_report <config> <raw dir>`. Pre-registrations:
[v2](../docs/v2-preregistration.md) (`v2-prodflow-1`) and [size bins](../docs/v3-sizebins-preregistration.md)
(`v3-sizebins-1`).
