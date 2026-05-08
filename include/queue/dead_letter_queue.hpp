#pragma once

#include "message.hpp"

#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace pmlmq {

/// Dead Letter Queue — holds messages that could not be successfully processed.
///
/// Messages land here when:
///   - retry_count >= max_retries (MAX_RETRIES_EXCEEDED)
///   - TTL has expired            (TTL_EXPIRED)
///   - Handler signals PROCESSING_ERROR
///
/// The DLQ is a simple FIFO and does not re-deliver messages automatically.
/// It can be inspected, drained, or replayed by an operator.
class DeadLetterQueue {
public:
    /// Add a message to the DLQ.
    void push(Message msg, DLQReason reason, std::string details = {});

    /// Remove and return the oldest DLQ entry, or std::nullopt if empty.
    [[nodiscard]] std::optional<DLQEntry> pop();

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool empty() const;

private:
    mutable std::mutex mutex_;
    std::deque<DLQEntry> entries_;
};

} // namespace pmlmq
