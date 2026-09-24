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

/// Thread-safe strict-priority queue with optional aging.
/// Level 0 holds the highest-priority messages; dequeue always drains
/// level 0 before 1, and so on. Aging promotes stale messages one level
/// per scan so low-priority queues are not starved.
class MultiLevelQueue {
public:
    /// Build a queue with the given level count and aging policy.
    /// @param num_levels Number of priority levels; must be >= 1.
    /// @param aging Optional aging config; nullopt disables the aging thread.
    /// @return New queue with empty levels.
    /// @side_effects Spawns a background aging thread if aging is set.
    /// @throws std::invalid_argument if num_levels == 0.
    explicit MultiLevelQueue(uint8_t num_levels = 3,
                             std::optional<AgingConfig> aging = std::nullopt);

    ~MultiLevelQueue();

    // Non-copyable, non-movable (owns background thread).
    MultiLevelQueue(const MultiLevelQueue&) = delete;
    MultiLevelQueue& operator=(const MultiLevelQueue&) = delete;

    /// Place a message into its priority level.
    /// @param msg Message to store; priority must be < num_levels.
    /// @side_effects Resets msg.enqueue_time to now, appends to the level,
    ///   increments total size, notifies one blocked dequeue waiter.
    /// @throws std::out_of_range if msg.priority >= num_levels.
    void enqueue(Message msg);

    /// Take the highest-priority message without blocking.
    /// @return The message, or std::nullopt when all levels are empty.
    /// @side_effects Removes the message and decrements total size.
    [[nodiscard]] std::optional<Message> try_dequeue();

    /// Take the highest-priority message, waiting up to timeout.
    /// @param timeout Maximum time to block for a message.
    /// @return The message, or std::nullopt on timeout or after shutdown.
    /// @side_effects Blocks on a condition variable; removes and returns
    ///   a message when one arrives, decrements total size.
    [[nodiscard]] std::optional<Message> dequeue(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{100});

    /// Remove all TTL-expired messages from every level.
    /// @return Expired messages in highest-to-lowest level order; the queue
    ///   never touches the DLQ — the caller owns disposal.
    /// @side_effects Locks once, splices expired out of each level,
    ///   decrements total size per removal. Reads only is_expired()
    ///   (arrival_time clock), never the aging (enqueue_time) clock.
    [[nodiscard]] std::vector<Message> sweep_expired();

    /// Stop the queue permanently.
    /// @side_effects Sets the shutdown flag, wakes all blocked dequeue
    ///   callers (they return nullopt), and joins the aging thread.
    ///   Safe to call multiple times from any thread.
    void shutdown();

    /// Total messages across all levels.
    /// @return Sum of all per-level sizes (lock-free estimate).
    [[nodiscard]] std::size_t size() const noexcept {
        return total_size_.load(std::memory_order_relaxed);
    }

    /// Messages held in one priority level.
    /// @param level Level index in [0, num_levels).
    /// @return Number of queued messages at that level.
    /// @throws std::out_of_range if level >= num_levels.
    [[nodiscard]] std::size_t size(uint8_t level) const;

    /// Check whether all levels are empty.
    /// @return True when total size is 0.
    [[nodiscard]] bool empty() const noexcept {
        return total_size_.load(std::memory_order_relaxed) == 0;
    }

    /// Number of priority levels this queue was built with.
    /// @return num_levels passed to the constructor.
    [[nodiscard]] uint8_t num_levels() const noexcept { return num_levels_; }

private:
    void run_aging();

    /// Highest-priority message; caller must hold mutex_.
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
