# Persistent feedback for embedded brokers

Issue #3 implements optional static-mode feedback collection without Python or an ML service. It records ingress decisions and actual Ack, retry, and DLQ transitions as version-1 JSONL. Routing remains static. Feedback is telemetry, not durable message delivery or an atomic settlement/log transaction.

## Enable collection

Create an existing, writable, dedicated directory, then configure an embedded broker:

```cpp
#include "harbinger_service.hpp"

harbinger::HarbingerConfig config;
config.ingress_features = harbinger::ml::IngressFeatureConfig{
    .schema = {
        .version = "features-v1-job-units",
        .headers = {
            {.name = "job", .type = harbinger::ml::FeatureType::Categorical,
             .vocabulary = {"resize", "convert"}},
            {.name = "units", .minimum = 0, .maximum = 1000000},
        },
    },
    .routing_policy_version = "static-v1-three-levels-priority-1",
};
config.feedback = harbinger::ml::FeedbackConfig{.path = "/var/lib/harbinger/feedback"};
harbinger::HarbingerService broker{config};
```

`feedback=nullopt` is the default: no feedback thread, serialization, or filesystem access. Feature capture may be enabled without persistence; persistence requires explicit `ingress_features`. An empty allowlist captures payload size only. Schema/policy identifiers describe immutable specifications; change the identifier when changing the corresponding specification. See the [feature guide](../analysis/README.md).

Configuration is copied at construction and has no runtime reload. Standalone schema-file loading and feedback command-line options are not implemented. The storage backend supports POSIX systems (Linux/macOS).

## Capacity and timing settings

Fields are members of `ml::FeedbackConfig`:

| Field | Default | Meaning |
|---|---|---|
| `path` | Required | Existing dedicated directory, exclusively owned while writer exists |
| `buffer_records` | 4,096 | Pending record count, including the record being written |
| `buffer_bytes` | 32 MiB | Pending serialized bytes, including newlines and writer-owned work |
| `max_event_bytes` | 16 KiB | Hard JSON cap, excluding its newline; may be lowered |
| `segment_bytes` | 64 MiB | Rotate before a whole record would cross this cap |
| `retention_bytes` | 1 GiB | Total recognized segment bytes, including active/suspect files |
| `retention_age` | 7 days | Age limit for sealed/suspect files, based on file modification time |
| `max_segments` | 256 | Recognized segment count, including active/suspect files |
| `sync_interval` | 1 second | Healthy periodic `fsync` target |
| `shutdown_drain` | 1 second | Healthy drain target before dropping queued work |

All capacities/durations must be positive. `max_event_bytes <= 16384`; buffer and segment must each fit `max_event_bytes + 1`; retention must fit a segment. Duration conversion and capacity relationships are validated before starting the writer. These defaults are implemented limits, not measured performance budgets or guaranteed timing bounds. Queue metadata and temporary producer serialization have bounded per-record overhead beyond the serialized-byte budget.

Admission tries a short mutex once and drops rather than waiting on contention. Full buffers drop newest events. The writer never holds this mutex during storage operations and never acquires broker locks. Broker capture occurs before moving message state; serialization/admission happens after the successful transition and outside settlement/queue locks. Telemetry failures cannot change accepted Ack/Nack statuses or message disposition.

## Records and label semantics

The [ML contract](ml-contract.md#feedback-event-format) defines the fields. Every outcome repeats the complete immutable ingress context, so losing an ingress record does not force a join. Features reuse the deterministic feature JSON representation; unknown observations/predictions use JSON null. No payload, unapproved headers, producer/consumer IDs, arbitrary Nack text, or opaque attempt tokens are exported.

- `broker_instance_id` is a fresh independent 128-bit random telemetry namespace for each writer/broker construction.
- `event_id` is instance plus a monotonic decimal sequence. Allocation gaps and out-of-order arrival at storage are allowed; it is not a cross-thread ordering guarantee.
- `attempt_id` is a decimal-string successful delivery ordinal, starting at 1 and independent of retries. Cancelled Pull restoration consumes no ordinal. Ingress and queued expiry use null, including expiry after a previous retry.
- Counters never wrap identity: exhausted event sequences drop telemetry; exhausted delivery ordinals restore the queued message and return `RESOURCE_EXHAUSTED` without installing ownership.
- Identical accepted replay emits no second event and does not replace the original measurement. Rejected owners/stale tokens/opposite settlements have no labels. A matching late settlement that actually reclaims an expired lease emits only that reclamation, with null measurement.
- Accepted Ack-after-TTL records TTL DLQ although Ack returns OK. TTL never increments retries. Lease reclamation uses the same failure budget as Nack unless TTL wins.
- Selected-message expiry uses `pull_expiry`; both Pull-path bulk cleanup and background sweeps use `ttl_sweep`.

Labels use first-match precedence: missing observation, negative, greater than configured delivery lease, unusable complete features, TTL censorship, Nack failure, then eligible successful Ack. Zero is valid; the current proto scalar treats omitted duration as reported zero. Individual missing/invalid headers remain valid missing-value model inputs. Measurements remain consumer observations rather than trusted timing truth. The [offline learners](online-predictor.md) train only on `eligible` successful Ack labels from exact-time exports; a live log-consuming learner remains planned.

## Segment ownership, retention, and restart

The writer locks `.feedback.lock` exclusively, creates mode-0600 files named `feedback-<32-hex-instance>-<20-digit-generation>.active`, and seals healthy files to `.jsonl`. File sync and directory-metadata sync are distinct operations. `close()` joins the worker but keeps directory ownership until writer destruction. A second writer sharing the directory fails startup.

Startup preserves previous logs, quarantines stale `.active` files to `.suspect`, and opens a new instance/segment. Quarantine preserves the entire file, including its complete prefix and any incomplete tail; the writer never appends to old files or trains/replays their contents. After an append/sync/sealing failure, the affected file is similarly quarantined where possible. Recovery uses a 200 ms steady-clock backoff; it never duplicates an ambiguously written record into another segment. Failed quarantine prevents opening more segments until recovery is possible. Invalid configuration, unavailable startup storage, unsafe recognized files, or failed ownership/recovery fail construction; runtime failures isolate telemetry from delivery.

Only exact recognized segment names are managed. Unrelated files are left untouched and excluded from the storage budget: keep the directory dedicated and do not modify its files while a broker owns it. Recognized symlinks/nonregular files are rejected. Directory traversal and record counting stream with bounded memory rather than loading entire logs or an unlimited segment catalog.

Retention removes oldest sealed/suspect files when age, byte, or segment-count limits require it. Active files are never deleted; age applies after sealing, while the active file remains byte-bounded until rotation/close. Backward wall-clock changes postpone age eviction; byte/count limits remain enforced. A forward jump can immediately evict old sealed files. If deletion fails or active storage prevents meeting a cap, new telemetry is dropped and recovery is retried without unbounded disk/buffer growth. Deletion is conservatively reported as potentially unconsumed data loss; there is no learner acknowledgement or durable cursor yet.

Future readers must validate record version/JSON, reject malformed complete records and incomplete final lines, and deduplicate by event identity. Do not interpret an entire `.suspect` file as confirmed durable data: even its complete prefix may have been unsynced. Model/cursor checkpointing and at-least-once ingestion belong to later issues.

Issue #4 implements this sealed-segment reader in `analysis/feedback.py` and
[dataset export](../benchmarks/README.md#leakage-safe-static-feedback-export).
It validates registered schemas/label precedence, rejects conflicting duplicate
identities, indexes message/attempt groups in SQLite, and keeps outcome,
measurement validity, failures, and TTL censorship separate. Exact temporal
train/evaluation splits require benchmark monotonic timing sidecars; legacy v1
logs alone produce audit views with unresolved chronology. Event sequence gaps
are possible incompleteness, not an exact count of lost message labels.

## Observability and durability window

`broker.feedback_stats()` returns a thread-safe approximate snapshot (zero/disabled when collection is off):

- Captured publication attempts, admitted, written, and successfully synced records.
- Dropped records by fixed `FeedbackDrop` reason: capture, serialization, event limit, contention, buffer full, closed, storage, shutdown, or identity exhaustion.
- Pending bytes/records including in-progress storage work; writer, sync, and recovery failures; storage health and last successful sync age.
- Uncertain records after write/sync failure and retention-deleted segment/record/byte counts. Retention record counts count complete lines, not validated training labels. All counters are process-local; uncertainty can overlap written counts.

Capture failures before publication appear in the capture-drop counter, not the captured-publication count. Do not use message IDs or header values as metric labels. Learner backlog and prediction-fallback metrics are deferred until those components exist.

An OK settlement means the in-memory broker accepted it, not that its feedback was admitted, written, or synced. Crashes can lose capture-to-admission work, buffered records, and unsynced records. A healthy one-second sync target is not a maximum loss window during filesystem stalls/failures. `written` means the complete write calls returned; only `synced` confirms a subsequent successful file sync. Runtime faults mark unsynced work uncertain rather than silently claiming durability.

## Shutdown responsibilities

Embedded callers must quiesce all broker calls before destruction. The standalone server already shuts down/waits for handlers before destroying its service. Destruction joins maintenance, closes admission, drains healthy storage, counts/discards queued work when the drain budget expires, then joins the writer safely. No writer is detached.

Blocked storage syscalls can exceed the one-second target and delay joining. The existing server's five-second RPC grace is not a hard filesystem shutdown bound. Stronger deadlines require storage isolation and a revised design. Broker delivery state remains process-local and disappears on restart even when feedback logs survive.
