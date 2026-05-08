#include "pmlmq_service.hpp"

#include <stdexcept>

namespace pmlmq {

PMLMQService::PMLMQService(PMLMQConfig config)
    : config_(std::move(config)),
      queue_(config_.num_levels, config_.aging),
      dlq_(),
      proxy_([this](Message msg) { route_message(std::move(msg)); }) {
    if (config_.default_priority >= config_.num_levels) {
        throw std::invalid_argument(
            "PMLMQConfig: default_priority must be < num_levels");
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

bool PMLMQService::is_registered_producer(const std::string& id) const {
    std::lock_guard lock{producers_mutex_};
    return registered_producers_.contains(id);
}

bool PMLMQService::is_registered_consumer(const std::string& id) const {
    std::lock_guard lock{consumers_mutex_};
    return registered_consumers_.contains(id);
}

std::size_t PMLMQService::in_flight_count() const {
    std::lock_guard lock{in_flight_mutex_};
    return in_flight_.size();
}

void PMLMQService::route_message(Message msg) {
    // Phase 1: assign static context from system config.
    // Phase 2: invoke ML classifier here to predict priority.
    msg.priority          = config_.default_priority;
    msg.original_priority = config_.default_priority;
    msg.max_retries       = config_.default_max_retries;
    if (msg.ttl.count() == 0) {
        msg.ttl = config_.default_ttl;
    }
    queue_.enqueue(std::move(msg));
}

// ── Producer RPCs ─────────────────────────────────────────────────────────────

grpc::Status PMLMQService::RegisterProducer(
    grpc::ServerContext*,
    const pmlmq_rpc::RegisterProducerRequest*,
    pmlmq_rpc::RegisterProducerResponse* resp) {
    const std::string id =
        "producer-" +
        std::to_string(producer_counter_.fetch_add(1, std::memory_order_relaxed));
    {
        std::lock_guard lock{producers_mutex_};
        registered_producers_.insert(id);
    }
    resp->set_producer_id(id);
    return grpc::Status::OK;
}

grpc::Status PMLMQService::Submit(grpc::ServerContext*,
                                   const pmlmq_rpc::SubmitRequest* req,
                                   pmlmq_rpc::SubmitResponse* resp) {
    if (!is_registered_producer(req->producer_id())) {
        return {grpc::StatusCode::PERMISSION_DENIED,
                "Unknown producer: " + req->producer_id()};
    }

    const auto& raw = req->payload();
    std::vector<uint8_t> payload(raw.begin(), raw.end());

    std::unordered_map<std::string, std::string> headers(req->headers().begin(),
                                                          req->headers().end());
    const std::string msg_id =
        proxy_.accept(std::move(payload), std::move(headers), req->producer_id());
    resp->set_message_id(msg_id);
    return grpc::Status::OK;
}

// ── Consumer RPCs ─────────────────────────────────────────────────────────────

grpc::Status PMLMQService::RegisterConsumer(
    grpc::ServerContext*,
    const pmlmq_rpc::RegisterConsumerRequest*,
    pmlmq_rpc::RegisterConsumerResponse* resp) {
    const std::string id =
        "consumer-" +
        std::to_string(consumer_counter_.fetch_add(1, std::memory_order_relaxed));
    {
        std::lock_guard lock{consumers_mutex_};
        registered_consumers_.insert(id);
    }
    resp->set_consumer_id(id);
    return grpc::Status::OK;
}

grpc::Status PMLMQService::Pull(grpc::ServerContext* ctx,
                                 const pmlmq_rpc::PullRequest* req,
                                 pmlmq_rpc::PullResponse* resp) {
    if (!is_registered_consumer(req->consumer_id())) {
        return {grpc::StatusCode::PERMISSION_DENIED,
                "Unknown consumer: " + req->consumer_id()};
    }

    // Determine effective wait duration.
    const auto requested =
        req->timeout_ms() > 0
            ? std::chrono::milliseconds(req->timeout_ms())
            : config_.max_pull_wait;
    const auto wait = std::min(requested, config_.max_pull_wait);
    const auto deadline = std::chrono::steady_clock::now() + wait;

    // Poll in short chunks so we can check for client cancellation.
    constexpr std::chrono::milliseconds kChunk{100};

    while (std::chrono::steady_clock::now() < deadline) {
        if (ctx->IsCancelled()) {
            return grpc::Status::CANCELLED;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        auto msg_opt = queue_.dequeue(std::min(remaining, kChunk));

        if (!msg_opt) continue; // timeout chunk — keep waiting

        auto& msg = *msg_opt;

        // Check TTL before dispatching to consumer.
        if (msg.is_expired()) {
            dlq_.push(std::move(msg), DLQReason::TTL_EXPIRED,
                      "TTL expired at Pull time");
            continue; // look for the next message
        }

        // Save copies before moving into the in-flight map.
        const std::string            msg_id  = msg.id;
        const std::vector<uint8_t>   payload = msg.payload;
        const auto                   headers = msg.headers;

        {
            std::lock_guard lock{in_flight_mutex_};
            in_flight_.emplace(msg_id, InFlightEntry{
                .message     = std::move(msg),
                .consumer_id = req->consumer_id(),
                .pull_time   = std::chrono::steady_clock::now(),
            });
        }

        // Build response.
        auto* pulled = resp->mutable_message();
        pulled->set_message_id(msg_id);
        pulled->set_payload(std::string(payload.begin(), payload.end()));
        for (const auto& [k, v] : headers) {
            (*pulled->mutable_headers())[k] = v;
        }
        resp->set_timed_out(false);
        return grpc::Status::OK;
    }

    resp->set_timed_out(true);
    return grpc::Status::OK;
}

grpc::Status PMLMQService::Ack(grpc::ServerContext*,
                                const pmlmq_rpc::AckRequest* req,
                                pmlmq_rpc::AckResponse*) {
    if (!is_registered_consumer(req->consumer_id())) {
        return {grpc::StatusCode::PERMISSION_DENIED,
                "Unknown consumer: " + req->consumer_id()};
    }

    std::lock_guard lock{in_flight_mutex_};
    auto it = in_flight_.find(req->message_id());
    if (it == in_flight_.end()) {
        return {grpc::StatusCode::NOT_FOUND,
                "Message not in flight: " + req->message_id()};
    }
    if (it->second.consumer_id != req->consumer_id()) {
        return {grpc::StatusCode::PERMISSION_DENIED,
                "Message owned by a different consumer"};
    }

    // Phase 2: feed req->processing_time_ms() into the ML feedback loop.
    in_flight_.erase(it);
    return grpc::Status::OK;
}

grpc::Status PMLMQService::Nack(grpc::ServerContext*,
                                 const pmlmq_rpc::NackRequest* req,
                                 pmlmq_rpc::NackResponse*) {
    if (!is_registered_consumer(req->consumer_id())) {
        return {grpc::StatusCode::PERMISSION_DENIED,
                "Unknown consumer: " + req->consumer_id()};
    }

    std::optional<Message> msg_to_handle;
    {
        std::lock_guard lock{in_flight_mutex_};
        auto it = in_flight_.find(req->message_id());
        if (it == in_flight_.end()) {
            return {grpc::StatusCode::NOT_FOUND,
                    "Message not in flight: " + req->message_id()};
        }
        if (it->second.consumer_id != req->consumer_id()) {
            return {grpc::StatusCode::PERMISSION_DENIED,
                    "Message owned by a different consumer"};
        }
        // Phase 2: feed req->processing_time_ms() into the ML feedback loop.
        msg_to_handle = std::move(it->second.message);
        in_flight_.erase(it);
    }

    auto& msg = *msg_to_handle;
    msg.retry_count++;

    if (msg.retry_count >= msg.max_retries) {
        dlq_.push(std::move(msg), DLQReason::MAX_RETRIES_EXCEEDED,
                  "Exceeded max_retries=" + std::to_string(msg.max_retries) +
                      "; last reason: " + req->reason());
    } else {
        msg.priority = msg.original_priority;
        queue_.enqueue(std::move(msg));
    }

    return grpc::Status::OK;
}

} // namespace pmlmq
