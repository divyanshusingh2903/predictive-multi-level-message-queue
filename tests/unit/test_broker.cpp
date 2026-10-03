#include "harbinger_service.hpp"
#include "feedback_test_support.hpp"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <chrono>
#include <atomic>
#include <optional>
#include <thread>
#include <barrier>
#include <future>
#include <limits>
#include <set>

using namespace harbinger;
using namespace std::chrono_literals;

namespace harbinger {
struct QueueTestAccess {
    static void expire_selected(MultiLevelQueue& queue) {
        std::lock_guard lock{queue.mutex_};
        for (auto& level : queue.queues_) {
            if (level.empty()) continue;
            // Emulate crossing TTL after the index sweep, before selected-message validation.
            level.front().message.ttl = 1ms;
            level.front().message.arrival_time = std::chrono::steady_clock::now() - 1s;
            return;
        }
    }
};
struct BrokerTestAccess {
    static void expire_selected(HarbingerService& service) { QueueTestAccess::expire_selected(service.queue_); }
    static void stop_maintenance(HarbingerService& service) {
        {
            std::lock_guard lock{service.sweeper_mutex_};
            service.stop_sweeper_.store(true, std::memory_order_release);
        }
        service.sweeper_cv_.notify_all();
        if (service.sweeper_thread_.joinable()) service.sweeper_thread_.join();
    }
    static void close_feedback(HarbingerService& service) {
        stop_maintenance(service);
        service.feedback_writer_->close();
    }
    static ml::FeedbackWriter& writer(HarbingerService& service) { return *service.feedback_writer_; }
    static bool prime_ingress(HarbingerService& service) {
        auto event = feedback_test::event();
        event.trigger = ml::FeedbackTrigger::Submit;
        event.attempt_id.reset(); event.operation.reset(); event.outcome.reset();
        event.processing_time_ms.reset();
        return feedback_test::admit(writer(service), event);
    }
    static void exhaust_delivery_count(HarbingerService& service) {
        auto message = service.queue_.try_dequeue();
        ASSERT_TRUE(message);
        message->delivery_count = std::numeric_limits<uint64_t>::max();
        service.queue_.requeue_front(std::move(*message));
    }
    static void replace_writer(HarbingerService& service, std::shared_ptr<feedback_test::Storage> storage) {
        service.feedback_writer_.reset();
        service.feedback_writer_ = ml::FeedbackWriterTestAccess::create(
            *service.config_.feedback, service.config_.delivery_lease.count(), std::move(storage));
    }
    static std::unique_lock<std::mutex> gate_delivery(HarbingerService& service) {
        return std::unique_lock{service.in_flight_mutex_};
    }
    static std::optional<Message> queued_front(HarbingerService& service) {
        auto message = service.queue_.try_dequeue();
        if (message) service.queue_.requeue_front(*message);
        return message;
    }
    static std::shared_ptr<const ml::RoutingContext> in_flight_context(
        HarbingerService& service, const std::string& id) {
        std::lock_guard lock{service.in_flight_mutex_};
        return service.in_flight_.at(id).message.routing_context;
    }
    static void expire_ttl(HarbingerService& service, const std::string& id) {
        std::lock_guard lock{service.in_flight_mutex_};
        auto& message = service.in_flight_.at(id).message;
        message.ttl = 1ms;
        message.arrival_time = HarbingerService::Clock::now() - 1s;
    }
    static void expire_queued(HarbingerService& service) {
        auto message = service.queue_.try_dequeue();
        ASSERT_TRUE(message);
        message->ttl = 1ms;
        message->arrival_time = HarbingerService::Clock::now() - 1s;
        service.queue_.requeue_front(std::move(*message));
        service.dlq_swept(service.queue_.sweep_expired_batch(256), "test queued expiry");
    }
    static void expire(HarbingerService& service, const std::string& id, bool ttl = false) {
        std::lock_guard lock{service.in_flight_mutex_};
        auto& entry = service.in_flight_.at(id);
        service.deadlines_.erase(entry.deadline);
        entry.deadline = service.deadlines_.emplace(
            HarbingerService::Clock::now() - 1s, id);
        if (ttl) {
            entry.message.ttl = 1ms;
            entry.message.arrival_time = HarbingerService::Clock::now() - 1s;
        }
    }
    static void maintain(HarbingerService& service) { service.maintain_deliveries(); }
    static void expire_history(HarbingerService& service) {
        std::lock_guard lock{service.in_flight_mutex_};
        for (auto& c : service.completions_) c.time -= service.config_.completion_retention;
    }
    static void check_indexes(HarbingerService& service, std::size_t expected_history) {
        std::lock_guard lock{service.in_flight_mutex_};
        EXPECT_EQ(service.in_flight_.size(), service.deadlines_.size());
        EXPECT_EQ(service.completions_.size(), expected_history);
        EXPECT_EQ(service.completed_.size(), expected_history);
    }
};
}

TEST(ConfigValidationTest, RejectsInvalidServiceConfiguration) {
    EXPECT_THROW(
        (HarbingerService{HarbingerConfig{.num_levels = 0}}),
        std::invalid_argument);
    EXPECT_THROW(
        (HarbingerService{HarbingerConfig{.default_max_retries = 0}}),
        std::invalid_argument);
    EXPECT_THROW(
        (HarbingerService{HarbingerConfig{.default_ttl = -1ms}}),
        std::invalid_argument);
    EXPECT_THROW(
        (HarbingerService{HarbingerConfig{.max_pull_wait = 0ms}}),
        std::invalid_argument);
}

TEST(ConfigValidationTest, RejectsInvalidAgingConfiguration) {
    EXPECT_THROW(
        (HarbingerService{HarbingerConfig{.aging = AgingConfig{0ms, 100ms}}}),
        std::invalid_argument);
    EXPECT_THROW(
        (HarbingerService{HarbingerConfig{.aging = AgingConfig{100ms, 0ms}}}),
        std::invalid_argument);
}

// ── Test fixture ──────────────────────────────────────────────────────────────

class BrokerTest : public ::testing::Test {
protected:
    void SetUp() override {
        int port = 0;
        grpc::ServerBuilder builder;
        builder.AddListeningPort("127.0.0.1:0",
                                 grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service_);
        server_ = builder.BuildAndStart();

        const std::string addr = "127.0.0.1:" + std::to_string(port);
        channel_ = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
        stub_    = harbinger_rpc::Broker::NewStub(channel_);
    }

    void TearDown() override {
        server_->Shutdown();
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    std::string register_producer() {
        harbinger_rpc::RegisterProducerRequest  req;
        harbinger_rpc::RegisterProducerResponse resp;
        grpc::ClientContext ctx;
        EXPECT_TRUE(stub_->RegisterProducer(&ctx, req, &resp).ok());
        return resp.producer_id();
    }

    std::string register_consumer() {
        harbinger_rpc::RegisterConsumerRequest  req;
        harbinger_rpc::RegisterConsumerResponse resp;
        grpc::ClientContext ctx;
        EXPECT_TRUE(stub_->RegisterConsumer(&ctx, req, &resp).ok());
        return resp.consumer_id();
    }

    std::string submit(const std::string& producer_id,
                       const std::string& payload = "hello",
                       std::unordered_map<std::string,std::string> headers = {}) {
        harbinger_rpc::SubmitRequest req;
        req.set_producer_id(producer_id);
        req.set_payload(payload);
        for (const auto& [k, v] : headers) (*req.mutable_headers())[k] = v;

        harbinger_rpc::SubmitResponse resp;
        grpc::ClientContext ctx;
        EXPECT_TRUE(stub_->Submit(&ctx, req, &resp).ok());
        return resp.message_id();
    }

    std::string submit_with_ttl(const std::string& producer_id,
                                int64_t ttl_ms,
                                const std::string& payload = "hello") {
        harbinger_rpc::SubmitRequest req;
        req.set_producer_id(producer_id);
        req.set_payload(payload);
        req.set_ttl_ms(ttl_ms);

        harbinger_rpc::SubmitResponse resp;
        grpc::ClientContext ctx;
        EXPECT_TRUE(stub_->Submit(&ctx, req, &resp).ok());
        return resp.message_id();
    }

    harbinger_rpc::PullResponse pull(const std::string& consumer_id,
                                  int64_t timeout_ms = 500) {
        harbinger_rpc::PullRequest req;
        req.set_consumer_id(consumer_id);
        req.set_timeout_ms(timeout_ms);

        harbinger_rpc::PullResponse resp;
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + 3s);
        EXPECT_TRUE(stub_->Pull(&ctx, req, &resp).ok());
        return resp;
    }

    HarbingerService                             service_;
    std::unique_ptr<grpc::Server>            server_;
    std::shared_ptr<grpc::Channel>           channel_;
    std::unique_ptr<harbinger_rpc::Broker::Stub> stub_;
};

// ── Registration ──────────────────────────────────────────────────────────────

TEST_F(BrokerTest, RegisterProducerReturnsUniqueId) {
    const auto id1 = register_producer();
    const auto id2 = register_producer();
    EXPECT_FALSE(id1.empty());
    EXPECT_FALSE(id2.empty());
    EXPECT_NE(id1, id2);
}

TEST_F(BrokerTest, RegisterConsumerReturnsUniqueId) {
    const auto id1 = register_consumer();
    const auto id2 = register_consumer();
    EXPECT_NE(id1, id2);
}

// ── Submit ────────────────────────────────────────────────────────────────────

TEST_F(BrokerTest, SubmitWithUnknownProducerReturnsPermissionDenied) {
    harbinger_rpc::SubmitRequest req;
    req.set_producer_id("not-registered");
    req.set_payload("data");

    harbinger_rpc::SubmitResponse resp;
    grpc::ClientContext ctx;
    const auto status = stub_->Submit(&ctx, req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
}

TEST_F(BrokerTest, SubmitReturnsMessageId) {
    const auto prod = register_producer();
    const auto msg_id = submit(prod, "payload");
    EXPECT_FALSE(msg_id.empty());
}

TEST_F(BrokerTest, SubmitIncreasesQueueSize) {
    const auto prod = register_producer();
    EXPECT_EQ(service_.queue_size(), 0u);
    submit(prod);
    EXPECT_EQ(service_.queue_size(), 1u);
}

// ── Pull ──────────────────────────────────────────────────────────────────────

TEST_F(BrokerTest, PullWithUnknownConsumerReturnsPermissionDenied) {
    harbinger_rpc::PullRequest req;
    req.set_consumer_id("ghost");
    req.set_timeout_ms(100);

    harbinger_rpc::PullResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + 2s);
    const auto status = stub_->Pull(&ctx, req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
}

TEST_F(BrokerTest, PullTimesOutWhenQueueIsEmpty) {
    const auto cons = register_consumer();
    const auto resp = pull(cons, 100 /*ms*/);
    EXPECT_TRUE(resp.timed_out());
}

TEST_F(BrokerTest, PullDeliversSubmittedMessage) {
    const auto prod = register_producer();
    const auto cons = register_consumer();

    submit(prod, "hello-world", {{"type", "test"}});

    const auto resp = pull(cons, 1000);
    ASSERT_FALSE(resp.timed_out());
    EXPECT_EQ(resp.message().payload(), "hello-world");
    EXPECT_EQ(resp.message().headers().at("type"), "test");
}

TEST_F(BrokerTest, PullMovesMessageToInFlight) {
    const auto prod = register_producer();
    const auto cons = register_consumer();
    submit(prod);

    EXPECT_EQ(service_.in_flight_count(), 0u);
    pull(cons, 500);
    EXPECT_EQ(service_.in_flight_count(), 1u);
    EXPECT_EQ(service_.queue_size(), 0u);
}

// ── Ack ───────────────────────────────────────────────────────────────────────

TEST_F(BrokerTest, AckRemovesFromInFlight) {
    const auto prod = register_producer();
    const auto cons = register_consumer();
    submit(prod);
    const auto pull_resp = pull(cons, 500);
    ASSERT_FALSE(pull_resp.timed_out());

    harbinger_rpc::AckRequest ack;
    ack.set_consumer_id(cons);
    ack.set_message_id(pull_resp.message().message_id());
    ack.set_attempt_token(pull_resp.message().attempt_token());
    ack.set_processing_time_ms(5);

    harbinger_rpc::AckResponse ack_resp;
    grpc::ClientContext ctx;
    EXPECT_TRUE(stub_->Ack(&ctx, ack, &ack_resp).ok());
    EXPECT_EQ(service_.in_flight_count(), 0u);
    EXPECT_EQ(service_.dlq_size(), 0u);
}

TEST_F(BrokerTest, AckForUnknownMessageReturnsNotFound) {
    const auto cons = register_consumer();

    harbinger_rpc::AckRequest ack;
    ack.set_consumer_id(cons);
    ack.set_message_id("nonexistent");
    ack.set_attempt_token("unknown-token");

    harbinger_rpc::AckResponse ack_resp;
    grpc::ClientContext ctx;
    EXPECT_EQ(stub_->Ack(&ctx, ack, &ack_resp).error_code(),
              grpc::StatusCode::NOT_FOUND);
}

TEST_F(BrokerTest, AckByDifferentConsumerIsDeniedWithoutReleasingMessage) {
    const auto prod = register_producer();
    const auto owner = register_consumer();
    const auto other = register_consumer();
    submit(prod);
    const auto pulled = pull(owner, 500);
    ASSERT_FALSE(pulled.timed_out());

    harbinger_rpc::AckRequest ack;
    ack.set_consumer_id(other);
    ack.set_message_id(pulled.message().message_id());
    ack.set_attempt_token(pulled.message().attempt_token());
    harbinger_rpc::AckResponse ack_resp;
    grpc::ClientContext ctx;
    EXPECT_EQ(stub_->Ack(&ctx, ack, &ack_resp).error_code(),
              grpc::StatusCode::PERMISSION_DENIED);
    EXPECT_EQ(service_.in_flight_count(), 1u);

    ack.set_consumer_id(owner);
    grpc::ClientContext owner_ctx;
    EXPECT_TRUE(stub_->Ack(&owner_ctx, ack, &ack_resp).ok());
    EXPECT_EQ(service_.in_flight_count(), 0u);
}

// ── Nack ──────────────────────────────────────────────────────────────────────

TEST_F(BrokerTest, NackRequeuesMessage) {
    const auto prod = register_producer();
    const auto cons = register_consumer();
    submit(prod);
    const auto pull_resp = pull(cons, 500);
    ASSERT_FALSE(pull_resp.timed_out());

    harbinger_rpc::NackRequest nack;
    nack.set_consumer_id(cons);
    nack.set_message_id(pull_resp.message().message_id());
    nack.set_attempt_token(pull_resp.message().attempt_token());
    nack.set_processing_time_ms(10);
    nack.set_reason("test_nack");

    harbinger_rpc::NackResponse nack_resp;
    grpc::ClientContext ctx;
    EXPECT_TRUE(stub_->Nack(&ctx, nack, &nack_resp).ok());

    EXPECT_EQ(service_.in_flight_count(), 0u);
    EXPECT_EQ(service_.queue_size(), 1u); // re-queued
}

TEST_F(BrokerTest, NackByDifferentConsumerIsDeniedWithoutRequeuingMessage) {
    const auto prod = register_producer();
    const auto owner = register_consumer();
    const auto other = register_consumer();
    submit(prod);
    const auto pulled = pull(owner, 500);
    ASSERT_FALSE(pulled.timed_out());

    harbinger_rpc::NackRequest nack;
    nack.set_consumer_id(other);
    nack.set_message_id(pulled.message().message_id());
    nack.set_attempt_token(pulled.message().attempt_token());
    nack.set_reason("not the owner");
    harbinger_rpc::NackResponse nack_resp;
    grpc::ClientContext ctx;
    EXPECT_EQ(stub_->Nack(&ctx, nack, &nack_resp).error_code(),
              grpc::StatusCode::PERMISSION_DENIED);
    EXPECT_EQ(service_.in_flight_count(), 1u);
    EXPECT_EQ(service_.queue_size(), 0u);

    nack.set_consumer_id(owner);
    grpc::ClientContext owner_ctx;
    EXPECT_TRUE(stub_->Nack(&owner_ctx, nack, &nack_resp).ok());
    EXPECT_EQ(service_.in_flight_count(), 0u);
    EXPECT_EQ(service_.queue_size(), 1u);
}

TEST_F(BrokerTest, NackExceedingMaxRetriesSendsToDLQ) {
    // Use a config where max_retries=1 so one nack sends to DLQ.
    HarbingerService svc{ HarbingerConfig{ .default_max_retries = 1 } };
    int port = 0;
    grpc::ServerBuilder b;
    b.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    b.RegisterService(&svc);
    auto srv = b.BuildAndStart();
    auto ch  = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                   grpc::InsecureChannelCredentials());
    auto st  = harbinger_rpc::Broker::NewStub(ch);

    // Register, submit, pull.
    { grpc::ClientContext c; harbinger_rpc::RegisterProducerRequest rq; harbinger_rpc::RegisterProducerResponse rs; st->RegisterProducer(&c, rq, &rs); auto pid = rs.producer_id();
      grpc::ClientContext c2; harbinger_rpc::SubmitRequest sq; sq.set_producer_id(pid); sq.set_payload("x"); harbinger_rpc::SubmitResponse ss; st->Submit(&c2, sq, &ss); }

    grpc::ClientContext c3; harbinger_rpc::RegisterConsumerRequest rq2; harbinger_rpc::RegisterConsumerResponse rs2; st->RegisterConsumer(&c3, rq2, &rs2); auto cid = rs2.consumer_id();

    harbinger_rpc::PullRequest pr; pr.set_consumer_id(cid); pr.set_timeout_ms(500);
    harbinger_rpc::PullResponse presp; grpc::ClientContext c4;
    c4.set_deadline(std::chrono::system_clock::now() + 2s);
    st->Pull(&c4, pr, &presp);
    ASSERT_FALSE(presp.timed_out());

    // Nack once → retry_count becomes 1 ≥ max_retries=1 → DLQ.
    harbinger_rpc::NackRequest nq; nq.set_consumer_id(cid);
    nq.set_message_id(presp.message().message_id());
    nq.set_attempt_token(presp.message().attempt_token());
    harbinger_rpc::NackResponse nr; grpc::ClientContext c5;
    st->Nack(&c5, nq, &nr);

    EXPECT_EQ(svc.dlq_size(), 1u);
    srv->Shutdown();
}

namespace {

ml::IngressFeatureConfig capture_features() {
    return {
        .schema = {.version = "features-v1-job-units", .headers = {
            {.name = "job", .type = ml::FeatureType::Categorical, .vocabulary = {"binary", "resize"}},
            {.name = "units", .minimum = 0, .maximum = 1000000},
        }},
        .routing_policy_version = "static-v1-three-levels-priority-1",
    };
}

// Spin up a broker with a custom config on an ephemeral port.
// Returns the server (must outlive the test) and fills stub + ids.
struct LocalBroker {
    HarbingerService service;
    std::unique_ptr<grpc::Server> server;
    std::unique_ptr<harbinger_rpc::Broker::Stub> stub;
    std::string producer_id;
    std::string consumer_id;
    ~LocalBroker() { if (server) server->Shutdown(); }

    explicit LocalBroker(HarbingerConfig config) : service(std::move(config)) {
        int port = 0;
        grpc::ServerBuilder b;
        b.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        b.RegisterService(&service);
        server = b.BuildAndStart();
        auto ch = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                      grpc::InsecureChannelCredentials());
        stub = harbinger_rpc::Broker::NewStub(ch);

        grpc::ClientContext c1;
        harbinger_rpc::RegisterProducerRequest rq1;
        harbinger_rpc::RegisterProducerResponse rs1;
        EXPECT_TRUE(stub->RegisterProducer(&c1, rq1, &rs1).ok());
        producer_id = rs1.producer_id();

        grpc::ClientContext c2;
        harbinger_rpc::RegisterConsumerRequest rq2;
        harbinger_rpc::RegisterConsumerResponse rs2;
        EXPECT_TRUE(stub->RegisterConsumer(&c2, rq2, &rs2).ok());
        consumer_id = rs2.consumer_id();
    }

    std::string submit(const std::string& payload = "x",
                       std::optional<int64_t> ttl_ms = std::nullopt,
                       const std::unordered_map<std::string, std::string>& headers = {}) {
        harbinger_rpc::SubmitRequest sq;
        sq.set_producer_id(producer_id);
        sq.set_payload(payload);
        for (const auto& [key, value] : headers) (*sq.mutable_headers())[key] = value;
        if (ttl_ms) sq.set_ttl_ms(*ttl_ms);
        harbinger_rpc::SubmitResponse ss;
        grpc::ClientContext c;
        EXPECT_TRUE(stub->Submit(&c, sq, &ss).ok());
        return ss.message_id();
    }

    harbinger_rpc::PullResponse pull(int64_t timeout_ms = 1000) {
        harbinger_rpc::PullRequest pr;
        pr.set_consumer_id(consumer_id);
        pr.set_timeout_ms(timeout_ms);
        harbinger_rpc::PullResponse presp;
        grpc::ClientContext c;
        c.set_deadline(std::chrono::system_clock::now() + 3s);
        EXPECT_TRUE(stub->Pull(&c, pr, &presp).ok());
        return presp;
    }

    grpc::Status ack(const harbinger_rpc::PulledMessage& delivery, int64_t duration = 5) {
        harbinger_rpc::AckRequest aq;
        aq.set_consumer_id(consumer_id);
        aq.set_message_id(delivery.message_id());
        aq.set_attempt_token(delivery.attempt_token());
        aq.set_processing_time_ms(duration);
        harbinger_rpc::AckResponse aresp;
        grpc::ClientContext c;
        c.set_deadline(std::chrono::system_clock::now() + 3s);
        return stub->Ack(&c, aq, &aresp);
    }

    grpc::Status nack(const harbinger_rpc::PulledMessage& delivery, const std::string& reason = "t", int64_t duration = 5) {
        harbinger_rpc::NackRequest nq;
        nq.set_consumer_id(consumer_id);
        nq.set_message_id(delivery.message_id());
        nq.set_attempt_token(delivery.attempt_token());
        nq.set_processing_time_ms(duration);
        nq.set_reason(reason);
        harbinger_rpc::NackResponse nresp;
        grpc::ClientContext c;
        c.set_deadline(std::chrono::system_clock::now() + 3s);
        return stub->Nack(&c, nq, &nresp);
    }

    harbinger_rpc::InspectDlqResponse inspect(int32_t limit = 0,
                                          int32_t offset = 0) {
        harbinger_rpc::InspectDlqRequest rq;
        rq.set_limit(limit);
        rq.set_offset(offset);
        harbinger_rpc::InspectDlqResponse rs;
        grpc::ClientContext c;
        EXPECT_TRUE(stub->InspectDlq(&c, rq, &rs).ok());
        return rs;
    }
};

} // namespace

namespace {
// Keep context inspection synchronized with the forwarding handler's lifetime.
class ObservedPull final : public harbinger_rpc::Broker::Service {
public:
    explicit ObservedPull(HarbingerService& broker) : broker_(broker) {}
    std::promise<grpc::Status> finished;
    std::atomic<bool> cancelled_response_empty{false};
    bool cancelled() {
        std::lock_guard lock{mutex_};
        return context_ && context_->IsCancelled();
    }
    grpc::Status Pull(grpc::ServerContext* ctx, const harbinger_rpc::PullRequest* req,
                      harbinger_rpc::PullResponse* resp) override {
        {
            std::lock_guard lock{mutex_};
            context_ = ctx;
        }
        const auto status = broker_.Pull(ctx, req, resp);
        if (status.error_code() == grpc::StatusCode::CANCELLED)
            cancelled_response_empty = !resp->has_message();
        {
            std::lock_guard lock{mutex_};
            context_ = nullptr;
        }
        finished.set_value(status);
        return status;
    }
private:
    HarbingerService& broker_;
    std::mutex mutex_;
    grpc::ServerContext* context_{nullptr};
};

template <typename Pred>
bool wait_until(Pred pred) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!pred() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    return pred();
}
}

TEST(DeliveryRecovery, CancelledSelectedPullRestoresFrontWithoutLeaseOrRetry) {
    feedback_test::Directory feedback_directory;
    LocalBroker b{HarbingerConfig{
        .default_max_retries = 2, .ttl_sweep_interval = 0ms,
        .delivery_lease = 30s, .lease_sweep_interval = 30s,
        .ingress_features = capture_features(), .feedback = feedback_directory.config()}};
    const std::string payload{"\0\x80\xffX", 4};
    const std::unordered_map<std::string, std::string> headers{{"job", "binary"}, {"empty", ""}};
    const auto first_id = b.submit(payload, std::nullopt, headers);
    const auto second_id = b.submit("B");
    const auto before = BrokerTestAccess::queued_front(b.service);
    ASSERT_TRUE(before);
    ObservedPull observed{b.service};
    auto finished = observed.finished.get_future();
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&observed);
    auto server = builder.BuildAndStart();
    ASSERT_TRUE(server);
    struct ServerGuard {
        grpc::Server& server;
        ~ServerGuard() { server.Shutdown(); server.Wait(); }
    } server_guard{*server};
    auto stub = harbinger_rpc::Broker::NewStub(grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + 10s);
    harbinger_rpc::PullRequest req;
    req.set_consumer_id(b.consumer_id);
    req.set_timeout_ms(5000);
    harbinger_rpc::PullResponse resp;
    auto gate = BrokerTestAccess::gate_delivery(b.service);
    auto pending = std::async(std::launch::async, [&] { return stub->Pull(&ctx, req, &resp); });
    // No fatal assertions while holding the gate: always release before joining.
    EXPECT_TRUE(wait_until([&] { return b.service.queue_size() == 1; }));
    ctx.TryCancel();
    EXPECT_TRUE(wait_until([&] { return observed.cancelled(); }));
    gate.unlock();
    EXPECT_EQ(pending.get().error_code(), grpc::StatusCode::CANCELLED);
    ASSERT_EQ(finished.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(finished.get().error_code(), grpc::StatusCode::CANCELLED);
    EXPECT_TRUE(observed.cancelled_response_empty);
    EXPECT_EQ(b.service.queue_size(), 2u);
    EXPECT_EQ(b.service.in_flight_count(), 0u);
    EXPECT_EQ(b.service.dlq_size(), 0u);
    BrokerTestAccess::check_indexes(b.service, 0);
    const auto restored = BrokerTestAccess::queued_front(b.service);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->id, first_id);
    EXPECT_EQ(restored->priority, before->priority);
    EXPECT_EQ(restored->original_priority, before->original_priority);
    EXPECT_EQ(restored->retry_count, before->retry_count);
    EXPECT_EQ(restored->delivery_count, 0u);
    EXPECT_EQ(restored->enqueue_time, before->enqueue_time);
    EXPECT_EQ(restored->arrival_time, before->arrival_time);
    EXPECT_EQ(restored->payload, before->payload);
    EXPECT_EQ(restored->headers, before->headers);
    ASSERT_TRUE(before->routing_context);
    EXPECT_EQ(restored->routing_context, before->routing_context);

    // Register a different receiver, then prove both FIFO and the remaining budget.
    grpc::ClientContext registration;
    registration.set_deadline(std::chrono::system_clock::now() + 3s);
    harbinger_rpc::RegisterConsumerRequest registration_req;
    harbinger_rpc::RegisterConsumerResponse registration_resp;
    ASSERT_TRUE(b.stub->RegisterConsumer(&registration, registration_req, &registration_resp).ok());
    b.consumer_id = registration_resp.consumer_id();
    const auto start = std::chrono::steady_clock::now();
    const auto first = b.pull(200);
    ASSERT_FALSE(first.timed_out());
    EXPECT_EQ(first.message().message_id(), first_id);
    EXPECT_EQ(first.message().payload(), payload);
    for (const auto& [key, value] : headers) EXPECT_EQ(first.message().headers().at(key), value);
    EXPECT_TRUE(b.nack(first.message()).ok());
    EXPECT_EQ(b.service.dlq_size(), 0u);
    const auto second = b.pull(200);
    ASSERT_FALSE(second.timed_out());
    EXPECT_EQ(second.message().message_id(), second_id);
    EXPECT_TRUE(b.ack(second.message()).ok());
    const auto retry = b.pull(200);
    ASSERT_FALSE(retry.timed_out());
    EXPECT_EQ(retry.message().message_id(), first_id);
    EXPECT_EQ(retry.message().payload(), payload);
    EXPECT_TRUE(b.ack(retry.message()).ok());
    EXPECT_LT(std::chrono::steady_clock::now() - start, 3s);
    BrokerTestAccess::close_feedback(b.service);
    const auto events = feedback_test::records(feedback_directory);
    ASSERT_EQ(events.size(), 5u);
    for (const auto& event : events) {
        if (event.fields().at("event_type").string_value() != "outcome") continue;
        const auto& fields = event.fields();
        const auto expected = fields.at("message_id").string_value() == first_id &&
            fields.at("outcome").string_value() == "ack" ? "2" : "1";
        EXPECT_EQ(fields.at("attempt_id").string_value(), expected);
    }
}

TEST(IngressContext, OptInStaticCapturePreservesFeaturesAcrossRetryAndDlqWithoutRpcLeakage) {
    HarbingerConfig config{.default_max_retries = 2, .ingress_features = capture_features()};
    LocalBroker b{config};
    // Configuration is owned by the broker, not borrowed from the caller.
    config.ingress_features->schema.version = "changed";
    config.ingress_features->schema.headers.clear();
    std::unordered_map<std::string, std::string> headers{{"job", "resize"}, {"units", "2"}, {"authorization", "secret"}};
    const std::string payload(1024 * 1024, '\xff');
    const auto id = b.submit(payload, std::nullopt, headers);
    headers["units"] = "999";
    const auto queued = BrokerTestAccess::queued_front(b.service);
    ASSERT_TRUE(queued);
    const auto context = queued->routing_context;
    ASSERT_TRUE(context);
    EXPECT_EQ(context->feature_schema_version, "features-v1-job-units");
    EXPECT_EQ(context->routing_policy_version, "static-v1-three-levels-priority-1");
    EXPECT_EQ(context->mode, ml::RoutingMode::Disabled);
    EXPECT_EQ(context->ingress_priority, 1);
    EXPECT_FALSE(context->model_version);
    EXPECT_FALSE(context->predicted_processing_time_ms);
    EXPECT_FALSE(context->predicted_bucket);
    EXPECT_FALSE(context->fallback_reason);
    EXPECT_FALSE(context->inference_elapsed_ms);
    ASSERT_TRUE(context->features);
    EXPECT_EQ(context->features->payload_size_bytes, payload.size());
    EXPECT_EQ(std::get<double>(context->features->headers.at("units")), 2);
    EXPECT_EQ(context->features->headers.size(), 2u);
    EXPECT_FALSE(context->features->headers.contains("authorization"));
    EXPECT_FALSE(context->features->headers.contains("__producer_id"));
    auto delivery = b.pull();
    EXPECT_EQ(delivery.message().message_id(), id);
    EXPECT_EQ(delivery.message().payload(), payload);
    EXPECT_EQ(delivery.message().headers().size(), 4u); // Three originals plus existing producer stamp.
    EXPECT_EQ(delivery.message().headers().at("authorization"), "secret");
    EXPECT_EQ(delivery.message().headers().at("units"), "2");
    EXPECT_EQ(BrokerTestAccess::in_flight_context(b.service, id), context);
    ASSERT_TRUE(b.nack(delivery.message()).ok());
    const auto retried = BrokerTestAccess::queued_front(b.service);
    ASSERT_TRUE(retried);
    EXPECT_EQ(retried->routing_context, context);
    EXPECT_EQ(retried->priority, retried->original_priority);
    delivery = b.pull();
    EXPECT_EQ(BrokerTestAccess::in_flight_context(b.service, id), context);
    ASSERT_TRUE(b.nack(delivery.message()).ok());
    const auto dlq = b.service.dlq_snapshot(0, 10);
    ASSERT_EQ(dlq.size(), 1u);
    EXPECT_EQ(dlq[0].message.routing_context, context);
    EXPECT_EQ(dlq[0].reason, DLQReason::MAX_RETRIES_EXCEEDED);
    const auto inspection = b.inspect();
    ASSERT_EQ(inspection.entries_size(), 1);
    EXPECT_EQ(inspection.entries(0).headers().size(), 4u);
    EXPECT_EQ(inspection.entries(0).payload(), payload);
}

TEST(IngressContext, DefaultDisabledPathAllocatesNoContext) {
    LocalBroker b{HarbingerConfig{}};
    const auto id = b.submit("");
    const auto queued = BrokerTestAccess::queued_front(b.service);
    ASSERT_TRUE(queued);
    EXPECT_FALSE(queued->routing_context);
    const auto delivery = b.pull();
    EXPECT_FALSE(BrokerTestAccess::in_flight_context(b.service, id));
    EXPECT_TRUE(b.ack(delivery.message()).ok());
}

TEST(IngressContext, LeaseRetryAndEveryTtlDispositionRetainSnapshot) {
    for (int path = 0; path < 4; ++path) {
        SCOPED_TRACE(path);
        LocalBroker b{HarbingerConfig{
            .default_max_retries = 2, .ttl_sweep_interval = 0ms,
            .delivery_lease = 30s, .lease_sweep_interval = 30s,
            .ingress_features = capture_features()}};
        const auto id = b.submit("", std::nullopt, {{"job", "binary"}});
        const auto queued = BrokerTestAccess::queued_front(b.service);
        ASSERT_TRUE(queued);
        const auto context = queued->routing_context;
        ASSERT_TRUE(context);
        EXPECT_EQ(context->features->payload_size_bytes, 0u);
        if (path == 3) {
            BrokerTestAccess::expire_queued(b.service);
        } else {
            const auto delivery = b.pull();
            if (path == 2) {
                BrokerTestAccess::expire_ttl(b.service, id);
                ASSERT_TRUE(b.ack(delivery.message()).ok());
            } else {
                BrokerTestAccess::expire(b.service, id, path == 1);
                BrokerTestAccess::maintain(b.service);
            }
            if (path == 0) {
                const auto retried = BrokerTestAccess::queued_front(b.service);
                ASSERT_TRUE(retried);
                EXPECT_EQ(retried->routing_context, context);
                EXPECT_EQ(retried->retry_count, 1u);
                const auto retry_delivery = b.pull();
                BrokerTestAccess::expire(b.service, id);
                BrokerTestAccess::maintain(b.service);
                EXPECT_EQ(b.ack(retry_delivery.message()).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
            }
        }
        const auto dlq = b.service.dlq_snapshot(0, 10);
        ASSERT_EQ(dlq.size(), 1u);
        EXPECT_EQ(dlq[0].message.routing_context, context);
        EXPECT_EQ(dlq[0].reason, path == 0 ? DLQReason::MAX_RETRIES_EXCEEDED : DLQReason::TTL_EXPIRED);
        EXPECT_EQ(dlq[0].message.retry_count, path == 0 ? 2u : 0u);
    }
}

TEST(IngressContext, OversizedSnapshotDoesNotRejectSubmissionOrChangeStaticPriority) {
    auto capture = capture_features();
    capture.schema.headers.clear();
    std::unordered_map<std::string, std::string> headers;
    for (int i = 0; i < 16; ++i) {
        const auto key = "h" + std::to_string(i);
        capture.schema.headers.push_back({.name = key, .type = ml::FeatureType::Categorical});
        headers[key] = std::string(256, '\0');
    }
    LocalBroker b{HarbingerConfig{.ingress_features = capture}};
    b.submit("", std::nullopt, headers);
    const auto queued = BrokerTestAccess::queued_front(b.service);
    ASSERT_TRUE(queued);
    ASSERT_TRUE(queued->routing_context);
    EXPECT_EQ(queued->priority, 1);
    EXPECT_EQ(queued->original_priority, 1);
    EXPECT_EQ(queued->routing_context->feature_validity, ml::FeatureValidity::FeatureLimit);
    EXPECT_FALSE(queued->routing_context->features);
    EXPECT_FALSE(queued->routing_context->fallback_reason);
    const auto delivery = b.pull();
    EXPECT_TRUE(b.ack(delivery.message()).ok());
}

TEST(IngressContext, InvalidConfigurationFailsConstruction) {
    auto config = HarbingerConfig{.ingress_features = capture_features()};
    config.ingress_features->routing_policy_version.clear();
    EXPECT_THROW(HarbingerService{config}, std::invalid_argument);
    config.ingress_features->routing_policy_version = "static-v1";
    config.ingress_features->schema.headers[0].name = "__producer_id";
    EXPECT_THROW(HarbingerService{config}, std::invalid_argument);
}

TEST(DeliveryRecovery, DlqInspectionPreservesEmptyAndBinaryPayloads) {
    for (const auto& payload : {std::string{}, std::string{"\0\x80\xffX", 4}, std::string(65536, '\xff')}) {
        LocalBroker b{HarbingerConfig{.default_max_retries = 1}};
        const auto id = b.submit(payload, std::nullopt, {{"job", "binary"}, {"empty", ""}});
        const auto delivery = b.pull();
        ASSERT_FALSE(delivery.timed_out());
        EXPECT_EQ(delivery.message().payload(), payload);
        ASSERT_TRUE(b.nack(delivery.message()).ok());
        for (int repeat = 0; repeat < 2; ++repeat) {
            const auto page = b.inspect();
            ASSERT_EQ(page.entries_size(), 1);
            EXPECT_EQ(page.entries(0).message_id(), id);
            EXPECT_EQ(page.entries(0).payload(), payload);
            EXPECT_EQ(page.entries(0).headers().at("job"), "binary");
            EXPECT_EQ(page.entries(0).headers().at("empty"), "");
            EXPECT_EQ(page.entries(0).headers().at("__producer_id"), b.producer_id);
        }
        EXPECT_EQ(b.service.dlq_size(), 1u);
    }
}

TEST_F(BrokerTest, PullCapsRequestedWaitAtServerMaximum) {
    LocalBroker b{HarbingerConfig{.max_pull_wait = 100ms}};
    const auto start = std::chrono::steady_clock::now();
    const auto resp = b.pull(30000 /*ms*/);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(resp.timed_out());
    EXPECT_LT(elapsed, 500ms);
}

TEST_F(BrokerTest, UnsetTtlUsesServerDefault) {
    LocalBroker b{HarbingerConfig{.default_ttl = 100ms}};
    EXPECT_EQ(b.service.queue_size(), 0u);
    b.submit("stale"); // no ttl_ms field set
    EXPECT_EQ(b.service.queue_size(), 1u);

    std::this_thread::sleep_for(400ms);
    const auto resp = b.pull(1000);

    EXPECT_TRUE(resp.timed_out());
    EXPECT_EQ(b.service.queue_size(), 0u);
    EXPECT_EQ(b.service.in_flight_count(), 0u);
    EXPECT_EQ(b.service.dlq_size(), 1u);
    const auto snap = b.service.dlq_snapshot(0, 10);
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].reason, DLQReason::TTL_EXPIRED);
}

TEST_F(BrokerTest, ExplicitZeroTtlDisablesExpiry) {
    LocalBroker b{HarbingerConfig{.default_ttl = 100ms}};
    b.submit("live", 0); // explicit ttl_ms=0 beats the default

    std::this_thread::sleep_for(400ms);
    const auto resp = b.pull(1000);

    ASSERT_FALSE(resp.timed_out());
    EXPECT_EQ(resp.message().payload(), "live");
    EXPECT_EQ(b.service.dlq_size(), 0u);
}

TEST_F(BrokerTest, PerMessageTtlExpiresWhenDefaultOff) {
    const auto prod = register_producer();
    const auto cons = register_consumer();
    submit_with_ttl(prod, 100, "short-lived"); // fixture default_ttl=0 (off)

    std::this_thread::sleep_for(400ms);
    const auto resp = pull(cons, 1000);

    EXPECT_TRUE(resp.timed_out());
    EXPECT_EQ(service_.dlq_size(), 1u);
    EXPECT_EQ(service_.in_flight_count(), 0u);
}

TEST_F(BrokerTest, SubmitNegativeTtlRejected) {
    const auto prod = register_producer();

    harbinger_rpc::SubmitRequest req;
    req.set_producer_id(prod);
    req.set_payload("x");
    req.set_ttl_ms(-5);

    harbinger_rpc::SubmitResponse resp;
    grpc::ClientContext ctx;
    EXPECT_EQ(stub_->Submit(&ctx, req, &resp).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(service_.queue_size(), 0u);
}

TEST_F(BrokerTest, NeverPulledExpiredReclaimedBySweeper) {
    LocalBroker b{HarbingerConfig{.default_ttl = 100ms,
                              .ttl_sweep_interval = 50ms}};
    for (int i = 0; i < 5; ++i) b.submit("stale");
    EXPECT_EQ(b.service.queue_size(), 5u);

    // No Pull at all: the background sweeper must reclaim everything.
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (b.service.dlq_size() < 5u) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(10ms);
    }

    EXPECT_EQ(b.service.queue_size(), 0u);
    EXPECT_EQ(b.service.in_flight_count(), 0u);
    const auto snap = b.service.dlq_snapshot(0, 10);
    ASSERT_EQ(snap.size(), 5u);
    for (const auto& e : snap) {
        EXPECT_EQ(e.reason, DLQReason::TTL_EXPIRED);
    }
}

TEST_F(BrokerTest, NackAfterExpiryGoesToDlq) {
    LocalBroker b{HarbingerConfig{.default_ttl = 200ms}};
    b.submit("doomed");
    const auto pulled = b.pull(1000); // live at pull time
    ASSERT_FALSE(pulled.timed_out());

    std::this_thread::sleep_for(300ms); // expire mid-processing
    EXPECT_TRUE(b.nack(pulled.message(), "late-failure").ok());

    EXPECT_EQ(b.service.queue_size(), 0u); // not requeued
    EXPECT_EQ(b.service.in_flight_count(), 0u);
    EXPECT_EQ(b.service.dlq_size(), 1u);
    const auto snap = b.service.dlq_snapshot(0, 10);
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].reason, DLQReason::TTL_EXPIRED);
}

TEST_F(BrokerTest, AckAfterExpiryGoesToDlq) {
    LocalBroker b{HarbingerConfig{.default_ttl = 200ms}};
    b.submit("done-late");
    const auto pulled = b.pull(1000);
    ASSERT_FALSE(pulled.timed_out());

    std::this_thread::sleep_for(300ms);
    EXPECT_TRUE(b.ack(pulled.message()).ok()); // still OK to the caller

    EXPECT_EQ(b.service.in_flight_count(), 0u);
    EXPECT_EQ(b.service.queue_size(), 0u);
    EXPECT_EQ(b.service.dlq_size(), 1u);
    const auto snap = b.service.dlq_snapshot(0, 10);
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].reason, DLQReason::TTL_EXPIRED);
}

TEST_F(BrokerTest, NackBeforeExpiryStillRetries) {
    LocalBroker b{HarbingerConfig{.default_ttl = 5000ms}};
    b.submit("retry-me");
    const auto pulled = b.pull(1000);
    ASSERT_FALSE(pulled.timed_out());

    EXPECT_TRUE(b.nack(pulled.message(), "transient").ok());

    EXPECT_EQ(b.service.queue_size(), 1u); // requeued, not expired
    EXPECT_EQ(b.service.dlq_size(), 0u);
    EXPECT_EQ(b.service.in_flight_count(), 0u);
}

TEST_F(BrokerTest, PullSkipsExpiredHeadAndDeliversFresh) {
    LocalBroker b{HarbingerConfig{.default_ttl = 100ms,
                              .ttl_sweep_interval = 0ms}}; // Pull path only
    b.submit("old");
    std::this_thread::sleep_for(300ms); // "old" now expired
    b.submit("fresh");                  // live, queued behind "old"

    const auto resp = b.pull(1000);

    ASSERT_FALSE(resp.timed_out());
    EXPECT_EQ(resp.message().payload(), "fresh");
    EXPECT_EQ(b.service.dlq_size(), 1u);
    EXPECT_EQ(b.service.queue_size(), 0u);
    EXPECT_EQ(b.service.in_flight_count(), 1u);

    // Queue drained: a second Pull finds nothing.
    EXPECT_TRUE(b.pull(200).timed_out());
}

TEST_F(BrokerTest, PullDrainsMultipleExpiredInOneCall) {
    LocalBroker b{HarbingerConfig{.default_ttl = 100ms,
                              .ttl_sweep_interval = 0ms}}; // Pull path only
    b.submit("a");
    b.submit("b");
    b.submit("c");
    EXPECT_EQ(b.service.queue_size(), 3u);

    std::this_thread::sleep_for(400ms);
    EXPECT_TRUE(b.pull(1000).timed_out());

    EXPECT_EQ(b.service.queue_size(), 0u);
    EXPECT_EQ(b.service.in_flight_count(), 0u);
    EXPECT_EQ(b.service.dlq_size(), 3u);
}

TEST_F(BrokerTest, ZeroTtlNeverExpires) {
    const auto prod = register_producer();
    const auto cons = register_consumer();
    submit(prod, "live"); // fixture default_ttl=0 (off)

    std::this_thread::sleep_for(300ms);
    const auto resp = pull(cons, 500);

    ASSERT_FALSE(resp.timed_out());
    EXPECT_EQ(resp.message().payload(), "live");
    EXPECT_EQ(service_.dlq_size(), 0u);
    EXPECT_EQ(service_.in_flight_count(), 1u);
}

TEST_F(BrokerTest, InspectDlqReturnsTtlEntries) {
    LocalBroker b{HarbingerConfig{.default_ttl = 100ms,
                              .ttl_sweep_interval = 0ms}};
    b.submit("one");
    b.submit("two");
    std::this_thread::sleep_for(400ms);
    EXPECT_TRUE(b.pull(1000).timed_out());
    ASSERT_EQ(b.service.dlq_size(), 2u);

    const auto resp = b.inspect();

    EXPECT_EQ(resp.total_size(), 2u);
    ASSERT_EQ(resp.entries_size(), 2);
    EXPECT_EQ(resp.entries(0).payload(), "one");
    EXPECT_EQ(resp.entries(1).payload(), "two");
    for (const auto& e : resp.entries()) {
        EXPECT_EQ(e.reason(), harbinger_rpc::DLQ_TTL_EXPIRED);
        EXPECT_FALSE(e.message_id().empty());
        EXPECT_GE(e.dlq_age_ms(), 0);
        EXPECT_EQ(e.retry_count(), 0u);
    }
}

TEST_F(BrokerTest, InspectDlqPagination) {
    LocalBroker b{HarbingerConfig{.default_ttl = 100ms,
                              .ttl_sweep_interval = 0ms}};
    b.submit("a");
    b.submit("b");
    b.submit("c");
    std::this_thread::sleep_for(400ms);
    EXPECT_TRUE(b.pull(1000).timed_out());
    ASSERT_EQ(b.service.dlq_size(), 3u);

    const auto page1 = b.inspect(2, 0);
    EXPECT_EQ(page1.total_size(), 3u);
    ASSERT_EQ(page1.entries_size(), 2);
    EXPECT_EQ(page1.entries(0).payload(), "a");
    EXPECT_EQ(page1.entries(1).payload(), "b");

    const auto page2 = b.inspect(2, 2);
    EXPECT_EQ(page2.total_size(), 3u);
    ASSERT_EQ(page2.entries_size(), 1);
    EXPECT_EQ(page2.entries(0).payload(), "c");

    // Default limit (<=0) returns everything here; over-limit is capped.
    EXPECT_EQ(b.inspect(0, 0).entries_size(), 3);
    EXPECT_EQ(b.inspect(1000, 0).entries_size(), 3);
    EXPECT_EQ(b.inspect(2, 10).entries_size(), 0);
}

TEST(DeliveryRecovery, DuplicateSettlementIsIdempotentAcrossRedelivery) {
    LocalBroker b{HarbingerConfig{}};
    b.submit();
    const auto first = b.pull().message();
    ASSERT_TRUE(b.nack(first).ok());
    const auto second = b.pull().message();
    EXPECT_EQ(first.message_id(), second.message_id());
    EXPECT_NE(first.attempt_token(), second.attempt_token());
    for (int i = 0; i < 20; ++i) EXPECT_TRUE(b.nack(first).ok());
    EXPECT_EQ(b.ack(first).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_EQ(b.service.in_flight_count(), 1u);
    EXPECT_TRUE(b.ack(second).ok());
    EXPECT_TRUE(b.ack(second).ok());
    EXPECT_EQ(b.nack(second).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_EQ(b.service.queue_size(), 0u);
    EXPECT_EQ(b.service.dlq_size(), 0u);
    BrokerTestAccess::check_indexes(b.service, 2);
}

TEST(DeliveryRecovery, BackgroundLeaseRecoveryWorksWithTtlSweeperDisabled) {
    LocalBroker b{HarbingerConfig{.ttl_sweep_interval = 0ms, .delivery_lease = 50ms,
                                .lease_sweep_interval = 10ms}};
    b.submit();
    const auto first = b.pull().message();
    ASSERT_EQ(first.lease_duration_ms(), 50);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (b.service.in_flight_count() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    ASSERT_EQ(b.service.in_flight_count(), 0u);
    ASSERT_EQ(b.service.queue_size(), 1u);
    const auto second = b.pull().message();
    EXPECT_EQ(second.message_id(), first.message_id());
    EXPECT_NE(second.attempt_token(), first.attempt_token());
    EXPECT_EQ(b.ack(first).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_EQ(b.nack(first).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_TRUE(b.ack(second).ok());
}

TEST(DeliveryRecovery, DeadlineCheckedWithoutMaintenanceAndFailuresExhaustBudget) {
    LocalBroker b{HarbingerConfig{.default_max_retries = 2, .lease_sweep_interval = 1h}};
    b.submit();
    auto first = b.pull().message();
    BrokerTestAccess::expire(b.service, first.message_id());
    EXPECT_EQ(b.ack(first).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    auto second = b.pull().message();
    BrokerTestAccess::expire(b.service, second.message_id());
    BrokerTestAccess::maintain(b.service);
    const auto entries = b.service.dlq_snapshot(0, 10);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].reason, DLQReason::MAX_RETRIES_EXCEEDED);
    EXPECT_EQ(entries[0].message.retry_count, 2u);
    EXPECT_EQ(b.service.in_flight_count(), 0u);
    EXPECT_EQ(b.service.queue_size(), 0u);
}

TEST(DeliveryRecovery, ExpiredTtlWinsOverLeaseFailureAccounting) {
    LocalBroker b{HarbingerConfig{.default_max_retries = 1, .lease_sweep_interval = 1h}};
    b.submit();
    auto delivery = b.pull().message();
    BrokerTestAccess::expire(b.service, delivery.message_id(), true);
    BrokerTestAccess::maintain(b.service);
    const auto entries = b.service.dlq_snapshot(0, 10);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].reason, DLQReason::TTL_EXPIRED);
    EXPECT_EQ(entries[0].message.retry_count, 0u);
    EXPECT_EQ(b.ack(delivery).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

TEST(DeliveryRecovery, ConcurrentDuplicatesAndOppositeOperationsHaveOneOutcome) {
    LocalBroker b{HarbingerConfig{.default_max_retries = 1}};
    b.submit();
    auto delivery = b.pull().message();
    std::barrier start{3};
    auto a = std::async(std::launch::async, [&] { start.arrive_and_wait(); return b.nack(delivery); });
    auto c = std::async(std::launch::async, [&] { start.arrive_and_wait(); return b.nack(delivery); });
    start.arrive_and_wait();
    EXPECT_TRUE(a.get().ok());
    EXPECT_TRUE(c.get().ok());
    ASSERT_EQ(b.service.dlq_size(), 1u);
    EXPECT_EQ(b.service.dlq_snapshot(0, 1)[0].message.retry_count, 1u);
    b.submit();
    delivery = b.pull().message();
    std::barrier conflict{3};
    auto ack = std::async(std::launch::async, [&] { conflict.arrive_and_wait(); return b.ack(delivery); });
    auto nack = std::async(std::launch::async, [&] { conflict.arrive_and_wait(); return b.nack(delivery); });
    conflict.arrive_and_wait();
    const auto ack_status = ack.get();
    const auto nack_status = nack.get();
    EXPECT_NE(ack_status.ok(), nack_status.ok());
    EXPECT_EQ((ack_status.ok() ? nack_status : ack_status).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_EQ(b.service.dlq_size(), ack_status.ok() ? 1u : 2u);
}

TEST(DeliveryRecovery, ExpiryRacingSettlementCannotDuplicateDisposition) {
    LocalBroker b{HarbingerConfig{.default_max_retries = 1, .lease_sweep_interval = 1h}};
    b.submit();
    auto delivery = b.pull().message();
    BrokerTestAccess::expire(b.service, delivery.message_id());
    std::barrier start{3};
    auto sweep = std::async(std::launch::async, [&] { start.arrive_and_wait(); BrokerTestAccess::maintain(b.service); });
    auto ack = std::async(std::launch::async, [&] { start.arrive_and_wait(); return b.ack(delivery); });
    start.arrive_and_wait();
    sweep.get();
    EXPECT_EQ(ack.get().error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_EQ(b.service.dlq_size(), 1u);
    EXPECT_EQ(b.service.dlq_snapshot(0, 1)[0].message.retry_count, 1u);
}

TEST(DeliveryRecovery, CompletionCacheIsBoundedAndEvictionDoesNotAuthorizeOldAttempts) {
    LocalBroker b{HarbingerConfig{.lease_sweep_interval = 1h, .completion_cache_max_entries = 2}};
    b.submit();
    const auto old = b.pull().message();
    ASSERT_TRUE(b.nack(old).ok());
    const auto current = b.pull().message();
    for (int i = 0; i < 4; ++i) {
        b.submit();
        const auto d = b.pull().message();
        EXPECT_TRUE(b.ack(d).ok());
        BrokerTestAccess::check_indexes(b.service, 2);
    }
    EXPECT_EQ(b.ack(old).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_EQ(b.nack(old).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_TRUE(b.ack(current).ok());
    BrokerTestAccess::expire_history(b.service);
    EXPECT_EQ(b.ack(current).error_code(), grpc::StatusCode::NOT_FOUND);
    BrokerTestAccess::maintain(b.service);
    BrokerTestAccess::check_indexes(b.service, 0);
}

TEST(DeliveryRecovery, MaintenanceBatchBoundsRecoveryAndHistoryOwnerIsChecked) {
    LocalBroker b{HarbingerConfig{.lease_sweep_interval = 1h, .maintenance_batch_size = 1}};
    for (int i = 0; i < 3; ++i) {
        b.submit();
        auto d = b.pull().message();
        BrokerTestAccess::expire(b.service, d.message_id());
    }
    BrokerTestAccess::maintain(b.service);
    EXPECT_EQ(b.service.in_flight_count(), 2u);
    EXPECT_EQ(b.service.queue_size(), 1u);
    auto d = b.pull().message();
    EXPECT_TRUE(b.ack(d).ok());
    harbinger_rpc::RegisterConsumerRequest req;
    harbinger_rpc::RegisterConsumerResponse resp;
    grpc::ClientContext ctx;
    ASSERT_TRUE(b.stub->RegisterConsumer(&ctx, req, &resp).ok());
    b.consumer_id = resp.consumer_id();
    EXPECT_EQ(b.ack(d).error_code(), grpc::StatusCode::PERMISSION_DENIED);
}

TEST(DeliveryRecovery, MissingAndOversizedTokensAreRejected) {
    LocalBroker b{HarbingerConfig{}};
    b.submit();
    auto d = b.pull().message();
    auto bad = d;
    bad.clear_attempt_token();
    EXPECT_EQ(b.ack(bad).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    bad.set_attempt_token(std::string(129, 'x'));
    EXPECT_EQ(b.nack(bad).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(b.service.in_flight_count(), 1u);
    EXPECT_TRUE(b.ack(d).ok());
}

TEST(ConfigValidationTest, RejectsInvalidRecoveryConfiguration) {
    EXPECT_THROW((HarbingerService{HarbingerConfig{.delivery_lease = 0ms}}), std::invalid_argument);
    EXPECT_THROW((HarbingerService{HarbingerConfig{.lease_sweep_interval = 0ms}}), std::invalid_argument);
    EXPECT_THROW((HarbingerService{HarbingerConfig{.completion_retention = 0ms}}), std::invalid_argument);
    EXPECT_THROW((HarbingerService{HarbingerConfig{.completion_cache_max_entries = 0}}), std::invalid_argument);
    EXPECT_THROW((HarbingerService{HarbingerConfig{.maintenance_batch_size = 0}}), std::invalid_argument);
}

namespace {
HarbingerConfig feedback_config(const feedback_test::Directory& directory) {
    HarbingerConfig config;
    config.ttl_sweep_interval = 0ms;
    config.lease_sweep_interval = 1h;
    config.ingress_features = capture_features();
    config.feedback = directory.config();
    return config;
}
std::vector<google::protobuf::Struct> outcomes(const feedback_test::Directory& directory) {
    auto rows = feedback_test::records(directory);
    std::erase_if(rows, [](const auto& row) { return row.fields().at("event_type").string_value() != "outcome"; });
    return rows;
}
}

TEST(FeedbackBroker, RetriesLeaseExpiryAndReplayPreserveFirstMeasurementAndAttemptSequence) {
    feedback_test::Directory directory;
    LocalBroker b{feedback_config(directory)};
    const auto id = b.submit("raw-payload-sentinel", std::nullopt,
        {{"job", "resize"}, {"units", "2"}, {"authorization", "private-sentinel"}});
    const auto first = b.pull().message();
    ASSERT_TRUE(b.nack(first, "failure-text-sentinel", 0).ok());
    const auto second = b.pull().message();
    EXPECT_TRUE(b.nack(first, "changed", 999).ok());
    EXPECT_EQ(b.ack(first).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    BrokerTestAccess::expire(b.service, id);
    EXPECT_EQ(b.ack(second, 777).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    const auto third = b.pull().message();
    EXPECT_TRUE(b.ack(third, 0).ok()); EXPECT_TRUE(b.ack(third, 888).ok());
    BrokerTestAccess::close_feedback(b.service);
    const auto rows = outcomes(directory); ASSERT_EQ(rows.size(), 3u);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& f = rows[i].fields();
        EXPECT_EQ(f.at("message_id").string_value(), id);
        EXPECT_EQ(f.at("attempt_id").string_value(), std::to_string(i + 1));
        const auto& context = f.at("routing").struct_value().fields();
        EXPECT_EQ(context.at("ingress_priority").number_value(), 1);
        EXPECT_EQ(context.at("features").struct_value().fields().at("headers").struct_value().fields()
            .at("units").number_value(), 2);
        EXPECT_EQ(rows[i].DebugString().find("sentinel"), std::string::npos);
        EXPECT_EQ(rows[i].DebugString().find(first.attempt_token()), std::string::npos);
    }
    EXPECT_EQ(rows[0].fields().at("outcome").string_value(), "retry");
    EXPECT_EQ(rows[0].fields().at("processing_time_ms").number_value(), 0);
    EXPECT_EQ(rows[0].fields().at("label_status").string_value(), "failure");
    EXPECT_EQ(rows[1].fields().at("trigger").string_value(), "lease_expiry");
    EXPECT_EQ(rows[1].fields().at("processing_time_ms").kind_case(), google::protobuf::Value::kNullValue);
    EXPECT_EQ(rows[1].fields().at("settlement_operation").kind_case(), google::protobuf::Value::kNullValue);
    EXPECT_EQ(rows[1].fields().at("retry_count").number_value(), 2);
    EXPECT_EQ(rows[2].fields().at("label_status").string_value(), "eligible");
}

TEST(FeedbackBroker, TtlOutcomesAndInvalidMeasurementsRespectPrecedenceWithoutChangingRpcStatus) {
    for (const bool nack : {false, true}) {
        for (const int64_t runtime : {int64_t{-1}, int64_t{0}, int64_t{30001}}) {
            feedback_test::Directory directory;
            LocalBroker b{feedback_config(directory)};
            b.submit(); const auto delivery = b.pull().message();
            BrokerTestAccess::expire_ttl(b.service, delivery.message_id());
            EXPECT_TRUE((nack ? b.nack(delivery, "failure", runtime) : b.ack(delivery, runtime)).ok());
            EXPECT_EQ(b.service.dlq_size(), 1u);
            BrokerTestAccess::close_feedback(b.service);
            const auto rows = outcomes(directory); ASSERT_EQ(rows.size(), 1u);
            const auto& f = rows[0].fields();
            EXPECT_EQ(f.at("outcome").string_value(), "dlq");
            EXPECT_EQ(f.at("dlq_reason").string_value(), "TTL_EXPIRED");
            EXPECT_EQ(f.at("retry_count").number_value(), 0);
            EXPECT_EQ(f.at("settlement_operation").string_value(), nack ? "nack" : "ack");
            EXPECT_EQ(f.at("label_status").string_value(), runtime < 0 ? "negative" : runtime > 30000 ? "out_of_range" : "censored");
        }
    }
}

TEST(FeedbackBroker, EveryExpiryPathIncludingNeverPulledAndPreviouslyRetriedMessagesEmitsMissingLabels) {
    for (int path = 0; path < 6; ++path) {
        feedback_test::Directory directory;
        auto config = feedback_config(directory);
        if (path == 5) { config.ttl_sweep_interval = 5ms; config.default_ttl = 10ms; }
        LocalBroker b{config};
        auto storage = std::make_shared<feedback_test::Storage>();
        feedback_test::ReleaseStorage release{storage};
        storage->gate();
        BrokerTestAccess::replace_writer(b.service, storage);
        ASSERT_TRUE(BrokerTestAccess::prime_ingress(b.service));
        ASSERT_TRUE(storage->await_entry());
        const auto before = b.service.feedback_stats();
        const auto id = b.submit();
        if (path == 0 || path == 1) {
            const auto delivery = b.pull().message();
            if (path == 1) { EXPECT_TRUE(b.nack(delivery).ok()); }
            if (path == 0) { BrokerTestAccess::expire(b.service, id, true); BrokerTestAccess::maintain(b.service); }
            else BrokerTestAccess::expire_queued(b.service);
        } else if (path == 2) {
            BrokerTestAccess::expire_selected(b.service);
            EXPECT_TRUE(b.pull(20).timed_out());
        } else if (path == 3) {
            // The helper drives the same swept-message disposition used by background maintenance.
            BrokerTestAccess::expire_queued(b.service);
        } else if (path == 4) {
            b.submit("expires", 1); std::this_thread::sleep_for(5ms);
            const auto live = b.pull().message(); EXPECT_EQ(live.message_id(), id); EXPECT_TRUE(b.ack(live).ok());
        } else { ASSERT_TRUE(feedback_test::wait_for([&] { return b.service.dlq_size() == 1; })); }
        // DLQ visibility precedes publication; join the sweeper while the writer is still parked.
        BrokerTestAccess::stop_maintenance(b.service);
        EXPECT_EQ(b.service.feedback_stats().dropped, before.dropped);
        storage->release();
        BrokerTestAccess::close_feedback(b.service);
        auto rows = outcomes(directory);
        std::erase_if(rows, [](const auto& row) { return row.fields().at("outcome").string_value() != "dlq"; });
        ASSERT_EQ(rows.size(), 1u) << path;
        const auto& f = rows[0].fields();
        EXPECT_EQ(f.at("dlq_reason").string_value(), "TTL_EXPIRED");
        EXPECT_EQ(f.at("label_status").string_value(), "missing");
        EXPECT_EQ(f.at("processing_time_ms").kind_case(), google::protobuf::Value::kNullValue);
        if (path == 0) {
            EXPECT_EQ(f.at("attempt_id").string_value(), "1");
            EXPECT_EQ(f.at("trigger").string_value(), "lease_expiry");
        } else {
            EXPECT_EQ(f.at("attempt_id").kind_case(), google::protobuf::Value::kNullValue);
            EXPECT_EQ(f.at("trigger").string_value(), path == 2 ? "pull_expiry" : "ttl_sweep");
        }
        EXPECT_EQ(f.at("retry_count").number_value(), path == 1 ? 1 : 0);
    }
}

TEST(FeedbackBroker, MaxRetriesRejectedOwnersStaleTokensAndEvictedReplaysDoNotCreateExtraLabels) {
    feedback_test::Directory directory;
    auto config = feedback_config(directory); config.default_max_retries = 2;
    config.completion_cache_max_entries = 1;
    LocalBroker b{config}; b.submit(); const auto first = b.pull().message();
    auto bad = first; bad.set_attempt_token("invalid token");
    EXPECT_EQ(b.ack(bad, 17).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    const auto owner = b.consumer_id;
    harbinger_rpc::RegisterConsumerRequest req; harbinger_rpc::RegisterConsumerResponse resp;
    grpc::ClientContext ctx; ASSERT_TRUE(b.stub->RegisterConsumer(&ctx, req, &resp).ok());
    b.consumer_id = resp.consumer_id();
    EXPECT_EQ(b.ack(first, 19).error_code(), grpc::StatusCode::PERMISSION_DENIED);
    b.consumer_id = owner;
    EXPECT_TRUE(b.nack(first).ok()); const auto second = b.pull().message();
    b.submit(); const auto other = b.pull().message(); EXPECT_TRUE(b.ack(other).ok());
    EXPECT_EQ(b.nack(first).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_EQ(b.ack(first).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_TRUE(b.nack(second).ok()); EXPECT_TRUE(b.nack(second).ok());
    BrokerTestAccess::close_feedback(b.service);
    const auto rows = outcomes(directory); ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows.back().fields().at("outcome").string_value(), "dlq");
    EXPECT_EQ(rows.back().fields().at("dlq_reason").string_value(), "MAX_RETRIES_EXCEEDED");
    EXPECT_EQ(rows.back().fields().at("retry_count").number_value(), 2);
    EXPECT_EQ(rows.back().fields().at("attempt_id").string_value(), "2");
}

TEST(FeedbackBroker, ConcurrentSettlementAndReclamationEmitExactlyOneOutcome) {
    for (bool expiry : {false, true}) {
        feedback_test::Directory directory;
        auto config = feedback_config(directory); config.default_max_retries = 1;
        LocalBroker b{config}; b.submit(); const auto delivery = b.pull().message();
        if (expiry) BrokerTestAccess::expire(b.service, delivery.message_id());
        std::barrier start{3};
        auto ack = std::async(std::launch::async, [&] { start.arrive_and_wait(); return b.ack(delivery); });
        auto competing = std::async(std::launch::async, [&] {
            start.arrive_and_wait();
            if (expiry) BrokerTestAccess::maintain(b.service); else (void)b.nack(delivery);
        });
        start.arrive_and_wait(); const auto status = ack.get(); competing.get();
        if (expiry) { EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION); }
        BrokerTestAccess::close_feedback(b.service);
        const auto rows = outcomes(directory); ASSERT_EQ(rows.size(), 1u);
        if (expiry) {
            EXPECT_EQ(rows[0].fields().at("trigger").string_value(), "lease_expiry");
            EXPECT_EQ(rows[0].fields().at("processing_time_ms").kind_case(), google::protobuf::Value::kNullValue);
        }
    }
}

TEST(FeedbackBroker, OverflowAndDiskFailurePreserveSettlementAndExposeTelemetryLoss) {
    for (bool disk_failure : {false, true}) {
        feedback_test::Directory directory;
        auto config = feedback_config(directory); config.feedback->buffer_records = 1;
        LocalBroker b{config};
        auto storage = std::make_shared<feedback_test::Storage>();
        feedback_test::ReleaseStorage release{storage};
        BrokerTestAccess::replace_writer(b.service, storage);
        if (disk_failure) storage->fault = feedback_test::Storage::Fault::Write;
        else storage->gate();
        b.submit();
        if (disk_failure) ASSERT_TRUE(feedback_test::wait_for([&] { return b.service.feedback_stats().writer_failures > 0; }));
        else EXPECT_TRUE(storage->await_entry());
        const auto delivery = b.pull().message();
        EXPECT_TRUE(b.ack(delivery).ok()); EXPECT_TRUE(b.ack(delivery).ok());
        EXPECT_EQ(b.service.in_flight_count(), 0u); EXPECT_EQ(b.service.dlq_size(), 0u);
        const auto stats = b.service.feedback_stats();
        EXPECT_GT(feedback_test::dropped(stats, disk_failure ? ml::FeedbackDrop::Storage : ml::FeedbackDrop::BufferFull), 0u);
        storage->release(); BrokerTestAccess::close_feedback(b.service);
    }
}

TEST(FeedbackBroker, InvalidCompleteFeaturesDoNotRejectSuccessfulSettlement) {
    feedback_test::Directory directory;
    auto config = feedback_config(directory);
    config.ingress_features->schema.headers.clear();
    std::unordered_map<std::string, std::string> headers;
    for (int i = 0; i < 16; ++i) {
        const auto key = std::string(60, 'k') + std::to_string(i);
        config.ingress_features->schema.headers.push_back({.name = key, .type = ml::FeatureType::Categorical});
        headers[key] = std::string(256, '\x01');
    }
    LocalBroker b{config}; b.submit("x", std::nullopt, headers);
    EXPECT_TRUE(b.ack(b.pull().message(), 0).ok());
    BrokerTestAccess::close_feedback(b.service);
    const auto rows = outcomes(directory); ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].fields().at("label_status").string_value(), "invalid_features");
    EXPECT_EQ(rows[0].fields().at("routing").struct_value().fields().at("features").kind_case(), google::protobuf::Value::kNullValue);
}

TEST(FeedbackBroker, ConfigurationRequiresExplicitFeatureVersionsAndDisabledStatsAreZero) {
    feedback_test::Directory directory;
    HarbingerConfig config; config.feedback = directory.config();
    EXPECT_THROW((HarbingerService{config}), std::invalid_argument);
    HarbingerService disabled;
    EXPECT_FALSE(disabled.feedback_stats().enabled);
    EXPECT_EQ(disabled.feedback_stats().admitted, 0u);
}

TEST(FeedbackBroker, ExhaustedDeliveryOrdinalRestoresMessageWithoutLeaseOrOutcome) {
    feedback_test::Directory directory;
    LocalBroker b{feedback_config(directory)}; b.submit();
    BrokerTestAccess::exhaust_delivery_count(b.service);
    harbinger_rpc::PullRequest req; req.set_consumer_id(b.consumer_id); req.set_timeout_ms(100);
    harbinger_rpc::PullResponse resp; grpc::ClientContext ctx;
    EXPECT_EQ(b.stub->Pull(&ctx, req, &resp).error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);
    EXPECT_EQ(b.service.queue_size(), 1u); EXPECT_EQ(b.service.in_flight_count(), 0u);
    BrokerTestAccess::close_feedback(b.service);
    EXPECT_TRUE(outcomes(directory).empty());
}

TEST(FeedbackBroker, LeaseMaintenancePublishesOnlyItsBoundedCommittedBatch) {
    feedback_test::Directory directory;
    auto config = feedback_config(directory); config.maintenance_batch_size = 1;
    LocalBroker b{config};
    auto storage = std::make_shared<feedback_test::Storage>();
    feedback_test::ReleaseStorage release{storage};
    storage->gate();
    BrokerTestAccess::replace_writer(b.service, storage);
    ASSERT_TRUE(BrokerTestAccess::prime_ingress(b.service));
    ASSERT_TRUE(storage->await_entry());
    // Park the worker in storage, outside the admission mutex, so every broker event is admitted.
    const auto before = b.service.feedback_stats();
    for (int i = 0; i < 3; ++i) {
        b.submit(); const auto delivery = b.pull().message();
        BrokerTestAccess::expire(b.service, delivery.message_id());
    }
    BrokerTestAccess::maintain(b.service);
    EXPECT_EQ(b.service.in_flight_count(), 2u); EXPECT_EQ(b.service.queue_size(), 1u);
    EXPECT_EQ(b.service.feedback_stats().captured - before.captured, 4u);
    EXPECT_EQ(b.service.feedback_stats().admitted - before.admitted, 4u);
    BrokerTestAccess::maintain(b.service); BrokerTestAccess::maintain(b.service);
    EXPECT_EQ(b.service.in_flight_count(), 0u); EXPECT_EQ(b.service.queue_size(), 3u);
    const auto after = b.service.feedback_stats();
    EXPECT_EQ(after.captured - before.captured, 6u);
    EXPECT_EQ(after.admitted - before.admitted, 6u);
    EXPECT_EQ(after.dropped, before.dropped);
    storage->release();
    BrokerTestAccess::close_feedback(b.service);
    const auto rows = outcomes(directory); ASSERT_EQ(rows.size(), 3u);
    for (const auto& row : rows) {
        EXPECT_EQ(row.fields().at("trigger").string_value(), "lease_expiry");
        EXPECT_EQ(row.fields().at("outcome").string_value(), "retry");
        EXPECT_EQ(row.fields().at("label_status").string_value(), "missing");
        EXPECT_EQ(row.fields().at("retry_count").number_value(), 1);
    }
}

TEST(FeedbackBroker, EmbeddedDestructionDrainsLogsAndRestartUsesNewNamespaceWithoutPython) {
    feedback_test::Directory directory;
    {
        LocalBroker b{feedback_config(directory)};
        b.submit("baseline", std::nullopt, {{"job", "resize"}, {"units", "4"}});
        EXPECT_TRUE(b.ack(b.pull().message(), 0).ok());
        // Normal server/service destruction, without explicitly closing the telemetry writer.
    }
    auto rows = feedback_test::records(directory); ASSERT_EQ(rows.size(), 2u);
    const auto instance = rows[0].fields().at("broker_instance_id").string_value();
    {
        auto config = feedback_config(directory); config.default_max_retries = 1;
        LocalBroker b{config}; b.submit("baseline"); EXPECT_TRUE(b.nack(b.pull().message()).ok());
    }
    rows = feedback_test::records(directory); ASSERT_EQ(rows.size(), 4u);
    std::set<std::string> namespaces;
    for (const auto& row : rows) namespaces.insert(row.fields().at("broker_instance_id").string_value());
    EXPECT_EQ(namespaces.size(), 2u); EXPECT_TRUE(namespaces.contains(instance));
}
