#include "queue/multi_level_queue.hpp"
#include <stdexcept>

namespace harbinger {

MultiLevelQueue::MultiLevelQueue(uint8_t num_levels, std::optional<AgingConfig> aging)
    : num_levels_(num_levels), queues_(num_levels), aging_cfg_(aging), next_aging_(num_levels) {
    if (!num_levels) throw std::invalid_argument("num_levels must be positive");
    if (aging_cfg_) {
        if (aging_cfg_->threshold.count() <= 0 || aging_cfg_->interval.count() <= 0)
            throw std::invalid_argument("aging durations must be positive");
        aging_thread_ = std::thread([this] { run_aging(); });
    }
}

MultiLevelQueue::~MultiLevelQueue() { shutdown(); }

void MultiLevelQueue::shutdown() {
    std::lock_guard join_lock{shutdown_mutex_};
    {
        std::lock_guard lock{mutex_};
        shutdown_ = true;
    }
    available_cv_.notify_all();
    aging_cv_.notify_all();
    if (aging_thread_.joinable()) aging_thread_.join();
}

void MultiLevelQueue::enqueue(Message msg) {
    place_message(std::move(msg), false);
}

void MultiLevelQueue::requeue_front(Message msg) {
    place_message(std::move(msg), true);
}

void MultiLevelQueue::place_message(Message msg, bool restore_front) {
    if (msg.priority >= num_levels_) throw std::out_of_range("priority out of range");
    if (!restore_front) msg.enqueue_time = Clock::now();
    {
        std::lock_guard lock{mutex_};
        auto& level = queues_[msg.priority];
        auto it = level.emplace(restore_front ? level.begin() : level.end(),
                                Node{std::move(msg), std::nullopt});
        try {
            if (it->message.ttl.count() > 0)
                it->expiry = expiry_.emplace(it->message.expiry_time(), it);
        } catch (...) {
            level.erase(it);
            throw;
        }
        ++total_size_;
        note_aging_deadline(it->message.priority, it->message.enqueue_time);
    }
    available_cv_.notify_one();
}

Message MultiLevelQueue::remove_locked(uint8_t level, Level::iterator it) {
    if (it->expiry) expiry_.erase(*it->expiry);
    Message msg = std::move(it->message);
    queues_[level].erase(it);
    if (queues_[level].empty()) next_aging_[level].reset();
    --total_size_;
    return msg;
}

std::optional<Message> MultiLevelQueue::dequeue_locked() {
    for (uint8_t level = 0; level < num_levels_; ++level)
        if (!queues_[level].empty()) return remove_locked(level, queues_[level].begin());
    return std::nullopt;
}

std::optional<Message> MultiLevelQueue::try_dequeue() {
    std::lock_guard lock{mutex_};
    return dequeue_locked();
}

std::optional<Message> MultiLevelQueue::dequeue(std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    available_cv_.wait_for(lock, timeout, [this] { return shutdown_ || total_size_ > 0; });
    return dequeue_locked();
}

std::vector<Message> MultiLevelQueue::sweep_expired_batch(std::size_t limit) {
    std::vector<Message> expired;
    std::lock_guard lock{mutex_};
    const auto now = Clock::now();
    while (expired.size() < limit && !expiry_.empty() && expiry_.begin()->first <= now) {
        const auto it = expiry_.begin()->second;
        expired.push_back(remove_locked(it->message.priority, it));
    }
    return expired;
}

std::vector<Message> MultiLevelQueue::sweep_expired() {
    std::vector<Message> expired;
    std::lock_guard lock{mutex_};
    if (expiry_.empty() || expiry_.begin()->first > Clock::now()) return expired;
    for (uint8_t level = 0; level < num_levels_; ++level) {
        auto& q = queues_[level];
        for (auto it = q.begin(); it != q.end();) {
            auto current = it++;
            if (current->message.is_expired())
                expired.push_back(remove_locked(level, current));
        }
    }
    return expired;
}

std::size_t MultiLevelQueue::size(uint8_t level) const {
    if (level >= num_levels_) throw std::out_of_range("level out of range");
    std::lock_guard lock{mutex_};
    return queues_[level].size();
}

std::optional<MultiLevelQueue::Clock::time_point>
MultiLevelQueue::aging_deadline(Clock::time_point enqueued) const {
    const auto max_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max());
    if (aging_cfg_->threshold >= max_ms) return std::nullopt;
    const auto delay = std::chrono::duration_cast<Clock::duration>(aging_cfg_->threshold);
    if (enqueued > Clock::time_point::max() - delay) return std::nullopt;
    return enqueued + delay;
}

void MultiLevelQueue::note_aging_deadline(uint8_t level, Clock::time_point enqueued) {
    if (!aging_cfg_ || level == 0) return;
    const auto due = aging_deadline(enqueued);
    if (due && (!next_aging_[level] || *due < *next_aging_[level])) next_aging_[level] = due;
}

std::size_t MultiLevelQueue::age_once_locked(Clock::time_point now) {
    if (!aging_cfg_) return 0;
    std::size_t visited = 0;
    bool promoted = false;
    for (uint8_t level = num_levels_ - 1; level >= 1; --level) {
        if (!next_aging_[level] || *next_aging_[level] > now) continue;
        auto& q = queues_[level];
        next_aging_[level].reset();
        for (auto it = q.begin(); it != q.end();) {
            auto current = it++;
            auto& msg = current->message;
            ++visited;
            if (msg.is_expired()) continue;
            const auto due = aging_deadline(msg.enqueue_time);
            if (due && *due <= now) {
                msg.priority = level - 1;
                msg.enqueue_time = now;
                queues_[level - 1].splice(queues_[level - 1].end(), q, current);
                note_aging_deadline(msg.priority, msg.enqueue_time);
                promoted = true;
            } else {
                note_aging_deadline(level, msg.enqueue_time);
            }
        }
    }
    if (promoted) available_cv_.notify_all();
    return visited;
}

void MultiLevelQueue::run_aging() {
    std::unique_lock lock{mutex_};
    while (!aging_cv_.wait_for(lock, aging_cfg_->interval, [this] { return shutdown_; })) {
        (void)age_once_locked(Clock::now());
    }
}

} // namespace harbinger
