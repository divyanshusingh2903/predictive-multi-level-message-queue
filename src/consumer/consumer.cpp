#include "consumer/consumer.hpp"

#include <grpcpp/grpcpp.h>

#include <stdexcept>

namespace pmlmq {

Consumer::Consumer(std::string id,
                   std::unique_ptr<pmlmq_rpc::Broker::Stub> stub,
                   Handler handler,
                   std::chrono::milliseconds pull_timeout)
    : id_(std::move(id)),
      stub_(std::move(stub)),
      handler_(std::move(handler)),
      pull_timeout_(pull_timeout) {}

std::shared_ptr<Consumer> Consumer::connect(const std::string& server_addr,
                                             Handler handler,
                                             std::chrono::milliseconds pull_timeout) {
    auto channel = grpc::CreateChannel(server_addr,
                                       grpc::InsecureChannelCredentials());
    auto stub = pmlmq_rpc::Broker::NewStub(channel);

    pmlmq_rpc::RegisterConsumerRequest  req;
    pmlmq_rpc::RegisterConsumerResponse resp;
    grpc::ClientContext ctx;

    const auto status = stub->RegisterConsumer(&ctx, req, &resp);
    if (!status.ok()) {
        throw std::runtime_error(
            "Consumer::connect failed: " + status.error_message());
    }

    return std::shared_ptr<Consumer>(
        new Consumer(resp.consumer_id(), std::move(stub),
                     std::move(handler), pull_timeout));
}

void Consumer::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
        throw std::runtime_error("Consumer '" + id_ + "' is already running");
    }
    worker_ = std::thread([this] { run(); });
}

void Consumer::stop() {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Consumer::run() {
    while (running_.load(std::memory_order_acquire)) {
        // ── Pull ─────────────────────────────────────────────────────────────
        pmlmq_rpc::PullRequest pull_req;
        pull_req.set_consumer_id(id_);
        pull_req.set_timeout_ms(
            static_cast<int64_t>(pull_timeout_.count()));

        pmlmq_rpc::PullResponse pull_resp;
        grpc::ClientContext ctx;
        // Give the gRPC call a deadline slightly beyond the server-side wait
        // so the network round-trip doesn't cause spurious deadline exceeded errors.
        ctx.set_deadline(
            std::chrono::system_clock::now() + pull_timeout_ +
            std::chrono::milliseconds{500});

        const auto pull_status = stub_->Pull(&ctx, pull_req, &pull_resp);

        if (!pull_status.ok()) {
            if (!running_.load(std::memory_order_acquire)) break;
            // Transient connection error — back off briefly before retrying.
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            continue;
        }

        if (pull_resp.timed_out()) {
            continue; // no message in time — loop and re-check running flag
        }

        // ── Convert proto message → ReceivedMessage ───────────────────────────
        const auto& pulled = pull_resp.message();
        ReceivedMessage msg;
        msg.id      = pulled.message_id();
        msg.payload = std::vector<uint8_t>(pulled.payload().begin(),
                                           pulled.payload().end());
        for (const auto& [k, v] : pulled.headers()) {
            msg.headers[k] = v;
        }

        // ── Invoke handler and measure actual processing time ─────────────────
        processed_.fetch_add(1, std::memory_order_relaxed);

        const auto t0 = std::chrono::steady_clock::now();
        const AckResult result = handler_(msg);
        const auto processing_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();

        // ── Ack or Nack — always carry actual processing time (Phase 2 data) ─
        if (result == AckResult::SUCCESS) {
            pmlmq_rpc::AckRequest ack_req;
            ack_req.set_consumer_id(id_);
            ack_req.set_message_id(msg.id);
            ack_req.set_processing_time_ms(processing_ms);

            pmlmq_rpc::AckResponse  ack_resp;
            grpc::ClientContext     ack_ctx;
            stub_->Ack(&ack_ctx, ack_req, &ack_resp);
            acked_.fetch_add(1, std::memory_order_relaxed);
        } else {
            pmlmq_rpc::NackRequest nack_req;
            nack_req.set_consumer_id(id_);
            nack_req.set_message_id(msg.id);
            nack_req.set_processing_time_ms(processing_ms);
            nack_req.set_reason("handler_returned_failure");

            pmlmq_rpc::NackResponse nack_resp;
            grpc::ClientContext     nack_ctx;
            stub_->Nack(&nack_ctx, nack_req, &nack_resp);
            nacked_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

} // namespace pmlmq
