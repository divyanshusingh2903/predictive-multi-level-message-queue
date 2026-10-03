#include "harbinger_service.hpp"
#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <future>

using namespace harbinger;
using namespace std::chrono_literals;

namespace harbinger {
struct BrokerTestAccess {
    static void expire_lease(HarbingerService& broker, const std::string& id, bool ttl) {
        std::lock_guard lock{broker.in_flight_mutex_};
        auto& entry = broker.in_flight_.at(id);
        broker.deadlines_.erase(entry.deadline);
        entry.deadline = broker.deadlines_.emplace(std::chrono::steady_clock::now() - 1ms, id);
        if (ttl) {
            entry.message.ttl = 1ms;
            entry.message.arrival_time = std::chrono::steady_clock::now() - 1s;
        }
    }
    static void maintain(HarbingerService& broker) { broker.maintain_deliveries(); }
    static void expire_queued(HarbingerService& broker) {
        auto message = *broker.queue_.try_dequeue();
        message.ttl = 1ms;
        message.arrival_time = std::chrono::steady_clock::now() - 1s;
        broker.queue_.requeue_front(std::move(message));
        broker.dlq_swept(broker.queue_.sweep_expired_batch(1), "benchmark test");
    }
};
}

namespace {
struct Fixture : testing::Test {
    std::shared_ptr<benchmark::Observations> sink = std::make_shared<benchmark::Observations>(100);
    HarbingerConfig config() {
        HarbingerConfig cfg;
        cfg.benchmark_options = benchmark::Options{benchmark::Policy::Static, {0, 1, 2}, sink};
        return cfg;
    }
    std::string producer(HarbingerService& broker) {
        grpc::ServerContext ctx; harbinger_rpc::RegisterProducerRequest req; harbinger_rpc::RegisterProducerResponse resp;
        EXPECT_TRUE(broker.RegisterProducer(&ctx, &req, &resp).ok()); return resp.producer_id();
    }
    std::string consumer(HarbingerService& broker) {
        grpc::ServerContext ctx; harbinger_rpc::RegisterConsumerRequest req; harbinger_rpc::RegisterConsumerResponse resp;
        EXPECT_TRUE(broker.RegisterConsumer(&ctx, &req, &resp).ok()); return resp.consumer_id();
    }
    void submit(HarbingerService& broker, const std::string& owner, int seq, int job) {
        grpc::ServerContext ctx; harbinger_rpc::SubmitRequest req; harbinger_rpc::SubmitResponse resp;
        req.set_producer_id(owner);
        (*req.mutable_headers())["job"] = "class" + std::to_string(job);
        (*req.mutable_headers())["__benchmark_sequence"] = std::to_string(seq);
        EXPECT_TRUE(broker.Submit(&ctx, &req, &resp).ok());
    }
    harbinger_rpc::PulledMessage pull(HarbingerService& broker, const std::string& owner) {
        grpc::ServerContext ctx; harbinger_rpc::PullRequest req; harbinger_rpc::PullResponse resp;
        req.set_consumer_id(owner); req.set_timeout_ms(10);
        EXPECT_TRUE(broker.Pull(&ctx, &req, &resp).ok()); EXPECT_FALSE(resp.timed_out()); return resp.message();
    }
};
TEST_F(Fixture, RealBrokerPoliciesKeepConsumersTierBlind) {
    for (const auto policy : {benchmark::Policy::Fifo, benchmark::Policy::Static, benchmark::Policy::RoundRobin}) {
        auto cfg = config(); cfg.benchmark_options->policy = policy;
        HarbingerService broker{cfg};
        const auto p = producer(broker), c = consumer(broker);
        submit(broker, p, 0, 2); submit(broker, p, 1, 0); submit(broker, p, 2, 0);
        const auto first = pull(broker, c), second = pull(broker, c);
        EXPECT_EQ(first.headers().at("__benchmark_sequence"), policy == benchmark::Policy::Fifo ? "0" : "1");
        EXPECT_EQ(second.headers().at("__benchmark_sequence"), policy == benchmark::Policy::Static ? "2" : policy == benchmark::Policy::Fifo ? "1" : "0");
    }
}
TEST_F(Fixture, AcceptedReplayAndRejectedSettlementEmitNoDuplicateOutcome) {
    HarbingerService broker{config()};
    const auto p = producer(broker), c = consumer(broker);
    submit(broker, p, 0, 1); auto message = pull(broker, c);
    harbinger_rpc::AckRequest req; harbinger_rpc::AckResponse resp;
    req.set_consumer_id(c); req.set_message_id(message.message_id()); req.set_attempt_token(message.attempt_token());
    { grpc::ServerContext ctx; ASSERT_TRUE(broker.Ack(&ctx, &req, &resp).ok()); }
    { grpc::ServerContext ctx; ASSERT_TRUE(broker.Ack(&ctx, &req, &resp).ok()); }
    req.set_consumer_id("wrong");
    { grpc::ServerContext ctx; EXPECT_EQ(broker.Ack(&ctx, &req, &resp).error_code(), grpc::StatusCode::PERMISSION_DENIED); }
    broker.benchmark_close_feedback();
    auto records = sink->snapshot(); ASSERT_EQ(records.size(), 3);
    EXPECT_EQ(records[1].kind, benchmark::Kind::Dispatch); EXPECT_EQ(records[1].attempt, 1);
    EXPECT_EQ(records[2].kind, benchmark::Kind::Ack);
}
TEST_F(Fixture, LeaseRetryTtlPrecedenceAndQueuedExpiryEmitActualOutcomesWithoutWriter) {
    auto cfg = config(); cfg.lease_sweep_interval = 1h; cfg.ttl_sweep_interval = 0ms;
    HarbingerService broker{cfg};
    const auto p = producer(broker), c = consumer(broker);
    submit(broker, p, 0, 2);
    const auto first = pull(broker, c);
    BrokerTestAccess::expire_lease(broker, first.message_id(), false);
    BrokerTestAccess::maintain(broker);
    const auto second = pull(broker, c);
    BrokerTestAccess::expire_lease(broker, second.message_id(), true);
    BrokerTestAccess::maintain(broker);
    submit(broker, p, 1, 0); BrokerTestAccess::expire_queued(broker);
    broker.benchmark_close_feedback();
    const auto records = sink->snapshot(); ASSERT_EQ(records.size(), 7);
    EXPECT_EQ(records[2].kind, benchmark::Kind::Retry); EXPECT_EQ(records[2].retries, 1);
    EXPECT_EQ(records[3].kind, benchmark::Kind::Dispatch); EXPECT_EQ(records[3].attempt, 2);
    EXPECT_EQ(records[4].kind, benchmark::Kind::Ttl); EXPECT_EQ(records[4].retries, 1);
    EXPECT_EQ(records[6].kind, benchmark::Kind::Ttl); EXPECT_EQ(records[6].attempt, 0);
}
TEST_F(Fixture, CancelledPullConsumesNoOrdinalOrObservation) {
    HarbingerService broker{config()};
    const auto p = producer(broker), c = consumer(broker);
    submit(broker, p, 0, 1);
    struct CancelledService : harbinger_rpc::Broker::Service {
        HarbingerService& broker;
        std::promise<grpc::StatusCode> result;
        explicit CancelledService(HarbingerService& b) : broker(b) {}
        grpc::Status Pull(grpc::ServerContext* ctx, const harbinger_rpc::PullRequest* req,
                          harbinger_rpc::PullResponse* resp) override {
            ctx->TryCancel();
            while (!ctx->IsCancelled()) std::this_thread::yield();
            auto status = broker.Pull(ctx, req, resp);
            result.set_value(status.error_code());
            return status;
        }
    } cancelled{broker};
    grpc::ServerBuilder builder; int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&cancelled);
    auto server = builder.BuildAndStart(); ASSERT_TRUE(server);
    auto stub = harbinger_rpc::Broker::NewStub(grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
    harbinger_rpc::PullRequest req; harbinger_rpc::PullResponse resp; req.set_consumer_id(c);
    grpc::ClientContext ctx; ctx.set_deadline(std::chrono::system_clock::now() + 5s);
    EXPECT_EQ(stub->Pull(&ctx, req, &resp).error_code(), grpc::StatusCode::CANCELLED);
    EXPECT_EQ(cancelled.result.get_future().get(), grpc::StatusCode::CANCELLED);
    server->Shutdown(); server->Wait();
    const auto msg = pull(broker, c); (void)msg;
    broker.benchmark_close_feedback();
    const auto records = sink->snapshot(); ASSERT_EQ(records.size(), 2);
    EXPECT_EQ(records.back().attempt, 1);
}
TEST(Observations, ConcurrentBoundedAdmissionReportsExactOverflow) {
    benchmark::Observations observations{10};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) threads.emplace_back([&] {
        for (int j = 0; j < 10; ++j) observations.publish({});
    });
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(observations.snapshot().size(), 10); EXPECT_EQ(observations.dropped(), 30);
}
} // namespace
