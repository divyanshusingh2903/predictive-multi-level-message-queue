#include "consumer/consumer.hpp"
#include "harbinger_service.hpp"
#include "producer/producer.hpp"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <future>
#include <barrier>

using namespace harbinger;
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

    HarbingerService                  service_;
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

TEST_F(ClientTest, BinaryPayloadsAndManyHeadersSurviveRetry) {
    auto producer = Producer::connect(addr_);
    for (const std::size_t size : {0u, 4u, 65536u}) {
        SCOPED_TRACE(size);
        std::vector<uint8_t> payload(size);
        for (std::size_t i = 0; i < size; ++i) payload[i] = static_cast<uint8_t>(i);
        std::unordered_map<std::string, std::string> headers;
        for (int i = 0; i < 128; ++i) headers["header-" + std::to_string(i)] = i % 2 ? "value" : "";
        std::atomic<int> calls{0};
        auto consumer = Consumer::connect(addr_, [&](const ReceivedMessage& msg) {
            EXPECT_EQ(msg.payload, payload);
            EXPECT_EQ(msg.headers.size(), headers.size() + 1);
            EXPECT_EQ(msg.headers.at("__producer_id"), producer->id());
            for (const auto& [key, value] : headers) EXPECT_EQ(msg.headers.at(key), value);
            return calls++ == 0 ? AckResult::FAILURE : AckResult::SUCCESS;
        }, 100ms);
        (void)producer->send(payload, headers);
        consumer->start();
        EXPECT_TRUE(wait_for([&] { return consumer->messages_acked() == 1; }));
        consumer->stop();
        EXPECT_EQ(calls, 2);
        EXPECT_EQ(consumer->messages_nacked(), 1u);
        EXPECT_EQ(consumer->leases_lost(), 0u);
    }
}

namespace {
class StalledRegistration final : public harbinger_rpc::Broker::Service {
public:
    std::promise<std::pair<std::chrono::system_clock::time_point,
                           std::chrono::system_clock::time_point>> entered;
    std::atomic<bool> release{false};
    std::atomic<bool> cancelled{false};
    grpc::Status RegisterProducer(grpc::ServerContext* ctx,
                                  const harbinger_rpc::RegisterProducerRequest*,
                                  harbinger_rpc::RegisterProducerResponse*) override {
        entered.set_value({ctx->deadline(), std::chrono::system_clock::now()});
        const auto watchdog = std::chrono::steady_clock::now() + 8s;
        while (!release && !ctx->IsCancelled() && std::chrono::steady_clock::now() < watchdog)
            std::this_thread::sleep_for(10ms);
        cancelled = ctx->IsCancelled();
        return {grpc::StatusCode::UNAVAILABLE, "registration deliberately stalled"};
    }
};
}

TEST(ProducerDeadline, StalledRegistrationUsesActualFiveSecondDeadline) {
    StalledRegistration service;
    auto entered = service.entered.get_future();
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_TRUE(server);
    struct Guard {
        StalledRegistration& service;
        grpc::Server& server;
        ~Guard() { service.release = true; server.Shutdown(); server.Wait(); }
    } guard{service, *server};
    const auto start = std::chrono::steady_clock::now();
    auto connect = std::async(std::launch::async, [&] {
        try {
            (void)Producer::connect("127.0.0.1:" + std::to_string(port));
            return false;
        } catch (const std::runtime_error&) {
            return true;
        }
    });
    const bool handler_entered = entered.wait_for(3s) == std::future_status::ready;
    EXPECT_TRUE(handler_entered);
    if (handler_entered) {
        const auto [deadline, arrival] = entered.get();
        EXPECT_NE(deadline, std::chrono::system_clock::time_point::max());
        EXPECT_GT(deadline - arrival, 4s);
        EXPECT_LT(deadline - arrival, 6s);
    }
    EXPECT_EQ(connect.wait_for(7s), std::future_status::ready);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto observe_until = std::chrono::steady_clock::now() + 1s;
    while (!service.cancelled && std::chrono::steady_clock::now() < observe_until)
        std::this_thread::sleep_for(10ms);
    EXPECT_TRUE(service.cancelled);
    service.release = true; // Always unblock before joining, including pre-fix failures.
    EXPECT_TRUE(connect.get());
    EXPECT_GE(elapsed, 4s);
    EXPECT_LT(elapsed, 8s);
}

// ── End-to-end: Producer → Harbinger → Consumer ──────────────────────────────────

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
    std::atomic<bool> captured{false};
    std::string captured_payload;
    std::string captured_type;

    auto producer = Producer::connect(addr_);
    auto consumer = Consumer::connect(addr_,
        [&](const ReceivedMessage& msg) {
            captured_payload = std::string(msg.payload.begin(), msg.payload.end());
            if (msg.headers.count("job_type")) {
                captured_type = msg.headers.at("job_type");
            }
            captured.store(true, std::memory_order_release);
            return AckResult::SUCCESS;
        }, 500ms);

    consumer->start();
    producer->send({'H','i'}, {{"job_type", "transcode"}});

    EXPECT_TRUE(wait_for([&] { return captured.load(std::memory_order_acquire); }));
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
    HarbingerService svc{ HarbingerConfig{ .default_max_retries = 1, .max_pull_wait = 500ms } };
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

TEST_F(ClientTest, DestructorStopsActivePollingAndRestartIsSafe) {
    {
        auto c = Consumer::connect(addr_, [](const ReceivedMessage&) { return AckResult::SUCCESS; }, 10s);
        for (int i = 0; i < 5; ++i) {
            c->start();
            c->stop();
        }
        c->start();
    }
}

TEST_F(ClientTest, ConcurrentStopAndHandlerStopHaveSingleJoinOwner) {
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    std::shared_ptr<Consumer> c;
    c = Consumer::connect(addr_, [&](const ReceivedMessage&) {
        entered.set_value();
        if (released.wait_for(5s) != std::future_status::ready)
            return AckResult::FAILURE;
        c->stop();
        return AckResult::SUCCESS;
    });
    auto p = Producer::connect(addr_);
    p->send({});
    c->start();
    ASSERT_EQ(entered.get_future().wait_for(3s), std::future_status::ready);
    auto a = std::async(std::launch::async, [&] { c->stop(); });
    auto b = std::async(std::launch::async, [&] { c->stop(); });
    EXPECT_TRUE(wait_for([&] { return !c->is_running(); }));
    EXPECT_THROW(c->start(), std::runtime_error);
    release.set_value();
    EXPECT_EQ(a.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(b.wait_for(3s), std::future_status::ready);
    a.get(); b.get();
    EXPECT_EQ(c->messages_acked(), 1u);
    c->start();
    c->stop();
}

namespace {
// Forward to a real broker, injecting transport errors before or after settlement.
class FaultBroker final : public harbinger_rpc::Broker::Service {
public:
    explicit FaultBroker(HarbingerConfig config = {}) : broker(config) {}
    HarbingerService broker;
    std::atomic<int> mode{0}; // 1 lost response, 2 before commit, 3 permanent, 4 unavailable
    std::atomic<int> settlement_calls{0};
    std::atomic<int> pull_error{0};
    std::atomic<int> pull_calls{0};
    std::atomic<int> settlement_error{0};
    std::atomic<bool> lose_after_transient{false};
    grpc::Status RegisterProducer(grpc::ServerContext* c, const harbinger_rpc::RegisterProducerRequest* r,
                                  harbinger_rpc::RegisterProducerResponse* s) override {
        return broker.RegisterProducer(c, r, s);
    }
    grpc::Status RegisterConsumer(grpc::ServerContext* c, const harbinger_rpc::RegisterConsumerRequest* r,
                                  harbinger_rpc::RegisterConsumerResponse* s) override {
        return broker.RegisterConsumer(c, r, s);
    }
    grpc::Status Submit(grpc::ServerContext* c, const harbinger_rpc::SubmitRequest* r,
                        harbinger_rpc::SubmitResponse* s) override { return broker.Submit(c, r, s); }
    grpc::Status Pull(grpc::ServerContext* c, const harbinger_rpc::PullRequest* r,
                      harbinger_rpc::PullResponse* s) override {
        ++pull_calls;
        const int error = pull_error.load();
        if (error) return {static_cast<grpc::StatusCode>(error), "injected Pull failure"};
        return broker.Pull(c, r, s);
    }
    template <typename Call> grpc::Status inject(Call call) {
        const int attempt = ++settlement_calls;
        const int error = settlement_error.load();
        if (error) return {static_cast<grpc::StatusCode>(error), "injected settlement failure"};
        if (lose_after_transient && attempt <= 2)
            return {attempt == 1 ? grpc::StatusCode::UNAVAILABLE
                                 : grpc::StatusCode::FAILED_PRECONDITION, "lost attempt"};
        const int fault = mode.load();
        if (fault == 3) return {grpc::StatusCode::PERMISSION_DENIED, "permanent"};
        if (fault == 4 || (fault == 2 && attempt == 1))
            return {grpc::StatusCode::UNAVAILABLE, "before commit"};
        const auto status = call();
        if (fault == 1 && attempt == 1 && status.ok())
            return {grpc::StatusCode::DEADLINE_EXCEEDED, "response lost after commit"};
        return status;
    }
    grpc::Status Ack(grpc::ServerContext* c, const harbinger_rpc::AckRequest* r,
                     harbinger_rpc::AckResponse* s) override {
        return inject([&] { return broker.Ack(c, r, s); });
    }
    grpc::Status Nack(grpc::ServerContext* c, const harbinger_rpc::NackRequest* r,
                      harbinger_rpc::NackResponse* s) override {
        return inject([&] { return broker.Nack(c, r, s); });
    }
};

class ClientRecovery : public ::testing::Test {
protected:
    explicit ClientRecovery(HarbingerConfig config = {}) : service(config) {}
    FaultBroker service;
    std::unique_ptr<grpc::Server> server;
    std::string addr;
    void SetUp() override {
        int port = 0;
        grpc::ServerBuilder builder;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service);
        server = builder.BuildAndStart();
        addr = "127.0.0.1:" + std::to_string(port);
    }
    void TearDown() override { server->Shutdown(); }
    template <typename Pred> bool wait_for(Pred pred) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!pred() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        return pred();
    }
};
}

TEST_F(ClientRecovery, LostAckResponseRetriesSameOutcomeWithoutReprocessing) {
    service.mode = 1;
    std::atomic<int> calls{0};
    auto c = Consumer::connect(addr, [&](const ReceivedMessage&) { ++calls; return AckResult::SUCCESS; });
    auto p = Producer::connect(addr);
    p->send({});
    c->start();
    EXPECT_TRUE(wait_for([&] { return c->messages_acked() == 1; }));
    c->stop();
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(service.settlement_calls, 2);
    EXPECT_EQ(c->rpc_failures(), 1u);
    EXPECT_TRUE(c->last_rpc_status().ok());
    EXPECT_EQ(service.broker.in_flight_count(), 0u);
}

TEST_F(ClientRecovery, LostNackResponseDoesNotNackNewAttempt) {
    service.mode = 1;
    std::atomic<int> calls{0};
    auto c = Consumer::connect(addr, [&](const ReceivedMessage&) {
        return calls++ == 0 ? AckResult::FAILURE : AckResult::SUCCESS;
    });
    auto p = Producer::connect(addr);
    p->send({});
    c->start();
    EXPECT_TRUE(wait_for([&] { return c->messages_acked() == 1; }));
    c->stop();
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(c->messages_nacked(), 1u);
    EXPECT_EQ(service.settlement_calls, 3);
    EXPECT_EQ(service.broker.dlq_size(), 0u);
}

TEST_F(ClientRecovery, BeforeCommitFailureRetriesAndPermanentFailureCanRestart) {
    service.mode = 2;
    auto c = Consumer::connect(addr, [](const ReceivedMessage&) { return AckResult::SUCCESS; });
    auto p = Producer::connect(addr);
    p->send({});
    c->start();
    EXPECT_TRUE(wait_for([&] { return c->messages_acked() == 1; }));
    service.mode = 3;
    p->send({});
    EXPECT_TRUE(wait_for([&] { return !c->is_running(); }));
    EXPECT_EQ(c->last_rpc_status().error_code(), grpc::StatusCode::PERMISSION_DENIED);
    const auto failures = c->rpc_failures();
    service.mode = 0;
    // No stop first: start must join the exited worker itself.
    c->start();
    p->send({});
    EXPECT_TRUE(wait_for([&] { return c->messages_acked() == 2; }));
    c->stop();
    EXPECT_EQ(c->rpc_failures(), failures);
    EXPECT_TRUE(c->last_rpc_status().ok());
}

TEST_F(ClientRecovery, ExhaustedSettlementRetriesStopWithObservableError) {
    service.mode = 4;
    auto c = Consumer::connect(addr, [](const ReceivedMessage&) { return AckResult::FAILURE; });
    auto p = Producer::connect(addr);
    p->send({});
    c->start();
    EXPECT_TRUE(wait_for([&] { return !c->is_running(); }));
    EXPECT_EQ(c->last_rpc_status().error_code(), grpc::StatusCode::UNAVAILABLE);
    EXPECT_EQ(service.settlement_calls, 3);
    EXPECT_EQ(c->rpc_failures(), 3u);
    EXPECT_EQ(c->messages_nacked(), 0u);
    EXPECT_EQ(c->leases_lost(), 0u);
    // Destructor also joins an already-exited thread.
}

namespace {
class ClientLeaseRecovery : public ClientRecovery,
                            public ::testing::WithParamInterface<AckResult> {
protected:
    ClientLeaseRecovery() : ClientRecovery([] {
        HarbingerConfig config;
        config.delivery_lease = 300ms;
        config.lease_sweep_interval = 10ms;
        config.ttl_sweep_interval = 0ms;
        return config;
    }()) {}
};
}

TEST_P(ClientLeaseRecovery, SlowHandlerLosesLeaseAndContinuesProcessing) {
    auto p = Producer::connect(addr);
    const auto slow_id = p->send({1});
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<int> slow_calls{0};
    std::atomic<bool> normal_seen{false};
    const auto first_result = GetParam();
    auto c = Consumer::connect(addr, [&](const ReceivedMessage& msg) {
        if (msg.id == slow_id && slow_calls++ == 0) {
            entered.set_value();
            // Bounded even if the test fails before releasing the handler.
            released.wait_for(3s);
            return first_result;
        }
        if (msg.id != slow_id) normal_seen = true;
        return AckResult::SUCCESS;
    }, 100ms);
    c->start();
    const bool handler_entered = entered_future.wait_for(3s) == std::future_status::ready;
    EXPECT_TRUE(handler_entered);
    if (handler_entered) {
        EXPECT_TRUE(wait_for([&] {
            return service.broker.in_flight_count() == 0 && service.broker.queue_size() == 1;
        }));
    }
    p->send({2});
    release.set_value();
    EXPECT_TRUE(wait_for([&] { return c->messages_acked() == 2; }));
    EXPECT_TRUE(c->is_running());
    c->stop();
    EXPECT_TRUE(normal_seen);
    EXPECT_EQ(slow_calls, 2);
    EXPECT_EQ(c->messages_processed(), 3u);
    EXPECT_EQ(c->messages_nacked(), 0u);
    EXPECT_EQ(c->leases_lost(), 1u);
    EXPECT_EQ(c->rpc_failures(), 1u);
    EXPECT_TRUE(c->last_rpc_status().ok());
    c->start();
    c->stop();
    EXPECT_EQ(c->leases_lost(), 1u);
}

INSTANTIATE_TEST_SUITE_P(AckAndNack, ClientLeaseRecovery,
                        ::testing::Values(AckResult::SUCCESS, AckResult::FAILURE));

TEST_F(ClientRecovery, PermanentSettlementErrorsRemainTerminalForAckAndNack) {
    auto p = Producer::connect(addr);
    for (const auto result : {AckResult::SUCCESS, AckResult::FAILURE}) {
        for (const auto error : {grpc::StatusCode::PERMISSION_DENIED,
                                 grpc::StatusCode::INVALID_ARGUMENT,
                                 grpc::StatusCode::NOT_FOUND}) {
            SCOPED_TRACE(static_cast<int>(result));
            SCOPED_TRACE(static_cast<int>(error));
            service.settlement_error = error;
            service.settlement_calls = 0;
            auto c = Consumer::connect(addr, [result](const ReceivedMessage&) { return result; });
            p->send({});
            c->start();
            EXPECT_TRUE(wait_for([&] { return !c->is_running(); }));
            c->stop();
            EXPECT_EQ(service.settlement_calls, 1);
            EXPECT_EQ(c->messages_processed(), 1u);
            EXPECT_EQ(c->messages_acked(), 0u);
            EXPECT_EQ(c->messages_nacked(), 0u);
            EXPECT_EQ(c->leases_lost(), 0u);
            EXPECT_EQ(c->rpc_failures(), 1u);
            EXPECT_EQ(c->last_rpc_status().error_code(), error);
        }
    }
}

TEST_F(ClientRecovery, TransientThenLostAttemptCountsOnceAndContinues) {
    service.lose_after_transient = true;
    auto p = Producer::connect(addr);
    std::atomic<int> calls{0};
    auto c = Consumer::connect(addr, [&](const ReceivedMessage&) {
        return calls++ == 0 ? AckResult::FAILURE : AckResult::SUCCESS;
    });
    p->send({});
    p->send({});
    c->start();
    EXPECT_TRUE(wait_for([&] { return c->messages_acked() == 1; }));
    EXPECT_TRUE(c->is_running());
    c->stop();
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(service.settlement_calls, 3);
    EXPECT_EQ(c->rpc_failures(), 2u);
    EXPECT_EQ(c->leases_lost(), 1u);
    EXPECT_EQ(c->messages_nacked(), 0u);
    EXPECT_TRUE(c->last_rpc_status().ok());
}

TEST_F(ClientRecovery, PullFailedPreconditionRemainsTerminal) {
    service.pull_error = grpc::StatusCode::FAILED_PRECONDITION;
    auto c = Consumer::connect(addr, [](const ReceivedMessage&) { return AckResult::SUCCESS; });
    c->start();
    EXPECT_TRUE(wait_for([&] { return !c->is_running(); }));
    c->stop();
    EXPECT_EQ(c->leases_lost(), 0u);
    EXPECT_EQ(c->last_rpc_status().error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(ClientRecovery, PullPermanentErrorsStopAndTransientBackoffIsInterruptible) {
    service.pull_error = grpc::StatusCode::PERMISSION_DENIED;
    auto c = Consumer::connect(addr, [](const ReceivedMessage&) { return AckResult::SUCCESS; });
    c->start();
    EXPECT_TRUE(wait_for([&] { return !c->is_running(); }));
    EXPECT_EQ(service.pull_calls, 1);
    EXPECT_EQ(c->last_rpc_status().error_code(), grpc::StatusCode::PERMISSION_DENIED);
    service.pull_error = grpc::StatusCode::UNAVAILABLE;
    c->start();
    EXPECT_TRUE(wait_for([&] { return service.pull_calls > 1; }));
    auto stop = std::async(std::launch::async, [&] { c->stop(); });
    EXPECT_EQ(stop.wait_for(500ms), std::future_status::ready);
    stop.get();
    service.pull_error = 0;
    auto p = Producer::connect(addr);
    p->send({});
    c->start();
    EXPECT_TRUE(wait_for([&] { return c->messages_acked() == 1; }));
    c->stop();
}
