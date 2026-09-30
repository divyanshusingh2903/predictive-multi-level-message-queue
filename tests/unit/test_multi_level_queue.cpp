#include "queue/multi_level_queue.hpp"

#include <gtest/gtest.h>

#include <future>
#include <thread>
#include <vector>

using namespace harbinger;
using namespace std::chrono_literals;

namespace harbinger {
struct QueueTestAccess {
    static std::size_t expiry_size(MultiLevelQueue& q) {
        std::lock_guard lock{q.mutex_};
        return q.expiry_.size();
    }
};
}

namespace {

Message make_msg(uint8_t priority, std::string id = {}) {
    Message m;
    m.id = id.empty() ? "msg-" + std::to_string(priority) : id;
    m.priority = priority;
    m.original_priority = priority;
    m.payload = {static_cast<uint8_t>(priority)};
    return m;
}

} // namespace

// ── Construction ─────────────────────────────────────────────────────────────

TEST(MultiLevelQueue, DefaultConstruction) {
    MultiLevelQueue q;
    EXPECT_EQ(q.num_levels(), 3);
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);
}

TEST(MultiLevelQueue, CustomNumLevels) {
    MultiLevelQueue q{5};
    EXPECT_EQ(q.num_levels(), 5);
}

TEST(MultiLevelQueue, ZeroLevelsThrows) {
    EXPECT_THROW(MultiLevelQueue{0}, std::invalid_argument);
}

// ── Enqueue / Dequeue ─────────────────────────────────────────────────────────

TEST(MultiLevelQueue, EnqueueIncreasesSize) {
    MultiLevelQueue q{3};
    q.enqueue(make_msg(0));
    EXPECT_EQ(q.size(), 1u);
    q.enqueue(make_msg(2));
    EXPECT_EQ(q.size(), 2u);
}

TEST(MultiLevelQueue, InvalidPriorityThrows) {
    MultiLevelQueue q{3};
    auto msg = make_msg(0);
    msg.priority = 5; // out of range
    EXPECT_THROW(q.enqueue(std::move(msg)), std::out_of_range);
}

TEST(MultiLevelQueue, TryDequeueEmptyReturnsNullopt) {
    MultiLevelQueue q{3};
    EXPECT_FALSE(q.try_dequeue().has_value());
}

TEST(MultiLevelQueue, StrictPriorityOrdering) {
    MultiLevelQueue q{3};
    // Enqueue in reverse order so lower priority arrives first.
    q.enqueue(make_msg(2, "low"));
    q.enqueue(make_msg(1, "med"));
    q.enqueue(make_msg(0, "high"));

    auto first  = q.try_dequeue();
    auto second = q.try_dequeue();
    auto third  = q.try_dequeue();

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(third.has_value());

    EXPECT_EQ(first->id,  "high");
    EXPECT_EQ(second->id, "med");
    EXPECT_EQ(third->id,  "low");
}

TEST(MultiLevelQueue, SizePerlevel) {
    MultiLevelQueue q{3};
    q.enqueue(make_msg(0));
    q.enqueue(make_msg(0));
    q.enqueue(make_msg(2));

    EXPECT_EQ(q.size(0), 2u);
    EXPECT_EQ(q.size(1), 0u);
    EXPECT_EQ(q.size(2), 1u);
}

TEST(MultiLevelQueue, SizePerLevelOutOfRangeThrows) {
    MultiLevelQueue q{3};
    EXPECT_THROW(q.size(3), std::out_of_range);
}

TEST(MultiLevelQueue, EnqueueTimestampIsSet) {
    MultiLevelQueue q{3};
    const auto before = std::chrono::steady_clock::now();
    q.enqueue(make_msg(0));
    const auto after = std::chrono::steady_clock::now();

    auto msg = q.try_dequeue();
    ASSERT_TRUE(msg.has_value());
    EXPECT_GE(msg->enqueue_time, before);
    EXPECT_LE(msg->enqueue_time, after);
}

TEST(MultiLevelQueue, RestoreFrontPreservesMetadataAndLevelOrder) {
    MultiLevelQueue q;
    q.enqueue(make_msg(1, "A"));
    q.enqueue(make_msg(1, "B"));
    auto selected = q.try_dequeue();
    ASSERT_TRUE(selected);
    selected->original_priority = 2;
    selected->retry_count = 1;
    selected->max_retries = 5;
    selected->enqueue_time = std::chrono::steady_clock::now() - 10s;
    selected->arrival_time = std::chrono::steady_clock::now() - 20s;
    selected->headers = {{"job", "test"}};
    const auto expected = *selected;
    q.requeue_front(std::move(*selected));
    EXPECT_EQ(q.size(), 2u);
    EXPECT_EQ(q.size(1), 2u);
    EXPECT_EQ(q.size(2), 0u);
    EXPECT_EQ(QueueTestAccess::expiry_size(q), 0u);
    q.enqueue(make_msg(0, "higher"));
    auto higher = q.try_dequeue();
    ASSERT_TRUE(higher);
    EXPECT_EQ(higher->id, "higher");
    auto restored = q.try_dequeue();
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->id, "A");
    EXPECT_EQ(restored->priority, expected.priority);
    EXPECT_EQ(restored->original_priority, expected.original_priority);
    EXPECT_EQ(restored->retry_count, expected.retry_count);
    EXPECT_EQ(restored->max_retries, expected.max_retries);
    EXPECT_EQ(restored->enqueue_time, expected.enqueue_time);
    EXPECT_EQ(restored->arrival_time, expected.arrival_time);
    EXPECT_EQ(restored->ttl, expected.ttl);
    EXPECT_EQ(restored->payload, expected.payload);
    EXPECT_EQ(restored->headers, expected.headers);
    auto next = q.try_dequeue();
    ASSERT_TRUE(next);
    EXPECT_EQ(next->id, "B");
    EXPECT_TRUE(q.empty());
}

TEST(MultiLevelQueue, RestoreFrontRejectsInvalidPriority) {
    MultiLevelQueue q;
    EXPECT_THROW(q.requeue_front(make_msg(3)), std::out_of_range);
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(QueueTestAccess::expiry_size(q), 0u);
}

TEST(MultiLevelQueue, RestoreFrontNotifiesDequeuerAndRemainsDrainableAfterShutdown) {
    MultiLevelQueue q;
    q.enqueue(make_msg(1, "restored"));
    auto selected = q.try_dequeue();
    ASSERT_TRUE(selected);
    std::promise<void> started;
    auto ready = started.get_future();
    auto pending = std::async(std::launch::async, [&] {
        started.set_value();
        return q.dequeue(5s);
    });
    ready.wait();
    EXPECT_EQ(pending.wait_for(50ms), std::future_status::timeout);
    q.requeue_front(std::move(*selected));
    EXPECT_EQ(pending.wait_for(2s), std::future_status::ready);
    auto received = pending.get();
    ASSERT_TRUE(received);
    EXPECT_EQ(received->id, "restored");
    q.shutdown();
    q.requeue_front(std::move(*received));
    auto drained = q.dequeue(1s);
    ASSERT_TRUE(drained);
    EXPECT_EQ(drained->id, "restored");
    EXPECT_FALSE(q.dequeue(1s));
}

// ── Blocking dequeue ──────────────────────────────────────────────────────────

TEST(MultiLevelQueue, BlockingDequeueTimesOut) {
    MultiLevelQueue q{3};
    const auto start = std::chrono::steady_clock::now();
    auto result = q.dequeue(20ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(result.has_value());
    EXPECT_GE(elapsed, 20ms);
}

TEST(MultiLevelQueue, BlockingDequeueReceivesEnqueuedMessage) {
    MultiLevelQueue q{3};

    // Enqueue from another thread after a short delay.
    std::thread producer([&q] {
        std::this_thread::sleep_for(20ms);
        q.enqueue(make_msg(0, "hello"));
    });

    auto result = q.dequeue(500ms);
    producer.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->id, "hello");
}

TEST(MultiLevelQueue, ShutdownUnblocksDequeue) {
    MultiLevelQueue q{3};

    auto fut = std::async(std::launch::async, [&q] {
        return q.dequeue(10s); // would block for 10 s without shutdown
    });

    std::this_thread::sleep_for(20ms);
    q.shutdown();

    auto result = fut.get();
    // Shutdown: either nullopt or (if any messages were present) a message.
    // Since queue is empty, expect nullopt.
    EXPECT_FALSE(result.has_value());
}

// ── Concurrency ───────────────────────────────────────────────────────────────

TEST(MultiLevelQueue, ConcurrentEnqueueDequeue) {
    constexpr int kMessages = 1000;
    MultiLevelQueue q{3};

    std::atomic<int> consumed{0};

    std::thread producer([&] {
        for (int i = 0; i < kMessages; ++i) {
            auto m = make_msg(static_cast<uint8_t>(i % 3));
            m.id = std::to_string(i);
            q.enqueue(std::move(m));
        }
    });

    std::thread consumer([&] {
        while (consumed.load() < kMessages) {
            if (q.try_dequeue()) {
                consumed.fetch_add(1);
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(consumed.load(), kMessages);
    EXPECT_TRUE(q.empty());
}

// ── Aging ─────────────────────────────────────────────────────────────────────

TEST(MultiLevelQueue, AgingPromotesLowPriorityMessage) {
    AgingConfig aging{.threshold = 50ms, .interval = 10ms};
    MultiLevelQueue q{3, aging};

    // Enqueue at lowest priority level.
    q.enqueue(make_msg(2, "aged-msg"));

    EXPECT_EQ(q.size(2), 1u);

    // Wait long enough for aging to promote it twice (2→1→0).
    std::this_thread::sleep_for(200ms);

    // The message should now be at level 0 (highest).
    EXPECT_EQ(q.size(0), 1u);
    EXPECT_EQ(q.size(2), 0u);

    auto msg = q.try_dequeue();
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->id, "aged-msg");
    EXPECT_EQ(msg->priority, 0u);
}

// ── TTL expiry ────────────────────────────────────────────────────────────────

namespace {

Message make_ttl_msg(uint8_t priority, std::string id,
                     std::chrono::milliseconds age,
                     std::chrono::milliseconds ttl = 100ms) {
    Message m;
    m.priority = priority;
    m.original_priority = priority;
    m.id = std::move(id);
    m.arrival_time =
        std::chrono::steady_clock::now() - age; // backdate to control expiry
    m.ttl = ttl;
    return m;
}

} // namespace

TEST(MultiLevelQueue, SweepExpiredRemovesOnlyExpired) {
    MultiLevelQueue q{3};

    q.enqueue(make_ttl_msg(2, "expired-low", 500ms));
    q.enqueue(make_ttl_msg(0, "live-high", 0ms));
    q.enqueue(make_ttl_msg(1, "expired-mid", 500ms, 50ms));
    ASSERT_EQ(q.size(), 3u);

    const auto expired = q.sweep_expired();

    ASSERT_EQ(expired.size(), 2u);
    EXPECT_EQ(expired[0].id, "expired-mid"); // level order: 1 before 2
    EXPECT_EQ(expired[1].id, "expired-low");
    EXPECT_EQ(q.size(), 1u);
    const auto rest = q.try_dequeue();
    ASSERT_TRUE(rest.has_value());
    EXPECT_EQ(rest->id, "live-high");
}

TEST(MultiLevelQueue, SweepExpiredEmptyWhenAllLive) {
    MultiLevelQueue q{3};
    q.enqueue(make_ttl_msg(0, "live", 0ms, 0ms)); // ttl=0 never expires

    EXPECT_TRUE(q.sweep_expired().empty());
    EXPECT_EQ(q.size(), 1u);
}

TEST(MultiLevelQueue, AgingSkipsExpiredMessages) {
    AgingConfig aging{.threshold = 50ms, .interval = 10ms};
    MultiLevelQueue q{3, aging};

    q.enqueue(make_ttl_msg(2, "dead-msg", 500ms)); // already expired

    // Wait past several aging scans: a live message would reach level 0.
    std::this_thread::sleep_for(200ms);

    EXPECT_EQ(q.size(0), 0u); // never promoted toward level 0
    EXPECT_EQ(q.size(2), 1u);

    // The sweeper path still reclaims it.
    const auto expired = q.sweep_expired();
    ASSERT_EQ(expired.size(), 1u);
    EXPECT_EQ(expired[0].id, "dead-msg");
    EXPECT_TRUE(q.empty());
}

TEST(MultiLevelQueue, IndexedSweepIsBoundedAndPreservesLiveFifo) {
    MultiLevelQueue q;
    for (int i = 0; i < 10000; ++i) q.enqueue(make_msg(1, std::to_string(i)));
    EXPECT_EQ(QueueTestAccess::expiry_size(q), 0u);
    for (int i = 0; i < 5; ++i) q.enqueue(make_ttl_msg(1, "expired", 1s));
    EXPECT_TRUE(q.sweep_expired_batch(0).empty());
    EXPECT_EQ(q.sweep_expired_batch(2).size(), 2u);
    EXPECT_EQ(QueueTestAccess::expiry_size(q), 3u);
    EXPECT_EQ(q.sweep_expired_batch(10).size(), 3u);
    EXPECT_EQ(QueueTestAccess::expiry_size(q), 0u);
    for (int i = 0; i < 10000; ++i) {
        auto msg = q.try_dequeue();
        ASSERT_TRUE(msg);
        EXPECT_EQ(msg->id, std::to_string(i));
    }
}

TEST(MultiLevelQueue, ExpiryIndexSurvivesPromotionAndRequeue) {
    MultiLevelQueue q{3, AgingConfig{10ms, 5ms}};
    q.enqueue(make_ttl_msg(2, "moving", 0ms, 30s));
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (q.size(0) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    EXPECT_EQ(q.size(0), 1u);
    EXPECT_EQ(QueueTestAccess::expiry_size(q), 1u);
    auto msg = q.try_dequeue();
    ASSERT_TRUE(msg);
    EXPECT_EQ(QueueTestAccess::expiry_size(q), 0u);
    msg->priority = msg->original_priority;
    msg->arrival_time -= 60s;
    q.enqueue(std::move(*msg));
    EXPECT_EQ(q.sweep_expired_batch(1).size(), 1u);
    EXPECT_EQ(QueueTestAccess::expiry_size(q), 0u);
}

TEST(MultiLevelQueue, HugeTtlDoesNotOverflow) {
    MultiLevelQueue q;
    auto msg = make_ttl_msg(1, "forever", 0ms, std::chrono::milliseconds::max());
    EXPECT_FALSE(msg.is_expired());
    q.enqueue(msg);
    EXPECT_TRUE(q.sweep_expired_batch(10).empty());
    EXPECT_TRUE(q.try_dequeue());
    EXPECT_EQ(QueueTestAccess::expiry_size(q), 0u);
}

TEST(MultiLevelQueue, RestoreFrontRebuildsExpiryIndexWithoutExtendingTtl) {
    MultiLevelQueue q;
    q.enqueue(make_ttl_msg(1, "live", 0ms, 30s));
    EXPECT_EQ(QueueTestAccess::expiry_size(q), 1u);
    auto selected = q.try_dequeue();
    ASSERT_TRUE(selected);
    const auto deadline = selected->expiry_time();
    EXPECT_EQ(QueueTestAccess::expiry_size(q), 0u);
    q.requeue_front(std::move(*selected));
    EXPECT_EQ(QueueTestAccess::expiry_size(q), 1u);
    selected = q.try_dequeue();
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected->expiry_time(), deadline);
    EXPECT_EQ(QueueTestAccess::expiry_size(q), 0u);
    selected->arrival_time -= 60s;
    q.requeue_front(std::move(*selected));
    EXPECT_EQ(QueueTestAccess::expiry_size(q), 1u);
    auto expired = q.sweep_expired_batch(1);
    ASSERT_EQ(expired.size(), 1u);
    EXPECT_EQ(expired.front().id, "live");
    EXPECT_EQ(expired.front().ttl, 30s);
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(QueueTestAccess::expiry_size(q), 0u);
}

TEST(MultiLevelQueue, AgingDoesNotStealEnqueueWakeup) {
    MultiLevelQueue q{3, AgingConfig{10s, 10s}};
    for (int i = 0; i < 20; ++i) {
        auto pending = std::async(std::launch::async, [&] { return q.dequeue(2s); });
        std::this_thread::sleep_for(2ms);
        q.enqueue(make_msg(1));
        EXPECT_EQ(pending.wait_for(500ms), std::future_status::ready);
        EXPECT_TRUE(pending.get());
    }
}

TEST(MultiLevelQueue, ConcurrentShutdownWakesAllWaiters) {
    MultiLevelQueue q{3, AgingConfig{10s, 10s}};
    auto waiter = std::async(std::launch::async, [&] { return q.dequeue(10s); });
    auto first = std::async(std::launch::async, [&] { q.shutdown(); });
    auto second = std::async(std::launch::async, [&] { q.shutdown(); });
    EXPECT_EQ(first.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(second.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(waiter.get());
}
