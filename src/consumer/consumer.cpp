#include "consumer/consumer.hpp"

#include <grpcpp/grpcpp.h>

#include <stdexcept>
#include <utility>

namespace harbinger {

Consumer::Consumer(std::string id,
                   std::unique_ptr<harbinger_rpc::Broker::Stub> stub,
                   Handler handler,
                   std::chrono::milliseconds pull_timeout)
    : id_(std::move(id)),
      stub_(std::move(stub)),
      handler_(std::move(handler)),
      pull_timeout_(pull_timeout) {
    if (!handler_) {
        throw std::invalid_argument("Consumer: handler must not be empty");
    }
    if (pull_timeout_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Consumer: pull_timeout must be positive");
    }
}

std::shared_ptr<Consumer> Consumer::connect(const std::string& server_addr,
                                             Handler handler,
                                             std::chrono::milliseconds pull_timeout) {
    auto channel = grpc::CreateChannel(server_addr,
                                       grpc::InsecureChannelCredentials());
    auto stub = harbinger_rpc::Broker::NewStub(channel);

    harbinger_rpc::RegisterConsumerRequest  req;
    harbinger_rpc::RegisterConsumerResponse resp;
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
        harbinger_rpc::PullRequest pull_req;
        pull_req.set_consumer_id(id_);
        pull_req.set_timeout_ms(
            static_cast<int64_t>(pull_timeout_.count()));

        harbinger_rpc::PullResponse pull_resp;
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
        AckResult result = AckResult::FAILURE;
        std::string failure_reason = "handler_returned_failure";
        try {
            result = handler_(msg);
        } catch (const std::exception& ex) {
            failure_reason = std::string{"handler_exception: "} + ex.what();
        } catch (...) {
            failure_reason = "handler_exception: unknown exception";
        }
        const auto processing_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();

        // ── Ack or Nack — always carry actual processing time (Phase 2 data) ─
        if (result == AckResult::SUCCESS) {
            harbinger_rpc::AckRequest ack_req;
            ack_req.set_consumer_id(id_);
            ack_req.set_message_id(msg.id);
            ack_req.set_processing_time_ms(processing_ms);

            harbinger_rpc::AckResponse  ack_resp;
            grpc::ClientContext     ack_ctx;
            ack_ctx.set_deadline(std::chrono::system_clock::now() +
                                 std::chrono::seconds{5});
            const auto status = stub_->Ack(&ack_ctx, ack_req, &ack_resp);
            if (!status.ok()) {
                rpc_failures_.fetch_add(1, std::memory_order_relaxed);
                running_.store(false, std::memory_order_release);
                continue;
            }
            acked_.fetch_add(1, std::memory_order_relaxed);
        } else {
            harbinger_rpc::NackRequest nack_req;
            nack_req.set_consumer_id(id_);
            nack_req.set_message_id(msg.id);
            nack_req.set_processing_time_ms(processing_ms);
            nack_req.set_reason(failure_reason);

            harbinger_rpc::NackResponse nack_resp;
            grpc::ClientContext     nack_ctx;
            nack_ctx.set_deadline(std::chrono::system_clock::now() +
                                  std::chrono::seconds{5});
            const auto status = stub_->Nack(&nack_ctx, nack_req, &nack_resp);
            if (!status.ok()) {
                rpc_failures_.fetch_add(1, std::memory_order_relaxed);
                running_.store(false, std::memory_order_release);
                continue;
            }
            nacked_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

} // namespace harbinger
