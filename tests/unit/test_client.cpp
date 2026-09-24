#include "consumer/consumer.hpp"
#include "pmlmq_service.hpp"
#include "producer/producer.hpp"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace pmlmq;
using namespace std::chrono_literals;

// ── Shared fixture: starts a real in-process gRPC server on port 0 ────────────

class ClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        int port = 0;
        grpc::ServerBuilder builder;
        builder.AddListeningPort("127.0.0.1:0",
                                 grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service_);
        server_ = builder.BuildAndStart();
        addr_   = "127.0.0.1:" + std::to_string(port);
    }

    void TearDown() override {
        server_->Shutdown();
    }

    template <typename Pred>
    static bool wait_for(Pred pred, std::chrono::milliseconds timeout = 3s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!pred()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(10ms);
        }
        return true;
    }

    PMLMQService                  service_;
    std::unique_ptr<grpc::Server> server_;
    std::string                   addr_;
};

// ── Producer client ───────────────────────────────────────────────────────────

TEST_F(ClientTest, ProducerConnectAssignsId) {
    auto p = Producer::connect(addr_);
    EXPECT_FALSE(p->id().empty());
    EXPECT_EQ(p->messages_sent(), 0u);
}

TEST_F(ClientTest, TwoProducersGetDistinctIds) {
    auto p1 = Producer::connect(addr_);
    auto p2 = Producer::connect(addr_);
    EXPECT_NE(p1->id(), p2->id());
}

TEST_F(ClientTest, ProducerSendIncrementsCounter) {
    auto p = Producer::connect(addr_);
    p->send({0x01});
    p->send({0x02});
    EXPECT_EQ(p->messages_sent(), 2u);
}

TEST_F(ClientTest, ProducerSendReturnsMessageId) {
    auto p = Producer::connect(addr_);
    const auto id = p->send({0xAB}, {{"job", "resize"}});
    EXPECT_FALSE(id.empty());
}

TEST_F(ClientTest, ProducerSendIncreasesQueueSize) {
    auto p = Producer::connect(addr_);
    EXPECT_EQ(service_.queue_size(), 0u);
    p->send({});
    EXPECT_EQ(service_.queue_size(), 1u);
}

TEST_F(ClientTest, ProducerSendNegativeTtlThrows) {
    auto p = Producer::connect(addr_);
    EXPECT_THROW(p->send({0x01}, {}, std::chrono::milliseconds{-5}),
                 std::invalid_argument);
    EXPECT_EQ(service_.queue_size(), 0u);
    EXPECT_EQ(p->messages_sent(), 0u);
}

// ── Consumer client ───────────────────────────────────────────────────────────

TEST_F(ClientTest, ConsumerConnectAssignsId) {
    auto c = Consumer::connect(addr_, [](const ReceivedMessage&) {
        return AckResult::SUCCESS;
    });
    EXPECT_FALSE(c->id().empty());
    EXPECT_FALSE(c->is_running());
}

TEST_F(ClientTest, TwoConsumersGetDistinctIds) {
    auto handler = [](const ReceivedMessage&) { return AckResult::SUCCESS; };
    auto c1 = Consumer::connect(addr_, handler);
    auto c2 = Consumer::connect(addr_, handler);
    EXPECT_NE(c1->id(), c2->id());
}

TEST_F(ClientTest, ConsumerStartStop) {
    auto c = Consumer::connect(addr_, [](const ReceivedMessage&) {
        return AckResult::SUCCESS;
    }, 100ms);
    EXPECT_FALSE(c->is_running());
    c->start();
    EXPECT_TRUE(c->is_running());
    c->stop();
    EXPECT_FALSE(c->is_running());
}

// ── End-to-end: Producer → PMLMQ → Consumer ──────────────────────────────────

TEST_F(ClientTest, EndToEndAck) {
    std::atomic<int> processed{0};

    auto producer  = Producer::connect(addr_);
    auto consumer  = Consumer::connect(addr_,
        [&](const ReceivedMessage&) {
            processed.fetch_add(1);
            return AckResult::SUCCESS;
        }, 500ms);

    consumer->start();
    constexpr int kMessages = 10;
    for (int i = 0; i < kMessages; ++i) {
        producer->send({static_cast<uint8_t>(i)});
    }

    EXPECT_TRUE(wait_for([&] { return processed.load() == kMessages; }));
    consumer->stop();

    EXPECT_EQ(consumer->messages_acked(), static_cast<uint64_t>(kMessages));
    EXPECT_EQ(consumer->messages_nacked(), 0u);
    EXPECT_EQ(service_.dlq_size(), 0u);
}

TEST_F(ClientTest, ConsumerReceivesPayloadAndHeaders) {
    std::string captured_payload;
    std::string captured_type;

    auto producer = Producer::connect(addr_);
    auto consumer = Consumer::connect(addr_,
        [&](const ReceivedMessage& msg) {
            captured_payload = std::string(msg.payload.begin(), msg.payload.end());
            if (msg.headers.count("job_type")) {
                captured_type = msg.headers.at("job_type");
            }
            return AckResult::SUCCESS;
        }, 500ms);

    consumer->start();
    producer->send({'H','i'}, {{"job_type", "transcode"}});

    EXPECT_TRUE(wait_for([&] { return !captured_type.empty(); }));
    consumer->stop();

    EXPECT_EQ(captured_payload, "Hi");
    EXPECT_EQ(captured_type,    "transcode");
}

TEST_F(ClientTest, NackCausesRetryThenAck) {
    std::atomic<int> attempts{0};

    auto producer = Producer::connect(addr_);
    // max_retries defaults to 3 — fail twice, succeed on third.
    auto consumer = Consumer::connect(addr_,
        [&](const ReceivedMessage&) {
            return (attempts.fetch_add(1) < 2) ? AckResult::FAILURE
                                               : AckResult::SUCCESS;
        }, 500ms);

    consumer->start();
    producer->send({0x01});

    EXPECT_TRUE(wait_for([&] { return attempts.load() == 3; }));
    consumer->stop();

    EXPECT_EQ(consumer->messages_acked(), 1u);
    EXPECT_EQ(consumer->messages_nacked(), 2u);
    EXPECT_EQ(service_.dlq_size(), 0u);
}

TEST_F(ClientTest, HandlerExceptionIsNackedAndRetried) {
    std::atomic<int> attempts{0};

    auto producer = Producer::connect(addr_);
    auto consumer = Consumer::connect(addr_, [&](const ReceivedMessage&) {
        if (attempts.fetch_add(1) == 0) {
            throw std::runtime_error("transient handler failure");
        }
        return AckResult::SUCCESS;
    }, 500ms);

    consumer->start();
    producer->send({0x02});

    EXPECT_TRUE(wait_for([&] { return attempts.load() == 2; }));
    consumer->stop();

    EXPECT_EQ(consumer->messages_acked(), 1u);
    EXPECT_EQ(consumer->messages_nacked(), 1u);
    EXPECT_EQ(service_.dlq_size(), 0u);
}

TEST_F(ClientTest, NackExceedingMaxRetriesSendsToDLQ) {
    // Spin up a service with max_retries=1 for this test.
    PMLMQService svc{ PMLMQConfig{ .default_max_retries = 1, .max_pull_wait = 500ms } };
    int port = 0;
    grpc::ServerBuilder b;
    b.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    b.RegisterService(&svc);
    auto srv = b.BuildAndStart();
    const std::string a = "127.0.0.1:" + std::to_string(port);

    auto producer = Producer::connect(a);
    auto consumer = Consumer::connect(a,
        [](const ReceivedMessage&) { return AckResult::FAILURE; }, 500ms);

    consumer->start();
    producer->send({0xBB});

    EXPECT_TRUE(wait_for([&] { return svc.dlq_size() == 1u; }));
    consumer->stop();
    srv->Shutdown();
}

TEST_F(ClientTest, TwoConsumersCompeteForMessages) {
    std::atomic<int> c1_count{0}, c2_count{0};

    auto producer = Producer::connect(addr_);
    auto c1 = Consumer::connect(addr_,
        [&](const ReceivedMessage&) { c1_count++; return AckResult::SUCCESS; }, 200ms);
    auto c2 = Consumer::connect(addr_,
        [&](const ReceivedMessage&) { c2_count++; return AckResult::SUCCESS; }, 200ms);

    constexpr int kMessages = 20;
    for (int i = 0; i < kMessages; ++i) producer->send({static_cast<uint8_t>(i)});

    c1->start();
    c2->start();

    EXPECT_TRUE(wait_for([&] { return (c1_count + c2_count) == kMessages; }));
    c1->stop();
    c2->stop();

    // Each message consumed exactly once.
    EXPECT_EQ(c1_count.load() + c2_count.load(), kMessages);
    EXPECT_EQ(service_.queue_size(), 0u);
}
