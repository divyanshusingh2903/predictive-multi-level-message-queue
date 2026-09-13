#pragma once

#include "../queue/message.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pmlmq {

/// Ingress layer between Submit handling and routing.
/// Generates the message ID, stamps arrival_time, records the producer ID
/// in headers, then forwards the message to the routing sink.
/// Has no knowledge of queue tiers, priorities, TTL, or retries.
class Proxy {
public:
    /// Callback invoked once per accepted message. Supplied by the broker
    /// so the proxy stays decoupled from queue internals.
    using MessageSink = std::function<void(Message)>;

    /// Build a proxy that forwards accepted messages to sink.
    /// @param sink Routing callback invoked with each stamped message.
    /// @throws std::invalid_argument if sink is null.
    explicit Proxy(MessageSink sink);

    /// Stamp and forward a raw producer payload.
    /// @param payload Opaque message bytes (moved into the Message).
    /// @param headers Key-value metadata (moved; producer ID is added under "__producer_id").
    /// @param producer_id Originating producer, stored in headers.
    /// @return The generated unique message ID.
    /// @side_effects Sets arrival_time to now, increments the accepted
    ///   counter, and invokes the sink with the finished Message.
    [[nodiscard]] std::string accept(std::vector<uint8_t> payload,
                                     std::unordered_map<std::string, std::string> headers,
                                     const std::string& producer_id);

    /// Generate a unique message ID.
    /// @return String of the form "<ns-timestamp>-<monotonic-counter>".
    /// @side_effects Atomically increments a process-wide counter.
    [[nodiscard]] static std::string generate_id();

    /// Total messages accepted so far.
    /// @return Value of the accepted counter.
    [[nodiscard]] uint64_t messages_accepted() const noexcept {
        return accepted_.load(std::memory_order_relaxed);
    }

private:
    MessageSink            sink_;
    std::atomic<uint64_t>  accepted_{0};
};

} // namespace pmlmq
