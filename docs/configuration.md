# Standalone server configuration

`harbinger_server` reads an optional version-1 JSON file with `--config PATH`. Everything else keeps its
existing command-line form. Examples live in [`examples/`](../examples/).

```bash
./build/harbinger_server 0.0.0.0:50051 --config examples/harbinger-shadow.json
./build/harbinger_server --routing-mode shadow --stats-interval-ms 10000   # no file needed
./build/harbinger_server --config prod.json --routing-mode static           # one-step rollback
```

## Precedence and validation

1. Built-in defaults (`HarbingerConfig`; the standalone server enables aging at 5000/500 ms).
2. The `--config` file, wherever it appears on the command line.
3. Every other option, including the positional listen address, `--routing-mode` and the recovery flags.

Validation happens before the server listens; any error exits nonzero with a message naming the file and
field. The loader rejects: files over 1 MiB, malformed JSON, trailing data, duplicate keys, unknown fields,
`config_version` other than 1, wrong types, out-of-range values, and anything the broker itself rejects
(for example `feedback` without `features`, `default_priority >= num_levels`, invalid feature schemas,
`mode: "disabled"`). Nothing is applied from a rejected file. There is no runtime reload.

A section that is **absent** keeps the current value; a section set to **`null`** disables it explicitly
(`"routing": null`, `"feedback": null`, `"features": null`, `"broker": {"aging": null}`).

## Fields

| Field | Type | Meaning |
|---|---|---|
| `config_version` | `1` | Required. |
| `listen` | string | Listen address; a positional address on the command line wins. |
| `stats_interval_ms` | integer | Print `[harbinger] stats {json}` every N ms (0 = off). |
| `broker.num_levels`, `default_priority`, `default_max_retries`, `default_ttl_ms`, `max_pull_wait_ms` | integers | As in `HarbingerConfig`. |
| `broker.aging` | object or null | `threshold_ms`, `interval_ms`; `null` = strict priority. |
| `broker.delivery_lease_ms`, `lease_sweep_interval_ms`, `completion_retention_ms`, `completion_cache_max_entries`, `maintenance_batch_size`, `ttl_sweep_interval_ms` | integers | Recovery settings, same rules as the flags. |
| `features.schema_version`, `features.routing_policy_version` | strings | Explicit immutable identities; nothing is generated. |
| `features.headers[]` | objects | `name`, `type` (`numeric` with `minimum`/`maximum`, or `categorical` with `encoding` `vocabulary` + `vocabulary` list, or `hash`). Validated by the broker's own schema code, shared with Python. |
| `feedback.directory` | string | Existing dedicated directory, owned exclusively by this broker. Optional `buffer_records`, `buffer_bytes`, `segment_bytes`, `retention_bytes`, `max_segments`, `retention_age_ms`, `sync_interval_ms`, `shutdown_drain_ms`. Requires `features`. |
| `routing.mode` | `shadow` or `predictive` | Omit the section (or set it to `null`) for static routing. |
| `routing.routing_policy_version` | string | Default `per-key-v2`. |
| `routing.key.job_header` | string or null | Header naming the job type (default `job_type`); `null` keys by producer only. |
| `routing.key.scope_by_producer` | bool | Default `true`; see [the predictor doc](duration-predictor.md#keys). |
| `routing.predictor.*` | | Every `PerKeyPredictorConfig` field: `summary`, `min_samples`, `spread_quantile`, `max_spread_ratio`, `max_censored_fraction`, `decay`, `global_decay`, `time_half_life_ms`, `stale_after_ms`, `histogram_bins`, `min_ms`, `max_ms`, `max_keys`, `shards`, `idle_eviction_ms`, `cold_eviction_grace_ms`, `boundary_refresh_every`, `global_min_samples`, `hysteresis`. `num_levels`/`default_priority` always come from `broker`. |
| `routing.snapshot.path`, `interval_ms` | string, integer | Predictor state file, rewritten atomically every interval and at shutdown. |

## Feedback collection without a predictor

`examples/harbinger-collect.json` enables typed feature capture and persistent feedback with static routing,
no Python and no predictor. The feedback directory must already exist and belong to this broker alone; see
[feedback.md](feedback.md) for retention, loss accounting and what persistence does not guarantee. Only
allowlisted headers are recorded; payloads never are. Remove the `feedback` section (or set it to `null`) to
stop collecting.

## Routing modes and rollback

- **static** (default): no predictor, no prediction cost.
- **shadow**: predicts and learns, records the prediction in the routing context and feedback, but routes to
  `default_priority`. Dispatch order is identical to static routing (tested).
- **predictive**: routes by the predicted tier; every fallback uses `default_priority`. Opt in only after the
  benchmark gate (see the Phase 2 report).

Rolling back routing is one step: restart with `--routing-mode static` (or `shadow`). Rolling back predictor
state is separate: stop the broker, then either delete the snapshot (cold start) or copy `PATH.prev` over
`PATH` (previous snapshot). A snapshot written under a different key policy or routing version, a corrupt one,
or one with another histogram layout is rejected at startup and the broker starts cold; the startup log says why.

## Stats line

`--stats-interval-ms N` prints queue/in-flight/DLQ sizes, feedback counters and, with a predictor:
lookups, routed count, outcome counts per fallback reason, lookup latency, learned/censored labels, rolling tier
agreement and log2 error, drift alerts, key count, overflow, evictions and snapshot saves/failures. It never
contains message ids, payloads or key values.
