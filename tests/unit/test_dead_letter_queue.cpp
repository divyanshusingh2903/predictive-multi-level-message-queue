#include "queue/dead_letter_queue.hpp"

#include <gtest/gtest.h>

using namespace pmlmq;

namespace {

Message make_msg(std::string id) {
    Message m;
    m.id = std::move(id);
    return m;
}

} // namespace

TEST(DeadLetterQueue, StartsEmpty) {
    DeadLetterQueue dlq;
    EXPECT_TRUE(dlq.empty());
    EXPECT_EQ(dlq.size(), 0u);
}

TEST(DeadLetterQueue, PopEmptyReturnsNullopt) {
    DeadLetterQueue dlq;
    EXPECT_FALSE(dlq.pop().has_value());
}

TEST(DeadLetterQueue, PushIncreasesSize) {
    DeadLetterQueue dlq;
    dlq.push(make_msg("m1"), DLQReason::MAX_RETRIES_EXCEEDED);
    EXPECT_EQ(dlq.size(), 1u);
    EXPECT_FALSE(dlq.empty());
}

TEST(DeadLetterQueue, PopDecreasesSize) {
    DeadLetterQueue dlq;
    dlq.push(make_msg("m1"), DLQReason::TTL_EXPIRED);
    auto entry = dlq.pop();
    ASSERT_TRUE(entry.has_value());
    EXPECT_TRUE(dlq.empty());
}

TEST(DeadLetterQueue, FIFOOrdering) {
    DeadLetterQueue dlq;
    dlq.push(make_msg("first"),  DLQReason::MAX_RETRIES_EXCEEDED);
    dlq.push(make_msg("second"), DLQReason::TTL_EXPIRED);

    auto e1 = dlq.pop();
    auto e2 = dlq.pop();

    ASSERT_TRUE(e1.has_value());
    ASSERT_TRUE(e2.has_value());
    EXPECT_EQ(e1->message.id, "first");
    EXPECT_EQ(e2->message.id, "second");
}

TEST(DeadLetterQueue, EntryHasCorrectReason) {
    DeadLetterQueue dlq;
    dlq.push(make_msg("m"), DLQReason::PROCESSING_ERROR, "handler threw");

    auto entry = dlq.pop();
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->reason, DLQReason::PROCESSING_ERROR);
    EXPECT_EQ(entry->details, "handler threw");
}

TEST(DeadLetterQueue, EntryDlqTimeIsSet) {
    DeadLetterQueue dlq;
    const auto before = std::chrono::steady_clock::now();
    dlq.push(make_msg("m"), DLQReason::TTL_EXPIRED);
    const auto after = std::chrono::steady_clock::now();

    auto entry = dlq.pop();
    ASSERT_TRUE(entry.has_value());
    EXPECT_GE(entry->dlq_time, before);
    EXPECT_LE(entry->dlq_time, after);
}

TEST(DeadLetterQueue, MessagePreservedInEntry) {
    DeadLetterQueue dlq;
    Message m;
    m.id = "preserve-me";
    m.priority = 2;
    m.retry_count = 3;
    m.headers["key"] = "value";

    dlq.push(m, DLQReason::MAX_RETRIES_EXCEEDED);
    auto entry = dlq.pop();

    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->message.id, "preserve-me");
    EXPECT_EQ(entry->message.priority, 2u);
    EXPECT_EQ(entry->message.retry_count, 3u);
    EXPECT_EQ(entry->message.headers.at("key"), "value");
}
