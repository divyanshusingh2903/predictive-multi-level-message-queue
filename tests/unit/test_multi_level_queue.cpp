#include "queue/multi_level_queue.hpp"
#include "ml/routing_context.hpp"

#include <gtest/gtest.h>

#include <future>
#include <limits>
#include <random>
#include <thread>
#include <vector>

using namespace harbinger;
using namespace std::chrono_literals;

namespace harbinger {
struct QueueTestAccess {
    static std::size_t age(MultiLevelQueue& q, std::chrono::steady_clock::time_point now) {
        std::lock_guard lock{q.mutex_};
        return q.age_once_locked(now);
    }
    static std::vector<std::vector<Message>> contents(MultiLevelQueue& q) {
        std::lock_guard lock{q.mutex_};
        std::vector<std::vector<Message>> levels(q.num_levels_);
        for (std::size_t i = 0; i < levels.size(); ++i)
            for (const auto& node : q.queues_[i]) levels[i].push_back(node.message);
        return levels;
    }
    static void check_cache(MultiLevelQueue& q) {
        std::lock_guard lock{q.mutex_};
        for (uint8_t level = 0; level < q.num_levels_; ++level) {
            std::size_t promoted = 0;
            for (const auto& node : q.queues_[level]) promoted += node.message.priority < node.message.original_priority;
            EXPECT_EQ(q.promoted_in_[level], promoted) << "level " << level;
        }
        for (uint8_t level = 1; level < q.num_levels_; ++level) {
            for (const auto& node : q.queues_[level]) {
                if (node.message.is_expired()) continue;
                const auto due = q.aging_deadline(node.message.enqueue_time);
                if (due) {
                    ASSERT_TRUE(q.next_aging_[level]);
                    EXPECT_LE(*q.next_aging_[level], *due);
                }
            }
        }
    }
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

TEST(MultiLevelQueue, RoundRobinSkipsEmptyLevelsAndWrapsWithFifo) {
    MultiLevelQueue q{3, std::nullopt, QueueSelection::RoundRobin};
    q.enqueue(make_msg(0, "a")); q.enqueue(make_msg(0, "b"));
    q.enqueue(make_msg(2, "c")); q.enqueue(make_msg(2, "d"));
    for (const auto* id : {"a", "c", "b", "d"}) EXPECT_EQ(q.try_dequeue()->id, id);
    EXPECT_FALSE(q.try_dequeue());
}

TEST(MultiLevelQueue, RoundRobinSupportsOneAnd255LevelsAndRestoration) {
    for (const uint8_t levels : {uint8_t{1}, uint8_t{255}}) {
        MultiLevelQueue q{levels, std::nullopt, QueueSelection::RoundRobin};
        q.enqueue(make_msg(levels - 1, "last"));
        auto msg = *q.try_dequeue();
        const auto placed = msg.enqueue_time;
        q.requeue_front(std::move(msg));
        auto restored = *q.try_dequeue();
        EXPECT_EQ(restored.id, "last"); EXPECT_EQ(restored.enqueue_time, placed);
    }
}

TEST(MultiLevelQueue, RoundRobinUsesSharedAgingWithoutResettingOriginalPriority) {
    MultiLevelQueue q{3, AgingConfig{10ms, 1h}, QueueSelection::RoundRobin};
    auto msg = make_msg(2, "old");
    msg.enqueue_time = std::chrono::steady_clock::now() - 100ms;
    q.requeue_front(msg);
    QueueTestAccess::age(q, std::chrono::steady_clock::now());
    auto promoted = *q.try_dequeue();
    EXPECT_EQ(promoted.priority, 1); EXPECT_EQ(promoted.original_priority, 2);
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
    EXPECT_THROW((void)q.size(3), std::out_of_range);
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
    selected->routing_context = std::make_shared<const ml::RoutingContext>(ml::RoutingContext{
        .feature_schema_version = "features-v1", .routing_policy_version = "policy-v1", .ingress_priority = 2});
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
    EXPECT_EQ(restored->routing_context, expected.routing_context);
    auto next = q.try_dequeue();
    ASSERT_TRUE(next);
    EXPECT_EQ(next->id, "B");
    EXPECT_TRUE(q.empty());
}

TEST(MultiLevelQueue, AgingAndExpiryPreserveImmutableIngressContext) {
    MultiLevelQueue q{3, AgingConfig{10ms, 1h}};
    auto message = make_msg(2, "context");
    message.routing_context = std::make_shared<const ml::RoutingContext>(ml::RoutingContext{
        .feature_schema_version = "features-v1", .routing_policy_version = "policy-v1",
        .features = ml::FeatureSnapshot{.payload_size_bytes = 123}, .ingress_priority = 2});
    const auto context = message.routing_context;
    q.enqueue(std::move(message));
    const auto placed = QueueTestAccess::contents(q)[2][0];
    QueueTestAccess::age(q, placed.enqueue_time + 11ms);
    auto aged = q.try_dequeue();
    ASSERT_TRUE(aged);
    EXPECT_EQ(aged->priority, 1);
    EXPECT_EQ(aged->original_priority, 2);
    EXPECT_EQ(aged->routing_context, context);
    EXPECT_EQ(context->ingress_priority, 2);
    EXPECT_EQ(context->features->payload_size_bytes, 123u);
    aged->ttl = 1ms;
    aged->arrival_time = std::chrono::steady_clock::now() - 1s;
    q.requeue_front(std::move(*aged));
    const auto expired = q.sweep_expired_batch(1);
    ASSERT_EQ(expired.size(), 1u);
    EXPECT_EQ(expired[0].routing_context, context);
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

TEST(MultiLevelQueue, AgingSkipsYoungLevelsButFindsOldWorkBehindYoungHead) {
    MultiLevelQueue q{3, AgingConfig{100ms, 1h}};
    q.shutdown(); // Controlled passes without a racing background thread.
    const auto now = std::chrono::steady_clock::now();
    auto old = make_msg(2, "old");
    old.enqueue_time = now - 100ms;
    q.requeue_front(old);
    auto young = make_msg(2, "young-front");
    young.enqueue_time = now;
    q.requeue_front(young);
    q.enqueue(make_msg(1, "young-level"));
    EXPECT_EQ(QueueTestAccess::age(q, now - 1ms), 0u);
    EXPECT_EQ(QueueTestAccess::age(q, now), 2u);
    auto levels = QueueTestAccess::contents(q);
    ASSERT_EQ(levels[1].size(), 2u);
    EXPECT_EQ(levels[1][0].id, "young-level");
    EXPECT_EQ(levels[1][1].id, "old");
    EXPECT_EQ(levels[1][1].enqueue_time, now);
    EXPECT_EQ(levels[1][1].original_priority, 2);
    EXPECT_EQ(QueueTestAccess::age(q, now + 99ms), 0u);
    QueueTestAccess::check_cache(q);
}

TEST(MultiLevelQueue, AgingRetainsFifoAcrossSourcesAndResetsDestinationDeadlines) {
    MultiLevelQueue q{4, AgingConfig{100ms, 1h}};
    q.shutdown();
    const auto now = std::chrono::steady_clock::now();
    for (uint8_t level = 1; level <= 3; ++level) {
        auto older = make_msg(level, "older-" + std::to_string(level));
        older.enqueue_time = now - 300ms;
        q.requeue_front(older);
        auto front = make_msg(level, "front-" + std::to_string(level));
        front.enqueue_time = now - 100ms;
        q.requeue_front(front);
    }
    q.enqueue(make_msg(0, "existing"));
    EXPECT_EQ(QueueTestAccess::age(q, now), 10u); // Includes new, young destination nodes.
    const auto levels = QueueTestAccess::contents(q);
    ASSERT_EQ(levels[0].size(), 3u);
    EXPECT_EQ(levels[0][0].id, "existing");
    EXPECT_EQ(levels[0][1].id, "front-1");
    EXPECT_EQ(levels[0][2].id, "older-1");
    for (std::size_t level = 1; level <= 2; ++level) {
        ASSERT_EQ(levels[level].size(), 2u);
        EXPECT_EQ(levels[level][0].id, "front-" + std::to_string(level + 1));
        EXPECT_EQ(levels[level][1].id, "older-" + std::to_string(level + 1));
        EXPECT_EQ(levels[level][0].enqueue_time, now);
    }
    EXPECT_EQ(QueueTestAccess::age(q, now + 99ms), 0u);
    EXPECT_GT(QueueTestAccess::age(q, now + 100ms), 0u);
    QueueTestAccess::check_cache(q);
}

TEST(MultiLevelQueue, AgingCacheHandlesRemovedOldestExpiredWorkAndRefill) {
    MultiLevelQueue q{3, AgingConfig{100ms, 1h}};
    q.shutdown();
    const auto now = std::chrono::steady_clock::now();
    auto old = make_msg(1, "removed");
    old.enqueue_time = now - 1s;
    auto young = make_msg(1, "young");
    young.enqueue_time = now;
    q.requeue_front(young);
    q.requeue_front(old);
    ASSERT_TRUE(q.try_dequeue());
    EXPECT_EQ(QueueTestAccess::age(q, now), 1u); // Conservative stale minimum costs one scan.
    EXPECT_EQ(QueueTestAccess::age(q, now), 0u);
    auto dead = make_ttl_msg(2, "expired", 1s);
    dead.enqueue_time = now - 1s;
    q.requeue_front(dead);
    EXPECT_EQ(QueueTestAccess::age(q, now), 1u);
    EXPECT_EQ(QueueTestAccess::age(q, now), 0u); // Expired node never becomes promotable.
    EXPECT_EQ(QueueTestAccess::expiry_size(q), 1u);
    ASSERT_EQ(q.sweep_expired_batch(1).size(), 1u);
    ASSERT_TRUE(q.try_dequeue());
    q.requeue_front(old);
    EXPECT_GT(QueueTestAccess::age(q, now), 0u);
    auto promoted = q.try_dequeue();
    ASSERT_TRUE(promoted);
    EXPECT_EQ(promoted->priority, 0);
    EXPECT_TRUE(q.empty());
    QueueTestAccess::check_cache(q);
}

TEST(MultiLevelQueue, AgingDeadlineArithmeticDoesNotWrap) {
    using Clock = std::chrono::steady_clock;
    MultiLevelQueue huge{3, AgingConfig{std::chrono::milliseconds::max(), 1h}};
    huge.shutdown();
    auto old = make_msg(2, "huge-threshold");
    old.enqueue_time = Clock::now() - 1h;
    huge.requeue_front(old);
    EXPECT_EQ(QueueTestAccess::age(huge, Clock::now()), 0u);
    MultiLevelQueue near_limit{3, AgingConfig{100ms, 1h}};
    near_limit.shutdown();
    auto future = make_msg(2, "future");
    future.enqueue_time = Clock::time_point::max() - 1ms;
    near_limit.requeue_front(future);
    EXPECT_EQ(QueueTestAccess::age(near_limit, Clock::time_point::max()), 0u);
    EXPECT_EQ(near_limit.size(2), 1u);
}

TEST(MultiLevelQueue, CachedAgingMatchesFullScanAfterMixedOperations) {
    using Clock = std::chrono::steady_clock;
    MultiLevelQueue q{4, AgingConfig{100ms, 1h}};
    q.shutdown();
    std::vector<std::vector<Message>> reference(4);
    std::mt19937 random{42};
    auto now = Clock::now();
    for (int step = 0; step < 300; ++step) {
        SCOPED_TRACE(step);
        const auto op = random() % 5;
        if (op <= 1) {
            const auto level = static_cast<uint8_t>(random() % 4);
            auto msg = make_msg(level, std::to_string(step));
            msg.enqueue_time = now - std::chrono::milliseconds{static_cast<int>(random() % 200)};
            if (op == 0) {
                q.requeue_front(msg);
                reference[level].insert(reference[level].begin(), msg);
            } else {
                q.enqueue(msg);
                reference[level].push_back(QueueTestAccess::contents(q)[level].back());
            }
        } else if (op == 2) {
            auto selected = q.try_dequeue();
            std::optional<Message> expected;
            for (auto& level : reference) {
                if (!level.empty()) {
                    expected = level.front();
                    level.erase(level.begin());
                    break;
                }
            }
            ASSERT_EQ(selected.has_value(), expected.has_value());
            if (selected) {
                EXPECT_EQ(selected->id, expected->id);
                q.requeue_front(*selected);
                reference[selected->priority].insert(reference[selected->priority].begin(), *expected);
            }
        } else if (op == 3) {
            auto expired = make_ttl_msg(2, "expired-" + std::to_string(step), 1s);
            expired.enqueue_time = now - 1s;
            q.requeue_front(expired);
            reference[2].insert(reference[2].begin(), expired);
            (void)q.sweep_expired_batch(1);
            reference[2].erase(reference[2].begin());
        } else {
            now += 50ms;
            (void)QueueTestAccess::age(q, now);
            for (std::size_t level = 3; level >= 1; --level) {
                auto& source = reference[level];
                for (auto it = source.begin(); it != source.end();) {
                    if (now - it->enqueue_time >= 100ms && !it->is_expired()) {
                        auto msg = *it;
                        it = source.erase(it);
                        msg.priority = static_cast<uint8_t>(level - 1);
                        msg.enqueue_time = now;
                        reference[level - 1].push_back(std::move(msg));
                    } else ++it;
                }
            }
        }
        const auto actual = QueueTestAccess::contents(q);
        for (std::size_t level = 0; level < reference.size(); ++level) {
            ASSERT_EQ(actual[level].size(), reference[level].size());
            for (std::size_t i = 0; i < reference[level].size(); ++i) {
                EXPECT_EQ(actual[level][i].id, reference[level][i].id);
                EXPECT_EQ(actual[level][i].priority, reference[level][i].priority);
                EXPECT_EQ(actual[level][i].original_priority, reference[level][i].original_priority);
                EXPECT_EQ(actual[level][i].enqueue_time, reference[level][i].enqueue_time);
            }
        }
        QueueTestAccess::check_cache(q);
    }
}

// ── Worker-time level share and pausing aging (#40) ─────────────────────────────

namespace {

/// Cost read from the "cost" header so tests control each message's expected duration.
LevelShare header_cost_share(std::vector<uint32_t> weights) {
    return {std::move(weights), [](const Message& m) {
        const auto found = m.headers.find("cost");
        return found == m.headers.end() ? 1.0 : std::stod(found->second);
    }};
}

Message costed(uint8_t priority, double cost, std::string id = {}) {
    Message m = make_msg(priority, std::move(id));
    m.headers["cost"] = std::to_string(cost);
    return m;
}

} // namespace

TEST(MultiLevelQueue, WeightedShareSplitsWorkerTimeNotPulls) {
    MultiLevelQueue q{3, std::nullopt, QueueSelection::WeightedTime, header_cost_share({8, 3, 1})};
    const double costs[] = {2.0, 30.0, 600.0};
    for (int i = 0; i < 10; ++i)
        for (uint8_t level = 0; level < 3; ++level) q.enqueue(costed(level, costs[level]));
    // Keep every level backlogged: each pulled message is replaced by another of the same level.
    for (int i = 0; i < 200000; ++i) {
        const auto m = q.try_dequeue();
        ASSERT_TRUE(m);
        q.enqueue(costed(m->priority, costs[m->priority]));
    }
    const auto stats = q.share_stats();
    const double total = stats.charged_ms[0] + stats.charged_ms[1] + stats.charged_ms[2];
    // Worker time splits 8:3:1; the error is bounded by one job's cost per level, tiny against this total.
    const double slack = 2 * costs[2] / total;
    EXPECT_NEAR(stats.charged_ms[0] / total, 8.0 / 12, slack);
    EXPECT_NEAR(stats.charged_ms[1] / total, 3.0 / 12, slack);
    EXPECT_NEAR(stats.charged_ms[2] / total, 1.0 / 12, slack);
    EXPECT_LT(slack, 0.005);
    // Pulls are nowhere near 8:3:1: short jobs are pulled far more often for the same share of time.
    EXPECT_GT(stats.pulls[0], 50 * stats.pulls[2]);
}

TEST(MultiLevelQueue, WeightedShareIsWorkConserving) {
    MultiLevelQueue q{3, std::nullopt, QueueSelection::WeightedTime, header_cost_share({8, 3, 1})};
    for (int i = 0; i < 5; ++i) q.enqueue(costed(2, 100.0, "long-" + std::to_string(i)));
    for (int i = 0; i < 5; ++i) {
        auto m = q.try_dequeue();
        ASSERT_TRUE(m);
        EXPECT_EQ(m->priority, 2);  // only the bottom level has work, so it gets every pull
    }
    EXPECT_FALSE(q.try_dequeue());
}

TEST(MultiLevelQueue, EmptyLevelDoesNotSaveUpCredit) {
    MultiLevelQueue q{2, std::nullopt, QueueSelection::WeightedTime, header_cost_share({1, 1})};
    for (int i = 0; i < 100; ++i) q.enqueue(costed(1, 1.0));
    for (int i = 0; i < 90; ++i) ASSERT_EQ(q.try_dequeue()->priority, 1);  // level 0 idle the whole time
    for (int i = 0; i < 10; ++i) q.enqueue(costed(0, 1.0));
    int top = 0;
    for (int i = 0; i < 10; ++i) top += q.try_dequeue()->priority == 0;
    EXPECT_GE(top, 4);  // equal weights: the returning level gets about half,
    EXPECT_LE(top, 6);  // not a burst of all ten for the time it sat empty
}

TEST(MultiLevelQueue, RequeueFrontRefundsTheCharge) {
    const auto run = [](bool with_requeue) {
        MultiLevelQueue q{2, std::nullopt, QueueSelection::WeightedTime, header_cost_share({1, 1})};
        for (int i = 0; i < 6; ++i) {
            q.enqueue(costed(0, 10.0, "a" + std::to_string(i)));
            q.enqueue(costed(1, 10.0, "b" + std::to_string(i)));
        }
        if (with_requeue) q.requeue_front(*q.try_dequeue());  // an uncommitted pull is undone
        std::vector<std::string> order;
        while (auto m = q.try_dequeue()) order.push_back(m->id);
        return std::make_pair(order, q.share_stats());
    };
    const auto [plain, plain_stats] = run(false);
    const auto [undone, undone_stats] = run(true);
    EXPECT_EQ(plain, undone);
    EXPECT_EQ(plain_stats.pulls, undone_stats.pulls);
    EXPECT_EQ(plain_stats.charged_ms, undone_stats.charged_ms);
}

TEST(MultiLevelQueue, WeightedShareRejectsBadConfigurationAndBadCosts) {
    EXPECT_THROW((MultiLevelQueue{3, std::nullopt, QueueSelection::WeightedTime, header_cost_share({8, 3})}), std::invalid_argument);
    EXPECT_THROW((MultiLevelQueue{2, std::nullopt, QueueSelection::WeightedTime, header_cost_share({0, 1})}), std::invalid_argument);
    EXPECT_THROW((MultiLevelQueue{2, std::nullopt, QueueSelection::WeightedTime, header_cost_share({1, 1001})}), std::invalid_argument);
    EXPECT_THROW((MultiLevelQueue{2, std::nullopt, QueueSelection::WeightedTime, LevelShare{{1, 1}, {}}}), std::invalid_argument);
    EXPECT_THROW((MultiLevelQueue{2, std::nullopt, QueueSelection::WeightedTime}), std::invalid_argument);
    EXPECT_THROW((MultiLevelQueue{2, std::nullopt, QueueSelection::StrictPriority, header_cost_share({1, 1})}), std::invalid_argument);
    // A cost that is negative, not finite, below the floor or throws is charged the minimum, never corrupting clocks.
    MultiLevelQueue q{2, std::nullopt, QueueSelection::WeightedTime, LevelShare{{1, 1}, [](const Message& m) -> double {
        if (m.id == "throws") throw std::runtime_error("cost");
        if (m.id == "nan") return std::numeric_limits<double>::quiet_NaN();
        return m.id == "negative" ? -5.0 : 0.25;
    }}};
    for (const char* id : {"throws", "nan", "negative", "tiny"}) q.enqueue(make_msg(0, id));
    while (q.try_dequeue()) {}
    EXPECT_DOUBLE_EQ(q.share_stats().charged_ms[0], 4 * LevelShare::kMinCostMs);
}

namespace {

/// A message that entered the broker at `arrival`, as the proxy would stamp it.
Message arrived(uint8_t priority, std::string id, std::chrono::steady_clock::time_point arrival) {
    Message m = make_msg(priority, std::move(id));
    m.arrival_time = arrival;
    return m;
}

AgingConfig pausing_aging() { return {std::chrono::milliseconds{1000}, std::chrono::hours{1}, true}; }

} // namespace

TEST(MultiLevelQueue, PausingAgingPromotesIntoALevelThatOnlyWaitsNatively) {
    // Level 0 holds an old message of its own, nothing promoted: promotion is not piling onto a backlog it made.
    const auto start = std::chrono::steady_clock::now();
    MultiLevelQueue q{2, pausing_aging()};
    q.enqueue(arrived(0, "old-top", start));
    q.enqueue(arrived(1, "waiting", start));
    QueueTestAccess::age(q, start + 2s);
    EXPECT_EQ(QueueTestAccess::contents(q)[0].size(), 2u);
    QueueTestAccess::check_cache(q);
}

TEST(MultiLevelQueue, PausingAgingStopsTheFloodAfterAPromotion) {
    const auto start = std::chrono::steady_clock::now();
    MultiLevelQueue q{2, pausing_aging()};
    q.enqueue(arrived(0, "native", start));
    q.enqueue(arrived(1, "a", start));
    q.enqueue(arrived(1, "b", start));
    q.enqueue(arrived(1, "c", start));
    // Pass 1 promotes one message (and only one), pass 2 finds level 0 holding a promoted message with an old head.
    EXPECT_GT(QueueTestAccess::age(q, start + 2s), 0u);
    EXPECT_EQ(QueueTestAccess::contents(q)[0].size(), 2u);
    QueueTestAccess::age(q, start + 2s + 600ms);
    EXPECT_EQ(QueueTestAccess::contents(q)[0].size(), 2u);
    EXPECT_EQ(QueueTestAccess::contents(q)[1].size(), 2u);
    QueueTestAccess::check_cache(q);
    // Resumes once the promoted message is served and the level above is clear.
    ASSERT_EQ(q.try_dequeue()->id, "native");
    ASSERT_EQ(q.try_dequeue()->id, "a");
    QueueTestAccess::age(q, start + 3s);
    EXPECT_EQ(QueueTestAccess::contents(q)[0].size(), 1u);
    QueueTestAccess::check_cache(q);
}

TEST(MultiLevelQueue, PausingAgingMovesAnOverdueBacklogOneMessagePerPass) {
    const auto start = std::chrono::steady_clock::now();
    MultiLevelQueue q{3, pausing_aging()};
    for (int i = 0; i < 50; ++i) q.enqueue(arrived(2, "bulk-" + std::to_string(i), start));
    QueueTestAccess::age(q, start + 2s);
    EXPECT_EQ(QueueTestAccess::contents(q)[1].size(), 1u);
    EXPECT_EQ(QueueTestAccess::contents(q)[2].size(), 49u);
    QueueTestAccess::check_cache(q);
    // The rest stays due: once the promoted message is served, the next pass moves exactly one more.
    ASSERT_EQ(q.try_dequeue()->id, "bulk-0");
    QueueTestAccess::age(q, start + 2s + 100ms);
    const auto levels = QueueTestAccess::contents(q);
    ASSERT_EQ(levels[1].size(), 1u);
    EXPECT_EQ(levels[1][0].id, "bulk-1");
    EXPECT_EQ(levels[2].size(), 48u);
    QueueTestAccess::check_cache(q);
}

TEST(MultiLevelQueue, PausingAgingCountsWaitFromArrivalNotFromPromotion) {
    const auto start = std::chrono::steady_clock::now();
    MultiLevelQueue q{3, pausing_aging()};
    q.enqueue(arrived(2, "old", start));
    q.enqueue(arrived(2, "next", start));
    QueueTestAccess::age(q, start + 2s);  // "old" enters level 1 with a fresh enqueue_time but its original arrival
    // Level 1 now holds a promoted message that has been in the broker 2 s > threshold: it is behind, so level 2 waits.
    QueueTestAccess::age(q, start + 2s + 100ms);
    EXPECT_EQ(QueueTestAccess::contents(q)[1].size(), 1u);
    EXPECT_EQ(QueueTestAccess::contents(q)[2].size(), 1u);
    QueueTestAccess::check_cache(q);
}

TEST(MultiLevelQueue, PausingAgingIgnoresRetriedMessagesAtTheirOriginalLevel) {
    const auto start = std::chrono::steady_clock::now();
    MultiLevelQueue q{2, pausing_aging()};
    Message retried = arrived(0, "retried", start);  // a retry resets priority to original, so it is not "promoted"
    retried.retry_count = 1;
    q.enqueue(std::move(retried));
    q.enqueue(arrived(1, "waiting", start));
    QueueTestAccess::age(q, start + 2s);
    EXPECT_EQ(QueueTestAccess::contents(q)[0].size(), 2u);
}

TEST(MultiLevelQueue, PlainAgingStillPromotesIntoABacklog) {
    AgingConfig aging{std::chrono::milliseconds{1000}, std::chrono::hours{1}, false};
    MultiLevelQueue q{2, aging};
    q.enqueue(make_msg(0, "old-top"));
    q.enqueue(make_msg(1, "waiting"));
    QueueTestAccess::age(q, std::chrono::steady_clock::now() + 2s);
    EXPECT_EQ(QueueTestAccess::contents(q)[0].size(), 2u);  // today's behaviour, unchanged
}

TEST(MultiLevelQueue, PausingAgingPromotesWhenTheLevelAboveIsYoung) {
    AgingConfig aging{std::chrono::milliseconds{1000}, std::chrono::hours{1}, true};
    MultiLevelQueue q{3, aging, QueueSelection::WeightedTime, header_cost_share({8, 3, 1})};
    q.enqueue(make_msg(2, "starved"));
    // Nothing above it, so it climbs one level per due pass like plain aging.
    QueueTestAccess::age(q, std::chrono::steady_clock::now() + 2s);
    EXPECT_EQ(QueueTestAccess::contents(q)[1].size(), 1u);
    QueueTestAccess::age(q, std::chrono::steady_clock::now() + 4s);
    EXPECT_EQ(QueueTestAccess::contents(q)[0].size(), 1u);
    QueueTestAccess::check_cache(q);
}

TEST(MultiLevelQueue, ConcurrentWeightedShareWithPausingAging) {
    AgingConfig aging{std::chrono::milliseconds{2}, std::chrono::milliseconds{1}, true};
    MultiLevelQueue q{3, aging, QueueSelection::WeightedTime, header_cost_share({8, 3, 1})};
    constexpr int kPerProducer = 3000;
    std::atomic<int> received{0};
    std::vector<std::thread> threads;
    for (int p = 0; p < 3; ++p)
        threads.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) q.enqueue(costed(static_cast<uint8_t>(i % 3), 1.0 + (i % 7) * p));
        });
    for (int c = 0; c < 3; ++c)
        threads.emplace_back([&, c] {
            while (received.load() < 3 * kPerProducer) {
                auto m = q.dequeue(1ms);
                if (!m) continue;
                if ((received.load() + c) % 11 == 0) { q.requeue_front(std::move(*m)); continue; }
                received.fetch_add(1);
            }
        });
    for (auto& t : threads) t.join();
    EXPECT_EQ(received.load(), 3 * kPerProducer);
    EXPECT_TRUE(q.empty());
    const auto stats = q.share_stats();
    EXPECT_EQ(stats.pulls[0] + stats.pulls[1] + stats.pulls[2], static_cast<uint64_t>(3 * kPerProducer));
}
