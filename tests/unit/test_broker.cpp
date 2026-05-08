#include "pmlmq_service.hpp"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace pmlmq;
using namespace std::chrono_literals;

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
        stub_    = pmlmq_rpc::Broker::NewStub(channel_);
    }

    void TearDown() override {
        server_->Shutdown();
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    std::string register_producer() {
        pmlmq_rpc::RegisterProducerRequest  req;
        pmlmq_rpc::RegisterProducerResponse resp;
        grpc::ClientContext ctx;
        EXPECT_TRUE(stub_->RegisterProducer(&ctx, req, &resp).ok());
        return resp.producer_id();
    }

    std::string register_consumer() {
        pmlmq_rpc::RegisterConsumerRequest  req;
        pmlmq_rpc::RegisterConsumerResponse resp;
        grpc::ClientContext ctx;
        EXPECT_TRUE(stub_->RegisterConsumer(&ctx, req, &resp).ok());
        return resp.consumer_id();
    }

    std::string submit(const std::string& producer_id,
                       const std::string& payload = "hello",
                       std::unordered_map<std::string,std::string> headers = {}) {
        pmlmq_rpc::SubmitRequest req;
        req.set_producer_id(producer_id);
        req.set_payload(payload);
        for (const auto& [k, v] : headers) (*req.mutable_headers())[k] = v;

        pmlmq_rpc::SubmitResponse resp;
        grpc::ClientContext ctx;
        EXPECT_TRUE(stub_->Submit(&ctx, req, &resp).ok());
        return resp.message_id();
    }

    pmlmq_rpc::PullResponse pull(const std::string& consumer_id,
                                  int64_t timeout_ms = 500) {
        pmlmq_rpc::PullRequest req;
        req.set_consumer_id(consumer_id);
        req.set_timeout_ms(timeout_ms);

        pmlmq_rpc::PullResponse resp;
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + 3s);
        EXPECT_TRUE(stub_->Pull(&ctx, req, &resp).ok());
        return resp;
    }

    PMLMQService                             service_;
    std::unique_ptr<grpc::Server>            server_;
    std::shared_ptr<grpc::Channel>           channel_;
    std::unique_ptr<pmlmq_rpc::Broker::Stub> stub_;
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
    pmlmq_rpc::SubmitRequest req;
    req.set_producer_id("not-registered");
    req.set_payload("data");

    pmlmq_rpc::SubmitResponse resp;
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
    pmlmq_rpc::PullRequest req;
    req.set_consumer_id("ghost");
    req.set_timeout_ms(100);

    pmlmq_rpc::PullResponse resp;
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

    pmlmq_rpc::AckRequest ack;
    ack.set_consumer_id(cons);
    ack.set_message_id(pull_resp.message().message_id());
    ack.set_processing_time_ms(5);

    pmlmq_rpc::AckResponse ack_resp;
    grpc::ClientContext ctx;
    EXPECT_TRUE(stub_->Ack(&ctx, ack, &ack_resp).ok());
    EXPECT_EQ(service_.in_flight_count(), 0u);
    EXPECT_EQ(service_.dlq_size(), 0u);
}

TEST_F(BrokerTest, AckForUnknownMessageReturnsNotFound) {
    const auto cons = register_consumer();

    pmlmq_rpc::AckRequest ack;
    ack.set_consumer_id(cons);
    ack.set_message_id("nonexistent");

    pmlmq_rpc::AckResponse ack_resp;
    grpc::ClientContext ctx;
    EXPECT_EQ(stub_->Ack(&ctx, ack, &ack_resp).error_code(),
              grpc::StatusCode::NOT_FOUND);
}

// ── Nack ──────────────────────────────────────────────────────────────────────

TEST_F(BrokerTest, NackRequeuesMessage) {
    const auto prod = register_producer();
    const auto cons = register_consumer();
    submit(prod);
    const auto pull_resp = pull(cons, 500);
    ASSERT_FALSE(pull_resp.timed_out());

    pmlmq_rpc::NackRequest nack;
    nack.set_consumer_id(cons);
    nack.set_message_id(pull_resp.message().message_id());
    nack.set_processing_time_ms(10);
    nack.set_reason("test_nack");

    pmlmq_rpc::NackResponse nack_resp;
    grpc::ClientContext ctx;
    EXPECT_TRUE(stub_->Nack(&ctx, nack, &nack_resp).ok());

    EXPECT_EQ(service_.in_flight_count(), 0u);
    EXPECT_EQ(service_.queue_size(), 1u); // re-queued
}

TEST_F(BrokerTest, NackExceedingMaxRetriesSendsToDLQ) {
    // Use a config where max_retries=1 so one nack sends to DLQ.
    PMLMQService svc{ PMLMQConfig{ .default_max_retries = 1 } };
    int port = 0;
    grpc::ServerBuilder b;
    b.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    b.RegisterService(&svc);
    auto srv = b.BuildAndStart();
    auto ch  = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                   grpc::InsecureChannelCredentials());
    auto st  = pmlmq_rpc::Broker::NewStub(ch);

    // Register, submit, pull.
    { grpc::ClientContext c; pmlmq_rpc::RegisterProducerRequest rq; pmlmq_rpc::RegisterProducerResponse rs; st->RegisterProducer(&c, rq, &rs); auto pid = rs.producer_id();
      grpc::ClientContext c2; pmlmq_rpc::SubmitRequest sq; sq.set_producer_id(pid); sq.set_payload("x"); pmlmq_rpc::SubmitResponse ss; st->Submit(&c2, sq, &ss); }

    grpc::ClientContext c3; pmlmq_rpc::RegisterConsumerRequest rq2; pmlmq_rpc::RegisterConsumerResponse rs2; st->RegisterConsumer(&c3, rq2, &rs2); auto cid = rs2.consumer_id();

    pmlmq_rpc::PullRequest pr; pr.set_consumer_id(cid); pr.set_timeout_ms(500);
    pmlmq_rpc::PullResponse presp; grpc::ClientContext c4;
    c4.set_deadline(std::chrono::system_clock::now() + 2s);
    st->Pull(&c4, pr, &presp);
    ASSERT_FALSE(presp.timed_out());

    // Nack once → retry_count becomes 1 ≥ max_retries=1 → DLQ.
    pmlmq_rpc::NackRequest nq; nq.set_consumer_id(cid);
    nq.set_message_id(presp.message().message_id());
    pmlmq_rpc::NackResponse nr; grpc::ClientContext c5;
    st->Nack(&c5, nq, &nr);

    EXPECT_EQ(svc.dlq_size(), 1u);
    srv->Shutdown();
}
