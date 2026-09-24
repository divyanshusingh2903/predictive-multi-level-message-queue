#include "proxy/proxy.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>

using namespace harbinger;

namespace {

struct SinkCapture {
    std::vector<Message> messages;
    Proxy::MessageSink sink() {
        return [this](Message msg) { messages.push_back(std::move(msg)); };
    }
};

} // namespace

TEST(Proxy, NullSinkThrows) {
    EXPECT_THROW(Proxy{nullptr}, std::invalid_argument);
}

TEST(Proxy, AcceptReturnsGeneratedId) {
    SinkCapture cap;
    Proxy proxy{cap.sink()};

    const std::string id = proxy.accept({0x01}, {}, "prod-1");
    EXPECT_FALSE(id.empty());
}

TEST(Proxy, AcceptIdMatchesMessageId) {
    SinkCapture cap;
    Proxy proxy{cap.sink()};

    const std::string returned_id = proxy.accept({}, {}, "prod-1");

    ASSERT_EQ(cap.messages.size(), 1u);
    EXPECT_EQ(cap.messages[0].id, returned_id);
}

TEST(Proxy, AcceptStampsArrivalTime) {
    SinkCapture cap;
    Proxy proxy{cap.sink()};

    const auto before = std::chrono::steady_clock::now();
    proxy.accept({}, {}, "prod-1");
    const auto after = std::chrono::steady_clock::now();

    ASSERT_EQ(cap.messages.size(), 1u);
    EXPECT_GE(cap.messages[0].arrival_time, before);
    EXPECT_LE(cap.messages[0].arrival_time, after);
}

TEST(Proxy, AcceptForwardsPayload) {
    SinkCapture cap;
    Proxy proxy{cap.sink()};

    proxy.accept({0xDE, 0xAD}, {{"type", "job"}}, "p1");

    ASSERT_EQ(cap.messages.size(), 1u);
    EXPECT_EQ(cap.messages[0].payload, (std::vector<uint8_t>{0xDE, 0xAD}));
    EXPECT_EQ(cap.messages[0].headers.at("type"), "job");
}

TEST(Proxy, AcceptStoresProducerId) {
    SinkCapture cap;
    Proxy proxy{cap.sink()};

    proxy.accept({}, {}, "my-producer");

    ASSERT_EQ(cap.messages.size(), 1u);
    EXPECT_EQ(cap.messages[0].headers.at("__producer_id"), "my-producer");
}

TEST(Proxy, AcceptIncrementsCounter) {
    SinkCapture cap;
    Proxy proxy{cap.sink()};

    EXPECT_EQ(proxy.messages_accepted(), 0u);
    proxy.accept({}, {}, "p");
    proxy.accept({}, {}, "p");
    EXPECT_EQ(proxy.messages_accepted(), 2u);
}

TEST(Proxy, GenerateIdIsUnique) {
    constexpr int kCount = 10000;
    std::set<std::string> ids;
    for (int i = 0; i < kCount; ++i) {
        ids.insert(Proxy::generate_id());
    }
    EXPECT_EQ(static_cast<int>(ids.size()), kCount);
}

TEST(Proxy, AcceptLeavesTtlUnsetByDefault) {
    SinkCapture cap;
    Proxy proxy{cap.sink()};

    proxy.accept({}, {}, "p1");

    ASSERT_EQ(cap.messages.size(), 1u);
    EXPECT_EQ(cap.messages[0].ttl, kTtlUnset);
}

TEST(Proxy, AcceptForwardsExplicitTtl) {
    SinkCapture cap;
    Proxy proxy{cap.sink()};

    proxy.accept({}, {}, "p1", std::chrono::milliseconds{0});
    proxy.accept({}, {}, "p1", std::chrono::milliseconds{250});

    ASSERT_EQ(cap.messages.size(), 2u);
    EXPECT_EQ(cap.messages[0].ttl, std::chrono::milliseconds{0});
    EXPECT_EQ(cap.messages[1].ttl, std::chrono::milliseconds{250});
}
