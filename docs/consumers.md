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

## Settlement and failures

```text
Pull → handler returns SUCCESS → Ack → remove from in-flight
Pull → handler fails/throws → Nack → retry at original priority, or DLQ
Pull → no settlement before lease deadline → retry at original priority, or DLQ
```

A non-expired Nack or lease timeout consumes **one** failure from the broker's `default_max_retries` budget (default **3**). `max_retries=1` means the first failed delivery goes to DLQ; it does not mean one extra attempt. Retried work retains the same message ID and original arrival time, but receives a **new attempt token**. Queue aging time restarts on re-enqueue. A message whose TTL has passed goes to `TTL_EXPIRED` instead of consuming another retry, including at lease expiry. Ack after TTL still returns OK and records TTL expiry **only if that delivery lease is still valid**. A stale Ack after lease expiry fails.

If an Ack/Nack RPC reports `UNAVAILABLE` or `DEADLINE_EXCEEDED`, the client retries the **same request and attempt token** up to **three total calls** with a fresh 5-second RPC deadline per call and 100/200 ms backoffs. It never reruns the callback just to retry settlement. Accepted same-operation retries return OK from the broker's bounded completion history without repeating effects; conflicting operations fail. If settlement still fails, or a permanent error occurs, the client stops polling. The broker may later recover the in-flight message on lease expiry.

| Method | What it counts or reports |
|---|---|
| `messages_processed()` | Callback invocations, including redeliveries; not unique messages |
| `messages_acked()` / `messages_nacked()` | One confirmed Ack/Nack outcome each, not each RPC attempt |
| `rpc_failures()` | Failed Ack/Nack RPC calls, including calls subsequently recovered by retry |
| `last_rpc_status()` | Terminal RPC error, or OK since the latest `start()`; a transient error followed by success does not become terminal |
| `is_running()` | Whether polling is currently requested; a running callback can still be finishing after `stop()` clears the flag |

Broker errors on direct Ack/Nack calls distinguish missing or invalid tokens (`INVALID_ARGUMENT`), wrong owner (`PERMISSION_DENIED`), stale/expired or conflicting attempts (`FAILED_PRECONDITION`), and unknown deliveries (`NOT_FOUND`). Once completion history expires or reaches its capacity limit, a duplicate settled attempt may instead return `NOT_FOUND`. Do not interpret `NOT_FOUND` as confirmation of success.

## Lifecycle and duplicate work

`stop()` cancels an outstanding Pull and joins the worker from an external thread. A handler already running will finish its callback and bounded settlement attempts first; arbitrary callbacks cannot be interrupted. Calling `stop()` **from the handler** only requests shutdown and avoids self-joining. Never destroy the consumer from inside its own callback. Destruction from another thread calls `stop()` and joins. An exited worker can be restarted with `start()`; calling `start()` while running or stopping is an error. Counters remain on the same client object across restarts.

**Processing is at-least-once while the broker runs, not exactly-once.** If a callback takes longer than its lease, the broker can redeliver to another consumer while the first callback is still executing. Choose `delivery_lease` longer than handler duration plus network and settlement overhead, and make handler side effects safe to repeat using `msg.id`. Attempt tokens prevent an old handler from settling a new delivery; they do not undo side effects. Neither messages nor completion records survive broker restart.
