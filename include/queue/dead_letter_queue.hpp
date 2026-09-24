#pragma once

#include "message.hpp"

#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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

    /// Copy a page of entries without removing them.
    /// @param offset Entries to skip from the front (FIFO order).
    /// @param limit Maximum entries to return.
    /// @return Up to limit entries starting at offset; empty past the end.
    [[nodiscard]] std::vector<DLQEntry> snapshot(std::size_t offset,
                                                std::size_t limit) const;

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
