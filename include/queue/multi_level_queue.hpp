#pragma once

#include "message.hpp"
#include <atomic>
#include <condition_variable>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace harbinger {

/// Thread-safe FIFO levels, strict priority, and optional aging.
class MultiLevelQueue {
public:
    explicit MultiLevelQueue(uint8_t num_levels = 3,
                             std::optional<AgingConfig> aging = std::nullopt);
    ~MultiLevelQueue();
    MultiLevelQueue(const MultiLevelQueue&) = delete;
    MultiLevelQueue& operator=(const MultiLevelQueue&) = delete;
    /// Append at priority and reset enqueue_time.
    void enqueue(Message msg);
    /// Remove the highest-priority available message.
    [[nodiscard]] std::optional<Message> try_dequeue();
    /// Wait for a message or shutdown; queued messages remain drainable after shutdown.
    [[nodiscard]] std::optional<Message> dequeue(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{100});
    /// Remove all expired messages in level/FIFO order.
    [[nodiscard]] std::vector<Message> sweep_expired();
    /// Remove at most limit expired messages in deadline order without scanning live messages.
    [[nodiscard]] std::vector<Message> sweep_expired_batch(std::size_t limit);
    /// Wake waiters and join aging; concurrent calls are serialized.
    void shutdown();
    [[nodiscard]] std::size_t size() const noexcept { return total_size_.load(); }
    [[nodiscard]] std::size_t size(uint8_t level) const;
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] uint8_t num_levels() const noexcept { return num_levels_; }

private:
    friend struct QueueTestAccess;
    using Clock = std::chrono::steady_clock;
    struct Node;
    using Level = std::list<Node>;
    using ExpiryIndex = std::multimap<Clock::time_point, Level::iterator>;
    struct Node {
        Message message;
        std::optional<ExpiryIndex::iterator> expiry;
    };
    void run_aging();
    std::optional<Message> dequeue_locked();
    Message remove_locked(uint8_t level, Level::iterator it);

    uint8_t num_levels_;
    std::vector<Level> queues_;
    ExpiryIndex expiry_;
    mutable std::mutex mutex_;
    std::mutex shutdown_mutex_;
    std::condition_variable available_cv_;
    std::condition_variable aging_cv_;
    std::atomic<std::size_t> total_size_{0};
    bool shutdown_{false};
    std::optional<AgingConfig> aging_cfg_;
    std::thread aging_thread_;
};

} // namespace harbinger
