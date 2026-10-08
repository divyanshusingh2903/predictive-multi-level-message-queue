#pragma once

#include "message.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace harbinger {

enum class QueueSelection { StrictPriority, RoundRobin, WeightedTime };

/// Worker-time share across levels (#40). Each level has a virtual clock; a pull serves the non-empty level with the
/// smallest clock and advances it by cost / weight, so while every level has messages level i receives about
/// weights[i] / sum(weights) of worker time measured in expected cost. A level that becomes non-empty starts no
/// earlier than the floor of the active clocks, so an empty level cannot save up credit.
struct LevelShare {
    std::vector<uint32_t> weights;
    /// Expected cost in milliseconds of serving a message. Called under the queue lock: must be cheap, must not block
    /// or take locks that are held while calling into the queue. Results below kMinCostMs are raised to it.
    std::function<double(const Message&)> cost;
    static constexpr double kMinCostMs = 1.0;
};

/// Pulls and expected cost charged per level since construction (weighted mode only).
struct LevelShareStats {
    std::vector<uint64_t> pulls;
    std::vector<double> charged_ms;
};

/// Thread-safe FIFO levels with strict-priority, cyclic or worker-time-weighted selection and optional aging.
class MultiLevelQueue {
public:
    explicit MultiLevelQueue(uint8_t num_levels = 3,
                             std::optional<AgingConfig> aging = std::nullopt,
                             QueueSelection selection = QueueSelection::StrictPriority,
                             std::optional<LevelShare> share = std::nullopt);
    ~MultiLevelQueue();
    MultiLevelQueue(const MultiLevelQueue&) = delete;
    MultiLevelQueue& operator=(const MultiLevelQueue&) = delete;
    /// Append at priority and reset enqueue_time.
    void enqueue(Message msg);
    /// Restore an uncommitted dequeue at its level's front, preserving all message metadata.
    void requeue_front(Message msg);
    /// Remove a FIFO head according to the configured selection policy.
    [[nodiscard]] std::optional<Message> try_dequeue();
    /// Wait for a message or shutdown; queued messages remain drainable after shutdown.
    [[nodiscard]] std::optional<Message> dequeue(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{100});
    /// Diagnostic full scan in level/FIFO order; use bounded batches on service hot paths.
    [[nodiscard]] std::vector<Message> sweep_expired();
    /// Remove at most limit expired messages in deadline order without scanning live messages.
    [[nodiscard]] std::vector<Message> sweep_expired_batch(std::size_t limit);
    /// Wake waiters and join aging; concurrent calls are serialized.
    void shutdown();
    [[nodiscard]] std::size_t size() const noexcept { return total_size_.load(); }
    [[nodiscard]] std::size_t size(uint8_t level) const;
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] uint8_t num_levels() const noexcept { return num_levels_; }
    /// Per-level pulls and charged cost; empty vectors unless selection is WeightedTime.
    [[nodiscard]] LevelShareStats share_stats() const;

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
    std::optional<Clock::time_point> aging_deadline(Clock::time_point enqueued) const;
    void note_aging_deadline(uint8_t level, Clock::time_point enqueued);
    std::size_t age_once_locked(Clock::time_point now);
    void place_message(Message msg, bool restore_front);
    std::optional<Message> dequeue_locked();
    Message remove_locked(uint8_t level, Level::iterator it);
    [[nodiscard]] double charge_locked(const Message& msg) const;
    void activate_locked(uint8_t level);
    [[nodiscard]] static bool is_promoted(const Message& msg) noexcept;
    [[nodiscard]] bool paused_locked(uint8_t level, Clock::time_point now) const;

    uint8_t num_levels_;
    QueueSelection selection_;
    std::size_t cursor_{0};
    std::vector<Level> queues_;
    ExpiryIndex expiry_;
    mutable std::mutex mutex_;
    std::mutex shutdown_mutex_;
    std::condition_variable available_cv_;
    std::condition_variable aging_cv_;
    std::atomic<std::size_t> total_size_{0};
    bool shutdown_{false};
    std::optional<AgingConfig> aging_cfg_;
    // Conservative minima: removal can leave an earlier deadline until the next due scan.
    std::vector<std::optional<Clock::time_point>> next_aging_;
    // Messages per level whose priority is below their original one (promoted into it by aging).
    std::vector<std::size_t> promoted_in_;
    std::optional<LevelShare> share_;
    std::vector<double> vclock_;
    double vfloor_{0.0};
    std::vector<uint64_t> pulls_;
    std::vector<double> charged_;
    std::thread aging_thread_;
};

} // namespace harbinger
