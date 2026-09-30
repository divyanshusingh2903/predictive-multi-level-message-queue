#include "queue/dead_letter_queue.hpp"
#include <algorithm>

namespace harbinger {

void DeadLetterQueue::push(Message msg, DLQReason reason, std::string details) {
    DLQEntry entry{
        .message = std::move(msg),
        .reason  = reason,
        .dlq_time = std::chrono::steady_clock::now(),
        .details  = std::move(details),
    };
    std::lock_guard lock{mutex_};
    entries_.push_back(std::move(entry));
}

std::optional<DLQEntry> DeadLetterQueue::pop() {
    std::lock_guard lock{mutex_};
    if (entries_.empty()) return std::nullopt;
    auto entry = std::move(entries_.front());
    entries_.pop_front();
    return entry;
}

std::vector<DLQEntry> DeadLetterQueue::snapshot(std::size_t offset,
                                               std::size_t limit) const {
    std::lock_guard lock{mutex_};
    std::vector<DLQEntry> out;
    if (offset >= entries_.size() || limit == 0) return out;
    const auto count = std::min(limit, entries_.size() - offset);
    const auto end = offset + count;
    out.reserve(count);
    for (std::size_t i = offset; i < end; ++i) {
        out.push_back(entries_[i]);
    }
    return out;
}

std::size_t DeadLetterQueue::size() const {
    std::lock_guard lock{mutex_};
    return entries_.size();
}

bool DeadLetterQueue::empty() const {
    std::lock_guard lock{mutex_};
    return entries_.empty();
}

} // namespace harbinger
