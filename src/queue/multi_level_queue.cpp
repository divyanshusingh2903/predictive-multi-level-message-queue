#include "queue/multi_level_queue.hpp"

#include <stdexcept>

namespace harbinger {

MultiLevelQueue::MultiLevelQueue(uint8_t num_levels,
                                 std::optional<AgingConfig> aging)
    : num_levels_(num_levels),
      queues_(num_levels),
      aging_cfg_(aging) {
    if (num_levels == 0) {
        throw std::invalid_argument("MultiLevelQueue: num_levels must be >= 1");
    }
    if (aging_cfg_) {
        if (aging_cfg_->threshold <= std::chrono::milliseconds::zero() ||
            aging_cfg_->interval <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument(
                "MultiLevelQueue: aging threshold and interval must be positive");
        }
        aging_thread_ = std::thread([this] { run_aging(); });
    }
}

MultiLevelQueue::~MultiLevelQueue() {
    shutdown();
}

void MultiLevelQueue::shutdown() {
    shutdown_.store(true, std::memory_order_release);
    cv_.notify_all();
    if (aging_thread_.joinable()) {
        aging_thread_.join();
    }
}

void MultiLevelQueue::enqueue(Message msg) {
    if (msg.priority >= num_levels_) {
        throw std::out_of_range("MultiLevelQueue::enqueue: priority " +
                                std::to_string(msg.priority) +
                                " >= num_levels " +
                                std::to_string(num_levels_));
    }
    msg.enqueue_time = std::chrono::steady_clock::now();
    {
        std::lock_guard lock{mutex_};
        queues_[msg.priority].push_back(std::move(msg));
        total_size_.fetch_add(1, std::memory_order_relaxed);
    }
    cv_.notify_one();
}

std::optional<Message> MultiLevelQueue::dequeue_locked() {
    for (auto& q : queues_) {
        if (!q.empty()) {
            auto msg = std::move(q.front());
            q.pop_front();
            total_size_.fetch_sub(1, std::memory_order_relaxed);
            return msg;
        }
    }
    return std::nullopt;
}

std::optional<Message> MultiLevelQueue::try_dequeue() {
    std::lock_guard lock{mutex_};
    return dequeue_locked();
}

std::vector<Message> MultiLevelQueue::sweep_expired() {
    std::vector<Message> expired;
    std::lock_guard lock{mutex_};
    for (auto& q : queues_) {
        std::deque<Message> remaining;
        while (!q.empty()) {
            auto& msg = q.front();
            if (msg.is_expired()) {
                expired.push_back(std::move(msg));
                q.pop_front();
                total_size_.fetch_sub(1, std::memory_order_relaxed);
            } else {
                remaining.push_back(std::move(msg));
                q.pop_front();
            }
        }
        q = std::move(remaining);
    }
    return expired;
}

std::optional<Message> MultiLevelQueue::dequeue(
    std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    const bool got_item = cv_.wait_for(lock, timeout, [this] {
        if (shutdown_.load(std::memory_order_acquire)) return true;
        return total_size_.load(std::memory_order_relaxed) > 0;
    });
    if (!got_item || shutdown_.load(std::memory_order_acquire)) {
        // Also drain remaining messages on shutdown if any exist
        if (shutdown_.load(std::memory_order_acquire) &&
            total_size_.load(std::memory_order_relaxed) > 0) {
            return dequeue_locked();
        }
        return std::nullopt;
    }
    return dequeue_locked();
}

std::size_t MultiLevelQueue::size(uint8_t level) const {
    if (level >= num_levels_) {
        throw std::out_of_range("MultiLevelQueue::size: level out of range");
    }
    std::lock_guard lock{mutex_};
    return queues_[level].size();
}

void MultiLevelQueue::run_aging() {
    while (!shutdown_.load(std::memory_order_acquire)) {
        std::unique_lock lock{mutex_};
        cv_.wait_for(lock, aging_cfg_->interval, [this] {
            return shutdown_.load(std::memory_order_acquire);
        });
        if (shutdown_.load(std::memory_order_acquire)) break;
        lock.unlock();

        const auto now = std::chrono::steady_clock::now();
        bool promoted_any = false;

        {
            std::lock_guard lock{mutex_};
            // Scan levels from lowest priority down to level 1.
            // Level 0 is already highest — nothing to promote from there.
            for (uint8_t lvl = num_levels_ - 1; lvl >= 1; --lvl) {
                auto& q = queues_[lvl];
                std::deque<Message> remaining;
                while (!q.empty()) {
                    auto& msg = q.front();
                    const auto age =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - msg.enqueue_time);
                    if (msg.is_expired()) {
                        // Leave expired messages for sweep_expired()/Pull to
                        // DLQ; never promote dead messages toward level 0.
                        remaining.push_back(std::move(msg));
                        q.pop_front();
                    } else if (age >= aging_cfg_->threshold) {
                        // Promote: move to next higher level.
                        msg.priority = static_cast<uint8_t>(lvl - 1);
                        msg.enqueue_time = now; // reset so it doesn't re-promote immediately
                        queues_[lvl - 1].push_back(std::move(msg));
                        q.pop_front();
                        promoted_any = true;
                    } else {
                        remaining.push_back(std::move(msg));
                        q.pop_front();
                    }
                }
                q = std::move(remaining);
            }
        }

        if (promoted_any) {
            cv_.notify_all();
        }
    }
}

} // namespace harbinger
