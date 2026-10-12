// RabbitMQ and Redis work queues for the production-flow benchmark (#37, #47). Producers and workers stay in
// benchmarks/prodflow.cpp; this file only moves messages. Built when HARBINGER_BUILD_BROKER_ADAPTERS is ON.
//
// Both brokers run as #37 freezes them: non-persistent, defaults otherwise.
// - RabbitMQ: one classic queue, transient messages, no publisher confirms; workers use manual ack with prefetch 1.
//   With priorities the queue has x-max-priority 3, and tier t (0 = most urgent) is sent as priority 2 - t.
//   A failed attempt is nacked with requeue, which RabbitMQ puts back near the head of the queue.
// - Redis: the reliable list-queue pattern of common job libraries. LPUSH to the queue; each worker BLMOVEs the oldest
//   message into its own processing list and LREMs it on success. A failed attempt is LMOVEd back to the queue's tail.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace harbinger::bench {

struct Endpoint {
    std::string host;
    int port{0};
};

/// Parse HOST:PORT.
Endpoint parse_endpoint(const std::string& text);

/// Runs one delivery; true acknowledges it, false has it retried.
using Handle = std::function<bool(uint64_t seq)>;

class Publisher {
public:
    virtual ~Publisher() = default;
    /// tier is honoured only by a priority queue; nullopt sends no priority.
    virtual void publish(uint64_t seq, const std::string& payload, std::optional<uint8_t> tier) = 0;
};

class Workers {
public:
    virtual ~Workers() = default;
    /// Stop pulling, finish the delivery in hand, close connections. Idempotent.
    virtual void stop() = 0;
    /// First error that stopped a worker, or empty.
    [[nodiscard]] virtual std::string error() const = 0;
};

enum class BrokerKind { RabbitMq, Redis };

/// Empty the benchmark queue (deleting and re-declaring it for RabbitMQ) and return the server's version string.
std::string prepare_queue(BrokerKind kind, const Endpoint& endpoint, bool priorities);
/// One connection per publisher; not thread-safe, so one per producer thread.
std::unique_ptr<Publisher> make_publisher(BrokerKind kind, const Endpoint& endpoint);
/// `count` worker threads, each with its own connection, calling handle for every delivery.
std::unique_ptr<Workers> start_workers(BrokerKind kind, const Endpoint& endpoint, int count, Handle handle);
/// Client library and version compiled in, for the run manifest.
std::string client_library(BrokerKind kind);

} // namespace harbinger::bench
