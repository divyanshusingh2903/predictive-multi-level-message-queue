# Consuming messages

The bundled `harbinger::Consumer` is a **competing work consumer**: each running client repeatedly asks the broker for a single message, processes it with a callback, and settles that delivery with Ack or Nack. Consumers do not select or see priority levels. If two consumers are attached, they compete for work; both do not receive a copy of each message.

For queue selection and recovery mechanics, see [broker internals](internals.md). For publishing, see [producer usage](producers.md).

## Register and handle work

```cpp
#include "consumer/consumer.hpp"

#include <chrono>

auto consumer = harbinger::Consumer::connect(
    "127.0.0.1:50051",
    [](const harbinger::ReceivedMessage& msg) -> harbinger::AckResult {
        // Process msg.id, msg.payload, and msg.headers.
        // Make side effects idempotent for this stable message ID.
        return harbinger::AckResult::SUCCESS;
    },
    std::chrono::milliseconds{1000});

consumer->start(); // starts one background polling/handler thread
// ... other application work ...
consumer->stop();  // waits for the worker to finish
```

`connect` calls `RegisterConsumer` with a **5-second deadline**. It throws `std::runtime_error` on registration failure. A null handler or a non-positive `pull_timeout` throws `std::invalid_argument`. The client connects using insecure gRPC credentials; registration IDs such as `consumer-0` identify the client to the broker but are not authentication credentials.

The callback receives a `ReceivedMessage` containing `id`, binary `payload`, and string-to-string `headers` (including `__producer_id`). It does **not** receive priority or lease details. On `SUCCESS`, the client Acks; on `FAILURE`, it Nacks with reason `handler_returned_failure`. If the handler throws, the client catches the exception and Nacks with a `handler_exception: ...` reason. The client measures callback runtime with `steady_clock` and puts `processing_time_ms` on either settlement request. The broker accepts that field but **does not yet store or train on it**.

## Polling, dispatch, and timeouts

Each running `Consumer` has one worker thread that issues one synchronous Pull at a time; it does not prefetch a batch or run callbacks concurrently. The broker chooses the highest-priority queued message, FIFO within a level. Competing consumers receive whichever message is next when their Pull is served; work completion can occur in a different order.

The client sets `PullRequest.timeout_ms` to its `pull_timeout` (default **1000 ms**) and a client RPC deadline of `pull_timeout + 500 ms`. The broker caps waiting to `HarbingerConfig::max_pull_wait` (default **5000 ms**) and checks cancellation in up-to-100 ms dequeue chunks. A successful Pull without a message sets `timed_out=true`; the client polls again. On `UNAVAILABLE` or `DEADLINE_EXCEEDED`, it backs off **200 ms** before retrying Pull; other Pull errors stop the worker and are available from `last_rpc_status()`.

The broker assigns an opaque **attempt token** and a fixed delivery lease (default **30 seconds**) when it installs ownership, before the Pull response is returned. The bundled client echoes the token on Ack/Nack; application callbacks need not manage it. Raw gRPC clients **must** echo the `PulledMessage.attempt_token` along with `consumer_id` and `message_id`. The wire `lease_duration_ms` is informational; it is not a renewable timer starting when the client reads the response.

If the broker observes Pull cancellation after selecting live work but before installing ownership, it restores that message to the front of its current level without consuming a retry or resetting its aging time. This also applies to cancellation requested by `stop()`. Work already expired at the selected-message TTL check still goes to DLQ. Front restoration cannot undo concurrent deliveries or promise historical FIFO across multiple cancelled Pulls.

## Settlement and failures

```text
Pull → handler returns SUCCESS → Ack → remove from in-flight
Pull → handler fails/throws → Nack → retry at original priority, or DLQ
Pull → no settlement before lease deadline → retry at original priority, or DLQ
```

A non-expired Nack or lease timeout consumes **one** failure from the broker's `default_max_retries` budget (default **3**). `max_retries=1` means the first failed delivery goes to DLQ; it does not mean one extra attempt. Retried work retains the same message ID and original arrival time, but receives a **new attempt token**. Queue aging time restarts on re-enqueue. A message whose TTL has passed goes to `TTL_EXPIRED` instead of consuming another retry, including at lease expiry. Ack after TTL still returns OK and records TTL expiry **only if that delivery lease is still valid**. A stale Ack after lease expiry fails.

If an Ack/Nack RPC reports `UNAVAILABLE` or `DEADLINE_EXCEEDED`, the client retries the **same request and attempt token** up to **three total calls** with a fresh 5-second RPC deadline per call and 100/200 ms backoffs. It never reruns the callback just to retry settlement. Accepted same-operation retries return OK from the broker's bounded completion history without repeating effects; conflicting operations fail. A settlement `FAILED_PRECONDITION` counts as a lost delivery attempt: the client stops settling that attempt and continues polling, without recording a confirmed Ack/Nack. Other settlement errors, including exhausted transient retries, stop polling. The broker may later recover an unsettled in-flight message on lease expiry.

| Method | What it counts or reports |
|---|---|
| `messages_processed()` | Callback invocations, including redeliveries; not unique messages |
| `messages_acked()` / `messages_nacked()` | One confirmed Ack/Nack outcome each, not each RPC attempt |
| `rpc_failures()` | Failed Ack/Nack RPC calls, including calls subsequently recovered by retry |
| `leases_lost()` | Delivery attempts rejected with settlement `FAILED_PRECONDITION`, counted once per attempt; includes expired, stale, or conflicting attempts |
| `last_rpc_status()` | Terminal RPC error, or OK since the latest `start()`; a transient error followed by success does not become terminal |
| `is_running()` | Whether polling is currently requested; a running callback can still be finishing after `stop()` clears the flag |

Broker errors on direct Ack/Nack calls distinguish missing or invalid tokens (`INVALID_ARGUMENT`), wrong owner (`PERMISSION_DENIED`), stale/expired or conflicting attempts (`FAILED_PRECONDITION`), and unknown deliveries (`NOT_FOUND`). Once completion history expires or reaches its capacity limit, a duplicate settled attempt may instead return `NOT_FOUND`. Do not interpret `NOT_FOUND` as confirmation of success.

Lost attempts leave `last_rpc_status()` unchanged because they are recoverable, but their failed RPC calls still count toward `rpc_failures()`. Pull errors retain the polling policy above, including terminal `FAILED_PRECONDITION`.

## System boundary: delivery safety and consumer recovery

**Harbinger fences stale deliveries, but does not guarantee that a consumer automatically continues after every late settlement.** Delivery ownership, settlement replay, and consumer availability are separate guarantees:

- **Broker ownership:** a late Ack/Nack cannot settle a different live attempt, even after completion-history eviction. Lease expiry requeues the message or sends it to DLQ according to TTL and the failure budget; it does not cancel the original handler.
- **Receipt uncertainty:** cancellation or response loss after the broker's final cancellation check can still create an in-flight delivery that the client never receives. There is no separate receipt-confirmation handshake; ordinary lease recovery applies, including its retry charge unless TTL wins. A cancelled Pull therefore does not guarantee immediate availability or an unchanged retry budget in every timing window.
- **Bounded history:** the broker retains accepted Ack/Nack and expired-attempt records for up to `completion_retention` (**60 seconds** by default), subject to `completion_cache_max_entries` (**10,000 records**). Retention starts when the broker records settlement or lease reclamation, not when the message is submitted or pulled. Capacity pressure can evict a record before its retention period ends. This is not a guaranteed 60-second replay window.
- **Bundled-client recovery:** settlement `FAILED_PRECONDITION` increments `leases_lost()` and permits continued polling. The client does not claim the handler's outcome was accepted. Other final settlement errors stop the worker and appear in `last_rpc_status()`; there is no automatic restart or re-registration.

For a late request from the original registered consumer with a syntactically valid token:

| Broker state when the late Ack/Nack arrives | Result | Bundled consumer behavior |
|---|---|---|
| Expired-attempt record still retained | `FAILED_PRECONDITION` | Count one lost attempt and continue polling |
| Accepted matching Ack/Nack record still retained | OK | Count one confirmed outcome for the local delivery; broker mutation is not repeated |
| History gone; no current in-flight delivery (message queued, completed, or in DLQ) | `NOT_FOUND` | Stop; outcome is not confirmed |
| History gone; a different consumer owns the current delivery | `PERMISSION_DENIED` | Stop; outcome is not confirmed |
| History gone; the same consumer owns a newer attempt | `FAILED_PRECONDITION` | Count one lost attempt and continue polling |

After history is gone, the broker cannot reliably distinguish a forgotten authentic attempt from an unknown delivery or a wrong-owner request using the current protocol. The client therefore keeps `NOT_FOUND` and `PERMISSION_DENIED` terminal rather than treating them as success or ignoring all ownership errors. `leases_lost()` counts observed settlement `FAILED_PRECONDITION` results, including stale/conflicting attempts; it is not a count of every broker lease expiry.

### Application and operator responsibilities

1. **Size the fixed lease for handler duration plus delivery/network and settlement overhead.** There is no lease renewal or handler preemption. An overlong handler may overlap a redelivery and consume the retry budget even if its application work eventually succeeds.
2. **Make external effects duplicate-safe.** Use an application-appropriate durable idempotency key or transaction. Attempt-token fencing protects broker state, not database writes, payments, or other handler effects; handler success alone is not confirmed settlement.
3. **Size completion history for expected settlement delays and peak completion volume.** Increasing retention without enough cache capacity does not preserve replay history. Larger settings reduce eviction risk but do not provide durable or indefinite history. Set `HarbingerConfig` fields for an embedded broker, or use the [standalone server recovery options](../README.md#standalone-server-recovery-options), such as `--completion-retention-ms` and `--completion-cache-max-entries`. Settings apply at startup.
4. **Monitor both availability and delivery outcomes.** Observe `is_running()`, `last_rpc_status()`, `rpc_failures()`, and `leases_lost()`. On a terminal error, investigate the status and decide whether to restart the worker; do not blindly reinterpret permission/argument errors as expired leases. `start()` resets terminal status, retains counters, and does not rerun the previous handler merely to settle it.
5. **Treat broker restart as loss of process-local state.** Queued messages, DLQ entries, leases, completion records, and registrations are not persisted. Reconnect/register clients after restart; reconnecting does not recover lost messages.

These boundaries apply to the current Phase 1 implementation. Persistent delivery, exactly-once external effects, lease renewal, and automatic consumer-session recovery are not provided. See [broker guarantees and limits](internals.md#delivery-guarantees-and-limits) for the wider system boundary.

## Lifecycle and duplicate work

`stop()` cancels an outstanding Pull and joins the worker from an external thread. A handler already running will finish its callback and bounded settlement attempts first; arbitrary callbacks cannot be interrupted. Calling `stop()` **from the handler** only requests shutdown and avoids self-joining. Never destroy the consumer from inside its own callback. Destruction from another thread calls `stop()` and joins. An exited worker can be restarted with `start()`; calling `start()` while running or stopping is an error. Counters remain on the same client object across restarts.

**Processing is at-least-once while the broker runs, not exactly-once.** If a callback takes longer than its lease, the broker can redeliver to another consumer while the first callback is still executing. Choose `delivery_lease` longer than handler duration plus network and settlement overhead, and make handler side effects safe to repeat using `msg.id`. Attempt tokens prevent an old handler from settling a new delivery; they do not undo side effects. Neither messages nor completion records survive broker restart.
