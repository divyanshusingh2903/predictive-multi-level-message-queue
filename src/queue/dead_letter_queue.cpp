#include "queue/dead_letter_queue.hpp"

namespace pmlmq {

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

std::size_t DeadLetterQueue::size() const {
    std::lock_guard lock{mutex_};
    return entries_.size();
}

bool DeadLetterQueue::empty() const {
    std::lock_guard lock{mutex_};
    return entries_.empty();
}

} // namespace pmlmq
