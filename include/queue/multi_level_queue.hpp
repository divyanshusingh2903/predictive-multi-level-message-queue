#pragma once

#include "message.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace pmlmq {

/// Thread-safe multi-level priority queue.
///
/// Messages are routed into one of `num_levels` sub-queues, where index 0
/// holds the highest-priority messages. `try_dequeue()` and `dequeue()`
/// always drain index 0 before 1, etc. (strict priority).
///
/// Optional aging: a background thread periodically promotes messages that
/// have been waiting longer than `AgingConfig::threshold`, moving them one
/// level toward index 0 to prevent starvation of lower-priority queues.
class MultiLevelQueue {
public:
    /// @param num_levels   Number of priority levels (must be >= 1).
    /// @param aging        Optional aging configuration. Pass std::nullopt to
    ///                     disable aging (pure strict-priority mode).
    explicit MultiLevelQueue(uint8_t num_levels = 3,
                             std::optional<AgingConfig> aging = std::nullopt);

    ~MultiLevelQueue();

    // Non-copyable, non-movable (owns a background thread).
    MultiLevelQueue(const MultiLevelQueue&) = delete;
    MultiLevelQueue& operator=(const MultiLevelQueue&) = delete;

    /// Enqueue a message. Sets `msg.enqueue_time` to now.
    /// @throws std::out_of_range if msg.priority >= num_levels.
    void enqueue(Message msg);

    /// Non-blocking dequeue. Returns the highest-priority available message,
    /// or std::nullopt if the queue is empty.
    [[nodiscard]] std::optional<Message> try_dequeue();

    /// Blocking dequeue with timeout.
    /// @param timeout  Maximum time to wait for a message.
    /// @return         The next message, or std::nullopt on timeout/shutdown.
    [[nodiscard]] std::optional<Message> dequeue(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{100});

    /// Signal all blocked `dequeue()` calls to return std::nullopt and stop
    /// the aging thread. Safe to call from any thread.
    void shutdown();

    /// Total number of messages across all levels.
    [[nodiscard]] std::size_t size() const noexcept {
        return total_size_.load(std::memory_order_relaxed);
    }

    /// Number of messages in a specific priority level.
    /// @throws std::out_of_range if level >= num_levels.
    [[nodiscard]] std::size_t size(uint8_t level) const;

    [[nodiscard]] bool empty() const noexcept {
        return total_size_.load(std::memory_order_relaxed) == 0;
    }

    [[nodiscard]] uint8_t num_levels() const noexcept { return num_levels_; }

private:
    void run_aging();

    /// Returns the highest-priority non-empty message while the mutex is held.
    /// Caller must hold mutex_.
    std::optional<Message> dequeue_locked();

    uint8_t num_levels_;
    std::vector<std::deque<Message>> queues_; // queues_[0] = highest priority
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<std::size_t> total_size_{0};
    std::atomic<bool> shutdown_{false};

    std::optional<AgingConfig> aging_cfg_;
    std::thread aging_thread_;
};

} // namespace pmlmq
