# Per-key duration predictor

Issue #6 provides the in-process C++ predictor used for tier assignment. It is a lookup over
per-key statistics, not a trained model: no Python, RPC, or classifier service is on the serving
path. Issue #7 connects it to the broker (see "Broker integration"), #8 adds snapshots and drift
monitoring, and [configuration.md](configuration.md) covers enabling it from the standalone server.

## Keys

The broker is one logical queue and the protocol has no topic or queue name, so there is no
"queue name" key. `derive_predictor_key()` forms the key from fields the broker already has:

- **Default: the producer id.** Broker-assigned, set after client headers are copied (a client
  cannot forge it), and requires a registered producer. Zero configuration, but a producer that
  mixes cheap and expensive jobs gets one key and will usually fall back (see the gates below).
- **Optional: `job_type` header**, joined to the producer id (`producer-1\x1fresize`). Labels
  are therefore scoped per producer, so a producer cannot claim another producer's fast history to
  jump the queue; lying about the label only pollutes that producer's own key. Labels longer than
  128 bytes or containing control characters are ignored and the producer key is used.
  When ingress features are enabled the header must be on the feature allowlist.
- `PredictorKeyPolicy::scope_by_producer = false` shares a label across producers (faster
  learning for several instances of one service) and is only appropriate when producers are
  trusted.
- Replay exports carry features, not producer ids, so the CLI takes `--key-header` (default `job`).

Without a `job_type` header nothing distinguishes a producer's job types, so "no configuration" in
practice means "one key per producer". Finer keys need a header or a size bin.

### Size bins (issue #35)

The #24 measurement (`feature-signal-v1`) found that, for jobs whose cost follows their input size, a per-key median
is off by a median factor of about 1.95 while a per-key, per-log2-size-bin median is off by 1.2 (68% → 93% tier
accuracy); a gradient-boosted model over every feature added almost nothing more. `PredictorKeyPolicy::size_source`
therefore adds an optional bin to the key; the default (`None`) changes nothing.

- **`Payload`**: the size is the payload length in bytes. Useful when the payload *is* the input.
- **`Header`**: the size is a numeric header named by `size_header` (for example `pixels` or `rows`), for jobs whose
  payload is only a reference to the real input. The value must be a plain non-negative finite decimal of at most
  32 characters; anything else (missing, `12px`, `-4`, `nan`) leaves the key un-binned rather than rejecting the
  message. The name must not start with `__`, and when ingress features are on it should be on the allowlist like
  `job_type`.
- The bin is `s` + `floor(log2(max(size, 1)))` (`s0`…`s63`), appended as `key\x1fsN`. A key that would exceed
  `kMaxPredictorKeyBytes` with its bin stays un-binned.
- **Cold bins borrow their own key, never another's.** A binned key keeps its un-binned *parent*. When the bin is
  `ColdKey` and the parent is `Predicted`, the parent's estimate and tier are used and `parent_fallbacks` is counted;
  otherwise the bin's own fallback applies. Every label is learned by both the bin and the parent, so the parent is
  exactly the un-binned key's history; the global histogram (and so the tier boundaries) counts the label once.
  Censored attempts mark both keys.
- A size is a producer-controlled hint like `job_type`, scoped the same way: lying about it moves the message between
  that producer's own bins only. Bins multiply the number of keys by at most the number of distinct size octaves a
  key sees, so `max_keys` may need raising.
- The size policy is part of the snapshot context, so a snapshot taken under another size policy is refused (starts
  cold) rather than misread.

Size bins help only where cost follows size: the control key in `feature-signal-v1` (a network call whose latency is
unrelated to its payload) gained nothing, and a binned key needs `min_samples` labels per bin before it stops
borrowing its parent.

## Model

- **State.** A decaying duration histogram per key plus one global histogram, all with an
  identical fixed layout: `[0, min_ms)`, geometric bins up to `max_ms`, the last bin including
  everything above (defaults 48 bins, 1 ms to 600 s). Durations are integer milliseconds on the
  wire, so `min_ms = 1` puts every sub-millisecond job in one bin instead of pretending to
  resolve it. Longer durations are clamped to `max_ms` (counted as `saturated`) so the median
  and mean agree. Decay per observation is O(1) (growing increment weight, renormalised before
  overflow); a wall-clock half-life additionally shrinks a key's history across idle gaps.
- **Prediction.** Summary statistic (median, p75 or mean) of the key's histogram, mapped to a tier
  by the published boundary snapshot. Equality with a boundary enters the next tier.
- **Fallback to the default tier** (no estimate returned), in this order: `unready` (no snapshot
  yet), `invalid_key`, `cold_key` (fewer than `min_samples`, including every unseen or untracked
  key), `stale_key` (idle longer than `stale_after`), `censored`, `high_spread`.
- **Tail gate.** `high_spread` compares `spread_quantile` (default p99) with the median. A p90
  gate cannot see a minority of very long jobs: a key that is 92% 3 ms and 8% 3000 ms looks
  unremarkable at p90 and was routed to a fast tier (verified before the fix, now covered by a
  test). Misrouting a long job into a fast tier is the costly error, so the gate looks at the tail.
  Genuinely heavy-tailed but stable keys (p99/p50 above 16) also fall back; relax
  `max_spread_ratio` if that proves too conservative on real data.
- **Censoring.** A delivered attempt that overran its lease yields no duration. Dropping those
  would bias a slow key toward "fast", so `observe_censored()` records them and a key whose decayed
  censored share exceeds `max_censored_fraction` (5%) falls back. Only delivered attempts count:
  a message that expired in the queue says nothing about its handler. The replay CLI calls it for
  `lease_expiry` events that carry an attempt. The broker must do the same in #7.
- **Staleness.** A key idle longer than `stale_after` (30 min) is not predicted and its history is
  discarded on its next observation, so a regime change while a key was quiet (or starved in a slow
  tier and unmeasured) is relearned from scratch rather than blended with old evidence.
- **Boundaries.** Quantiles `i / num_levels` of the global histogram. The first snapshot is
  published when `global_min_samples` is reached, then recomputed every `boundary_refresh_every`
  global observations and swapped atomically as an immutable `shared_ptr`. A boundary only moves when
  it differs from the published value by more than `hysteresis`; degenerate (non-increasing)
  results are discarded. Each key also keeps its current tier while its estimate stays within the
  hysteresis band of the neighbouring boundaries (tested with and without the band).
- **Learning.** `observe(key, duration_ms)` accepts finite, non-negative durations (zero is valid).
  Callers must feed only accepted successful-Ack durations; failures, retries' replays and
  fabricated lease-expiry durations never reach it.

## Bounds and concurrency

- At most `max_keys` tracked keys over `shards` shards. A full shard recycles its least recently
  updated key only if it is idle: a proven key (at least `min_samples`) after `idle_eviction`, an
  unproven one after the shorter `cold_eviction_grace`. Otherwise the newcomer's observations are
  counted as `overflow_observations` and are **not** predicted from: an untracked or unseen key is
  always a cold key. The grace stops a flood of one-off keys from locking real keys out for long,
  and stops a working set larger than the cap from thrashing (the first keys keep their slots and
  learn). `memory_bound_bytes()` states the implied histogram/key bound (not process RSS).
- Locks are per shard plus one global histogram lock and are never nested. `predict` takes one
  shard lock briefly. The boundary snapshot is an `std::atomic<std::shared_ptr>`, which libstdc++
  implements with a short internal lock rather than lock-free atomics; it is brief and never nested
  with the others. None of these is a queue, settlement or I/O lock. A single-thread probe measured
  roughly 115 ns per `predict` and 60 ns per `observe`. Tests run clean under ThreadSanitizer and
  AddressSanitizer.

## Threat model

- Producers cannot choose another producer's key (above). A producer can still influence its own
  tier by how it labels its own work; the learned history, not the label, decides the tier.
- Consumers report `processing_time_ms`, accepted by the broker between zero and the lease. A
  consumer that under-reports can drag a key's estimate down; the median and the tail gate limit a
  minority's effect but a consumer that owns most of a key's traffic controls it. Measurement
  authenticity is outside this change.
- Resolution is one millisecond. Differences below that are invisible to the predictor.

## Broker integration (issue #7)

`HarbingerConfig::predictive_routing` (absent = static routing, no predictor) selects `shadow` or
`predictive`. `route_message()` stays the only place that resolves TTL and routing:

- It derives the key, calls `predict()`, and records the mode, model version (`per-key-v2;boundaries=N`),
  estimate, predicted tier, fallback reason and lookup time in the immutable routing context. The key itself is
  kept broker-internal for learning and is never serialized into feedback or sent to consumers.
- **Shadow** sets `priority` and `original_priority` to `default_priority`; dispatch order is identical to static
  routing. **Predictive** sets both to the predicted tier, and to `default_priority` on any fallback.
- Retries and lease requeues restore `original_priority`; nothing is reclassified, and later learning never
  changes an existing message's context. Aging is unchanged. Consumers receive no tier, estimate or key.
- Learning happens after the settlement lock is released: an accepted Ack within the lease and TTL (and with
  valid features when capture is on) calls `observe()`; a lease expiry calls `observe_censored()`; Nack,
  Ack-after-TTL, out-of-range durations, rejected or stale tokens and replayed settlements teach nothing.
- `predictive` cannot be combined with a benchmark ingress policy (rejected at construction).
- New feedback fallback reasons: `cold_key`, `high_spread`, `censored`, `stale_key`, `invalid_key`
  (C++ and the Python validator agree; v1 exports stay valid).

## Snapshots and monitoring (issue #8)

- `save()` writes a versioned binary snapshot (magic, version, byte order, key-policy/routing context,
  histogram layout, boundaries, global and per-key histograms, key ages, checksum) to a temporary file, fsyncs,
  renames over the old file and keeps the previous one as `PATH.prev`. `load()` validates everything before
  changing state; a corrupt, truncated or incompatible snapshot throws and the broker starts cold, reporting the
  reason. Key ages are stored relative to save time, so staleness survives restarts.
- The broker loads at startup, saves every `snapshot_interval` from the maintenance thread (predictor locks
  only) and once more at shutdown.
- `routing_stats()` exposes bounded counters: lookups, outcomes per status, lookup-latency buckets, learned,
  rejected and censored labels, and drift signals: rolling tier agreement over the last 512 scored labels,
  an EWMA of |log2(predicted / actual)|, and `drift_alerts` (agreement below 50%, latched until above 60%).
  Drift signals are informational only: they never switch the routing mode or the number of levels.
- Recovery from a relationship shift is governed by `decay`: with `d = 0.995` a key's new regime holds half the
  weight after `ln 0.5 / ln d` ≈ 138 observations, and a tested seeded shift recovers within ±20% of that.
  Idle keys additionally decay by `time_half_life` and are discarded after `stale_after`.
- Rolling back predictor state (delete the snapshot or restore `.prev`) is separate from rolling back routing
  (`--routing-mode static`); see [configuration.md](configuration.md#routing-modes-and-rollback).

## Interface

`DurationPredictor` (`predict`, `observe`, `observe_censored`, `model_version`) is the only
surface the routing hook needs; a richer model (issue #24) can implement it without touching
`route_message()`. `PerKeyPredictorConfig` documents every parameter. The summary statistic, spread
quantile and limit are chosen by the pre-registered matrix (#23), so the defaults are not tuned results.

## Replay CLI and Python scoring

`harbinger_predictor_replay --export DIR` reads the temporal export's `events.jsonl`, replays each
independent run with a fresh predictor in chronological order (ingress before equal-time feedback),
predicts at ingress, learns from eligible train/evaluation labels and records lease-expiry
overruns as censored. Time-based decay, staleness and eviction use the export's
`benchmark_time_ns`, not replay wall-clock time. Like the Python dataset it ignores an identical
repeated event and rejects a conflicting one; it keeps only the fields it needs, not whole JSON
trees. It does not re-validate the export's chronology or provenance; run the Python dataset
validation first for evidence-grade replays. It writes one `prediction` line per ingress (the Python
replay's fields plus `boundaries_ms`, the boundaries in force when it routed) and one `summary` line
per run. `--help` lists the flags. `--size-source payload|header:NAME` applies the broker's size bins (reading
`routing.features.payload_size_bytes` or `routing.features.headers[NAME]`), with the same parent fallback and paired
learning; its summary adds `parent_fallbacks`. The broker test `InBrokerPredictionsMatchOfflineReplayWithSizeBins`
checks that the broker and the CLI agree exactly on a binned stream.

`ml_engine/cpp_replay.py` runs the binary and scores its predictions with the existing `Quality`
metrics. Tiers are learned, so each label's actual bucket uses the boundaries in force at that
message's ingress; only an unready prediction (no boundaries yet) falls back to the run's final
boundaries.

## What this does not fix

These tests show the predictor is correct, bounded and race-free. They do not show it improves
scheduling, and several risks are evidence problems, not code problems:

- **Per-key statistics alone failed the issue #5 gate.** `job_mean` was worse than `global_mean`
  in the control and shift cells, so a per-key lookup is not automatically better than a global one.
  The #23 matrix must include a global-statistics arm, an uninformative-key control and a shift arm,
  and must state in advance what result would end the project.
- **The baseline evidence is small and synthetic**: 200 messages per run, one worker, duration set by
  the class label. Expect no conclusion about real traffic before the Azure-trace simulation (#25)
  and the cloud run (#26).
- **Millisecond wire resolution.** A microsecond field would need a proto and feedback-schema
  change and is not part of #6.
