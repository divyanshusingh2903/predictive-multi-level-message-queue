#pragma once

#include "../queue/message.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pmlmq {

/// Thin ingress layer between the gRPC Submit handler and the routing logic.
///
/// Responsibilities (deliberately narrow):
///   1. Generate a globally unique message ID.
///   2. Stamp the arrival_time.
///   3. Store the originating producer_id in message headers.
///   4. Forward the enriched Message to the routing sink (PMLMQ::route_message).
///
/// The Proxy has no knowledge of queue tiers, priorities, TTL, or retries.
class Proxy {
public:
    /// Callback invoked once per message after ID and arrival time are set.
    /// Supplied by PMLMQService so the Proxy stays decoupled from queue internals.
    using MessageSink = std::function<void(Message)>;

    explicit Proxy(MessageSink sink);

    /// Accept a raw message from a producer, stamp it, and forward to the sink.
    /// @returns The generated message ID (so the caller can return it to the producer).
    [[nodiscard]] std::string accept(std::vector<uint8_t> payload,
                                     std::unordered_map<std::string, std::string> headers,
                                     const std::string& producer_id);

    /// Generate a globally unique message ID.
    /// Format: "<nanosecond-timestamp>-<monotonic-counter>"
    [[nodiscard]] static std::string generate_id();

    [[nodiscard]] uint64_t messages_accepted() const noexcept {
        return accepted_.load(std::memory_order_relaxed);
    }

private:
    MessageSink            sink_;
    std::atomic<uint64_t>  accepted_{0};
};

} // namespace pmlmq
