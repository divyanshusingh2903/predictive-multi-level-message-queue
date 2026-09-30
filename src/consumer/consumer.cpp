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
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds{5});

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
    std::unique_lock lock{lifecycle_mutex_};
    if (joining_ || running_.load()) {
        throw std::runtime_error("Consumer '" + id_ + "' is already running");
    }
    if (worker_.joinable()) {
        if (worker_id_ == std::this_thread::get_id())
            throw std::runtime_error("Cannot restart from handler");
        joining_ = true;
        auto old = std::move(worker_);
        lock.unlock();
        old.join();
        lock.lock();
        joining_ = false;
        lifecycle_cv_.notify_all();
    }
    last_status_ = grpc::Status::OK;
    running_.store(true);
    try {
        worker_ = std::thread([this] { run(); });
        worker_id_ = worker_.get_id();
    } catch (...) {
        running_.store(false);
        throw;
    }
}

void Consumer::stop() {
    std::unique_lock lock{lifecycle_mutex_};
    running_.store(false, std::memory_order_release);
    if (active_pull_) active_pull_->TryCancel();
    lifecycle_cv_.notify_all();
    if (worker_id_ == std::this_thread::get_id()) return;
    if (joining_) {
        lifecycle_cv_.wait(lock, [this] { return !joining_; });
        return;
    }
    if (worker_.joinable()) {
        joining_ = true;
        auto worker = std::move(worker_);
        lock.unlock();
        worker.join();
        lock.lock();
        worker_id_ = {};
        joining_ = false;
        lifecycle_cv_.notify_all();
    }
}

Consumer::~Consumer() { stop(); }

grpc::Status Consumer::last_rpc_status() const {
    std::lock_guard lock{lifecycle_mutex_};
    return last_status_;
}

void Consumer::fail(grpc::Status status) {
    std::lock_guard lock{lifecycle_mutex_};
    last_status_ = std::move(status);
    running_.store(false);
}

void Consumer::settlement_failed(grpc::Status status) {
    if (status.error_code() == grpc::StatusCode::FAILED_PRECONDITION) {
        leases_lost_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    fail(std::move(status));
}

namespace {
bool transient(const grpc::Status& status) {
    return status.error_code() == grpc::StatusCode::UNAVAILABLE ||
           status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED;
}

template <typename Call>
grpc::Status settle_with_retry(Call call, std::atomic<uint64_t>& failures) {
    grpc::Status status;
    for (int attempt = 0; attempt < 3; ++attempt) {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds{5});
        status = call(ctx);
        if (status.ok()) return status;
        ++failures;
        if (!transient(status) || attempt == 2) break;
        // A running handler's outcome is settled even when stop was requested.
        std::this_thread::sleep_for(std::chrono::milliseconds{100 * (attempt + 1)});
    }
    return status;
}
} // namespace

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

        {
            std::lock_guard lock{lifecycle_mutex_};
            if (!running_.load()) break;
            active_pull_ = &ctx;
        }
        const auto pull_status = stub_->Pull(&ctx, pull_req, &pull_resp);
        {
            std::lock_guard lock{lifecycle_mutex_};
            active_pull_ = nullptr;
        }

        if (!pull_status.ok()) {
            if (!running_.load(std::memory_order_acquire)) break;
            if (!transient(pull_status)) {
                fail(pull_status);
                break;
            }
            std::unique_lock lock{lifecycle_mutex_};
            lifecycle_cv_.wait_for(lock, std::chrono::milliseconds{200},
                                  [this] { return !running_.load(); });
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
        msg.headers.reserve(pulled.headers().size());
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
            ack_req.set_attempt_token(pulled.attempt_token());
            ack_req.set_processing_time_ms(processing_ms);

            harbinger_rpc::AckResponse  ack_resp;
            const auto status = settle_with_retry([&](grpc::ClientContext& ack_ctx) {
                return stub_->Ack(&ack_ctx, ack_req, &ack_resp);
            }, rpc_failures_);
            if (!status.ok()) {
                settlement_failed(status);
                continue;
            }
            acked_.fetch_add(1, std::memory_order_relaxed);
        } else {
            harbinger_rpc::NackRequest nack_req;
            nack_req.set_consumer_id(id_);
            nack_req.set_message_id(msg.id);
            nack_req.set_attempt_token(pulled.attempt_token());
            nack_req.set_processing_time_ms(processing_ms);
            nack_req.set_reason(failure_reason);

            harbinger_rpc::NackResponse nack_resp;
            const auto status = settle_with_retry([&](grpc::ClientContext& nack_ctx) {
                return stub_->Nack(&nack_ctx, nack_req, &nack_resp);
            }, rpc_failures_);
            if (!status.ok()) {
                settlement_failed(status);
                continue;
            }
            nacked_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

} // namespace harbinger
