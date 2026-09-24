#include "harbinger_service.hpp"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <thread>

using namespace harbinger;
using namespace std::chrono_literals;

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
    harbinger_rpc::NackResponse nr; grpc::ClientContext c5;
    st->Nack(&c5, nq, &nr);

    EXPECT_EQ(svc.dlq_size(), 1u);
    srv->Shutdown();
}

namespace {

// Spin up a broker with a custom config on an ephemeral port.
// Returns the server (must outlive the test) and fills stub + ids.
struct LocalBroker {
    HarbingerService service;
    std::unique_ptr<grpc::Server> server;
    std::unique_ptr<harbinger_rpc::Broker::Stub> stub;
    std::string producer_id;
    std::string consumer_id;

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
                       std::optional<int64_t> ttl_ms = std::nullopt) {
        harbinger_rpc::SubmitRequest sq;
        sq.set_producer_id(producer_id);
        sq.set_payload(payload);
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

    void ack(const std::string& message_id) {
        harbinger_rpc::AckRequest aq;
        aq.set_consumer_id(consumer_id);
        aq.set_message_id(message_id);
        aq.set_processing_time_ms(5);
        harbinger_rpc::AckResponse aresp;
        grpc::ClientContext c;
        EXPECT_TRUE(stub->Ack(&c, aq, &aresp).ok());
    }

    void nack(const std::string& message_id, const std::string& reason = "t") {
        harbinger_rpc::NackRequest nq;
        nq.set_consumer_id(consumer_id);
        nq.set_message_id(message_id);
        nq.set_processing_time_ms(5);
        nq.set_reason(reason);
        harbinger_rpc::NackResponse nresp;
        grpc::ClientContext c;
        EXPECT_TRUE(stub->Nack(&c, nq, &nresp).ok());
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
    b.nack(pulled.message().message_id(), "late-failure");

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
    b.ack(pulled.message().message_id()); // still OK to the caller

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

    b.nack(pulled.message().message_id(), "transient");

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
