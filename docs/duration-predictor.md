# Per-key duration predictor

Issue #6 provides the in-process C++ predictor used for tier assignment. It is a lookup over
per-key statistics, not a trained model: no Python, RPC, or classifier service is on the serving
path. It is **not yet connected to the broker**; `route_message()` integration, shadow mode and
Ack-time learning belong to issue #7, and this change leaves the broker's behaviour untouched.

## Model

- **Key.** The queue/topic name or an optional producer-supplied `job_type` header. The predictor
  takes a plain string; deriving it is the caller's job. Empty or over 256 byte keys are rejected.
- **State.** A decaying duration histogram per key plus one global histogram, all with an identical
  fixed layout: `[0, min_ms)`, then geometric bins up to roughly `max_ms`, with the last bin open-ended (defaults 48 bins, 0.1 ms to 600 s).
  Decay is O(1) (growing increment weight, renormalised before overflow). Fixed layouts make
  replays deterministic.
- **Prediction.** Summary statistic (median, p75 or mean) of the key's histogram, mapped to a tier
  by the published boundary snapshot. Equality with a boundary enters the next tier.
- **Fallback to the default tier** (no estimate returned): no snapshot yet (`unready`), fewer than
  `min_samples` observations (`cold_key`), p90/p50 above `max_spread_ratio` (`high_spread`), or an
  invalid key. A key not tracked because of the cap is predicted from the shared overflow bucket,
  under the same cold and spread rules.
- **Boundaries.** Quantiles `i / num_levels` of the global histogram. The first snapshot is published
  when `global_min_samples` is reached, then recomputed every `boundary_refresh_every` global
  observations and swapped atomically as an immutable `shared_ptr`. A boundary only moves when it
  differs from the published value by more than `hysteresis`; degenerate (non-increasing) results
  are discarded. Each key also keeps its current tier while its estimate stays within the hysteresis
  band of the neighbouring boundaries.
- **Learning.** `observe(key, duration_ms)` accepts finite, non-negative durations (zero is valid).
  Callers must feed only accepted successful-Ack durations; failures, retries' replays and
  fabricated lease-expiry durations never reach it.

## Bounds and concurrency

- At most `max_keys` tracked keys over `shards` shards. A full shard evicts its least recently
  updated key only if idle for `idle_eviction_observations` shard observations; otherwise the new
  key's samples go to the single overflow bucket. `memory_bound_bytes()` states the implied
  histogram/key bound (not process RSS). Tests drive 100,000 distinct keys through a 64-key cap.
- Locks are per shard, one global histogram lock and one overflow lock, and are never nested.
  `predict` takes one shard lock briefly and reads the snapshot lock-free. None of them is a queue,
  settlement or I/O lock, so the predictor is safe to call from `route_message()` and from Ack
  handling. Unit tests run clean under ThreadSanitizer and AddressSanitizer.

## Interface

`DurationPredictor` (`predict`, `observe`, `model_version`) is the only surface the routing hook
needs; a richer model (issue #24) can implement it without touching `route_message()`.
`PerKeyPredictorConfig` documents every parameter. The summary statistic and spread limit are
chosen by the pre-registered matrix (#23), so the defaults here are not tuned results.

## Replay CLI and Python scoring

`harbinger_predictor_replay --export DIR` reads the temporal export's `events.jsonl`, replays each
independent run with a fresh predictor in chronological order (ingress before equal-time feedback),
predicts at ingress, and learns only from eligible train/evaluation labels. It writes one
`prediction` line per ingress (same fields as the Python replay's emitted predictions) and one
`summary` line per run (final boundaries, key count, evictions, overflow). `--key-header NAME`
selects the key from the ingress features (default `job`); an empty value uses one constant key.
All predictor parameters are flags; `--help` lists them.

`ml_engine/cpp_replay.py` runs the binary and scores its predictions with the existing `Quality`
metrics, so the offline harness scores the same code that will run in the broker. Tier
boundaries are learned, so actual durations are bucketed with each run's final published
boundaries rather than the frozen `[3, 10]` ms policy.

The CLI is built by default as `harbinger_predictor_replay`; CTest runs the Python tests with
`HARBINGER_PREDICTOR_REPLAY` pointing at it.

## Not claimed

These tests show the predictor is correct, bounded and race-free. They do not show it improves
scheduling; that is for the v2 matrix (#23, #10). On the synthetic workloads a per-key lookup is
close to the best possible predictor by construction.
