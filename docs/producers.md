# Producing messages

The C++ `harbinger::Producer` registers with a broker and submits opaque byte payloads. Producers do not choose queue levels: the broker currently assigns every new message `default_priority`. See [broker internals](internals.md) for how dispatch priority is applied and [consumer usage](consumers.md) for processing and acknowledgement.

## Send a message

Start the [standalone broker](../README.md#running), then use `include/producer/producer.hpp`:

```cpp
#include "producer/producer.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

auto producer = harbinger::Producer::connect("127.0.0.1:50051");

std::vector<uint8_t> payload{0x01, 0x02};
std::string id = producer->send(
    payload,
    {{"job_type", "resize"}, {"source", "image-service"}},
    std::chrono::seconds{30});

// A successful send returns the broker-assigned message ID.
```

`Producer::connect(address)` creates an insecure gRPC channel, calls `RegisterProducer`, and returns a client holding a broker-assigned ID such as `producer-0`. Registration happens once per `connect()` call; registration failures throw `std::runtime_error`. **This client does not set a registration deadline**, so a failed connection may wait according to gRPC behavior. The client does not reconnect or re-register automatically.

`send(payload, headers, ttl)` performs a synchronous `Submit` RPC with a **5-second client deadline**. It returns the server-generated message ID only when the RPC reports success. `messages_sent()` counts successful responses from this `Producer` instance; it is not a measure of acknowledged jobs. Failed Submit calls throw `std::runtime_error`. Negative TTL passed through the C++ API throws `std::invalid_argument` before any RPC.

## Payloads and headers

| Input | Meaning |
|---|---|
| `payload` | Opaque `std::vector<uint8_t>`; serialized to protobuf `bytes`, then forwarded to a consumer unchanged. The broker does not parse it. |
| `headers` | `std::unordered_map<std::string, std::string>` copied into the message and delivered to the consumer. No header-based routing is active yet. |
| `ttl` | Optional `std::chrono::milliseconds` duration measured from broker arrival, independent of queue waiting or retry time. |

The proxy sets (or overwrites) the reserved `__producer_id` header with the registered producer ID. Do not use that key for application metadata. Headers and raw payloads are **not** sent to an ML service in the current implementation.

Per-message TTL behavior:

| `send` argument | Protobuf `ttl_ms` | Broker behavior |
|---|---|---|
| Omitted / `std::nullopt` | Unset | Use `HarbingerConfig::default_ttl` |
| `std::chrono::milliseconds{0}` | `0` | Explicitly disable expiry, even if the broker default is positive |
| Positive duration | Positive | Expire that long after `arrival_time` |
| Negative duration | Not sent | C++ throws; a raw negative `SubmitRequest.ttl_ms` is rejected with `INVALID_ARGUMENT` |

`Producer::send` does not expose a priority, retry budget, or destination queue parameter. `HarbingerService::route_message` sets priority and `max_retries` from server configuration. A returned ID identifies the same message across Nack and lease-expiry redeliveries; the delivery-attempt token is separate and never needed by producers.

## What happens after Submit

```text
Producer::send
    → RegisterProducer ID checked by Submit
    → Proxy stamps message ID, arrival_time, and __producer_id
    → route_message resolves default TTL, retries, and static priority
    → MultiLevelQueue::enqueue
    → SubmitResponse.message_id
```

If the producer ID is unknown, `Submit` returns `PERMISSION_DENIED`. The proxy generates the ID from a steady-clock nanosecond timestamp and a process-local atomic counter; it is an opaque ID, not a wall-clock timestamp or a durable sequence number.

**A Submit timeout or lost response is ambiguous:** the broker might have queued the message even though `send()` throws. There is currently no producer-supplied idempotency key, publish confirmation beyond the Submit response, or safe automatic Submit retry. Blind retrying may create another message with another ID. Harbinger has no persistent storage; queued messages and DLQ entries disappear on broker restart. See [delivery guarantees](internals.md#delivery-guarantees-and-limits).
