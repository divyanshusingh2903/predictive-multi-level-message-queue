#pragma once

#include "message.hpp"

#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace pmlmq {

/// FIFO store for messages that cannot be delivered.
/// Entries arrive on max-retries, TTL expiry, or processing errors.
/// The DLQ never redelivers on its own; operators inspect, drain, or replay it.
class DeadLetterQueue {
public:
    /// Append a failed message with its failure context.
    /// @param msg Message to retain (moved into the DLQ).
    /// @param reason Machine-readable failure class.
    /// @param details Human-readable context (e.g. retry count, last error).
    /// @side_effects Stamps dlq_time to now and appends to the FIFO.
    void push(Message msg, DLQReason reason, std::string details = {});

    /// Remove and return the oldest entry.
    /// @return The front entry, or std::nullopt when the DLQ is empty.
    /// @side_effects Pops the front entry when present.
    [[nodiscard]] std::optional<DLQEntry> pop();

    /// Number of retained entries.
    /// @return Current FIFO depth.
    [[nodiscard]] std::size_t size() const;

    /// Check whether the DLQ holds no entries.
    /// @return True when size is 0.
    [[nodiscard]] bool empty() const;

private:
    mutable std::mutex mutex_;
    std::deque<DLQEntry> entries_;
};

} // namespace pmlmq
