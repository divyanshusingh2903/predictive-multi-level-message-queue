#include "harbinger_service.hpp"

#include <algorithm>
#include <stdexcept>

namespace harbinger {

namespace {

HarbingerConfig validate_config(HarbingerConfig config) {
    if (config.num_levels == 0) {
        throw std::invalid_argument("HarbingerConfig: num_levels must be >= 1");
    }
    if (config.default_priority >= config.num_levels) {
        throw std::invalid_argument(
            "HarbingerConfig: default_priority must be < num_levels");
    }
    if (config.default_max_retries == 0) {
        throw std::invalid_argument(
            "HarbingerConfig: default_max_retries must be >= 1");
    }
    if (config.default_ttl.count() < 0) {
        throw std::invalid_argument("HarbingerConfig: default_ttl must be >= 0");
    }
    if (config.max_pull_wait <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument(
            "HarbingerConfig: max_pull_wait must be positive");
    }
    if (config.ttl_sweep_interval.count() < 0) {
        throw std::invalid_argument(
            "HarbingerConfig: ttl_sweep_interval must be >= 0");
    }
    if (config.aging &&
        (config.aging->threshold <= std::chrono::milliseconds::zero() ||
         config.aging->interval <= std::chrono::milliseconds::zero())) {
        throw std::invalid_argument(
            "HarbingerConfig: aging threshold and interval must be positive");
    }
    return config;
}

} // namespace

HarbingerService::HarbingerService(HarbingerConfig config)
    : config_(validate_config(std::move(config))),
      queue_(config_.num_levels, config_.aging),
      dlq_(),
      proxy_([this](Message msg) { route_message(std::move(msg)); }) {
    if (config_.ttl_sweep_interval.count() > 0) {
        sweeper_thread_ = std::thread([this] { run_ttl_sweeper(); });
    }
}

HarbingerService::~HarbingerService() {
    stop_sweeper_.store(true, std::memory_order_release);
    sweeper_cv_.notify_all();
    if (sweeper_thread_.joinable()) {
        sweeper_thread_.join();
    }
}

void HarbingerService::run_ttl_sweeper() {
    while (!stop_sweeper_.load(std::memory_order_acquire)) {
        std::unique_lock lock{sweeper_mutex_};
        sweeper_cv_.wait_for(lock, config_.ttl_sweep_interval, [this] {
            return stop_sweeper_.load(std::memory_order_acquire);
        });
        if (stop_sweeper_.load(std::memory_order_acquire)) break;
        lock.unlock();
        dlq_swept(queue_.sweep_expired(), "TTL expired in queue (sweeper)");
    }
}

void HarbingerService::dlq_swept(std::vector<Message> expired,
                             std::string details) {
    for (auto& msg : expired) {
        dlq_.push(std::move(msg), DLQReason::TTL_EXPIRED, details);
    }
}

bool HarbingerService::is_registered_producer(const std::string& id) const {
    std::lock_guard lock{producers_mutex_};
    return registered_producers_.contains(id);
}

bool HarbingerService::is_registered_consumer(const std::string& id) const {
    std::lock_guard lock{consumers_mutex_};
    return registered_consumers_.contains(id);
}

std::size_t HarbingerService::in_flight_count() const {
    std::lock_guard lock{in_flight_mutex_};
    return in_flight_.size();
}

void HarbingerService::route_message(Message msg) {
    msg.priority          = config_.default_priority;
    msg.original_priority = config_.default_priority;
    msg.max_retries       = config_.default_max_retries;
    if (msg.ttl == kTtlUnset) {
        msg.ttl = config_.default_ttl;
    }
    queue_.enqueue(std::move(msg));
}

grpc::Status HarbingerService::RegisterProducer(
    grpc::ServerContext*,
    const harbinger_rpc::RegisterProducerRequest*,
    harbinger_rpc::RegisterProducerResponse* resp) {
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

grpc::Status HarbingerService::Submit(grpc::ServerContext*,
                                   const harbinger_rpc::SubmitRequest* req,
                                   harbinger_rpc::SubmitResponse* resp) {
    if (!is_registered_producer(req->producer_id())) {
        return {grpc::StatusCode::PERMISSION_DENIED,
                "Unknown producer: " + req->producer_id()};
    }

    const auto& raw = req->payload();
    std::vector<uint8_t> payload(raw.begin(), raw.end());

    std::optional<std::chrono::milliseconds> ttl;
    if (req->has_ttl_ms()) {
        if (req->ttl_ms() < 0) {
            return {grpc::StatusCode::INVALID_ARGUMENT,
                    "ttl_ms must be >= 0"};
        }
        ttl = std::chrono::milliseconds(req->ttl_ms());
    }

    std::unordered_map<std::string, std::string> headers(req->headers().begin(),
                                                          req->headers().end());
    const std::string msg_id = proxy_.accept(
        std::move(payload), std::move(headers), req->producer_id(), ttl);
    resp->set_message_id(msg_id);
    return grpc::Status::OK;
}

grpc::Status HarbingerService::RegisterConsumer(
    grpc::ServerContext*,
    const harbinger_rpc::RegisterConsumerRequest*,
    harbinger_rpc::RegisterConsumerResponse* resp) {
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

grpc::Status HarbingerService::Pull(grpc::ServerContext* ctx,
                                 const harbinger_rpc::PullRequest* req,
                                 harbinger_rpc::PullResponse* resp) {
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
    constexpr std::chrono::milliseconds kChunk{100};

    while (std::chrono::steady_clock::now() < deadline) {
        if (ctx->IsCancelled()) {
            return grpc::Status::CANCELLED;
        }

        // Reclaim expired messages in bulk so a backlog of dead messages
        // cannot burn the whole Pull deadline one dequeue at a time.
        dlq_swept(queue_.sweep_expired(), "TTL expired in queue");

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        auto msg_opt = queue_.dequeue(std::min(remaining, kChunk));

        if (!msg_opt) continue;

        auto& msg = *msg_opt;

        if (msg.is_expired()) {
            dlq_.push(std::move(msg), DLQReason::TTL_EXPIRED,
                      "TTL expired at Pull time");
            continue;
        }

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

grpc::Status HarbingerService::Ack(grpc::ServerContext*,
                                const harbinger_rpc::AckRequest* req,
                                harbinger_rpc::AckResponse*) {
    if (!is_registered_consumer(req->consumer_id())) {
        return {grpc::StatusCode::PERMISSION_DENIED,
                "Unknown consumer: " + req->consumer_id()};
    }

    // TTL guards queue wait, but the message expired mid-processing: the
    // work completed, so report success while retaining DLQ accounting.
    std::optional<Message> expired;
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
        if (it->second.message.is_expired()) {
            expired = std::move(it->second.message);
        }
        in_flight_.erase(it);
    }
    if (expired) {
        dlq_.push(std::move(*expired), DLQReason::TTL_EXPIRED,
                  "TTL expired before Ack");
    }
    return grpc::Status::OK;
}

grpc::Status HarbingerService::Nack(grpc::ServerContext*,
                                 const harbinger_rpc::NackRequest* req,
                                 harbinger_rpc::NackResponse*) {
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
        msg_to_handle = std::move(it->second.message);
        in_flight_.erase(it);
    }

    auto& msg = *msg_to_handle;

    // Expiry beats retry accounting: a dead message must not burn a retry
    // round-trip, inflate retry_count, or misreport as MAX_RETRIES_EXCEEDED.
    if (msg.is_expired()) {
        const auto retries = msg.retry_count;
        dlq_.push(std::move(msg), DLQReason::TTL_EXPIRED,
                  "TTL expired before Nack; retry_count=" +
                      std::to_string(retries) +
                      "; last reason: " + req->reason());
        return grpc::Status::OK;
    }
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

namespace {

harbinger_rpc::DlqReason to_proto_reason(DLQReason reason) {
    switch (reason) {
        case DLQReason::MAX_RETRIES_EXCEEDED:
            return harbinger_rpc::DLQ_MAX_RETRIES_EXCEEDED;
        case DLQReason::TTL_EXPIRED:
            return harbinger_rpc::DLQ_TTL_EXPIRED;
        case DLQReason::PROCESSING_ERROR:
            return harbinger_rpc::DLQ_PROCESSING_ERROR;
    }
    return harbinger_rpc::DLQ_UNKNOWN;
}

} // namespace

grpc::Status HarbingerService::InspectDlq(grpc::ServerContext*,
                                      const harbinger_rpc::InspectDlqRequest* req,
                                      harbinger_rpc::InspectDlqResponse* resp) {
    constexpr int kDefaultLimit = 10;
    constexpr int kMaxLimit = 100;
    const int limit =
        std::clamp(req->limit() <= 0 ? kDefaultLimit : req->limit(), 1,
                   kMaxLimit);
    const std::size_t offset =
        static_cast<std::size_t>(std::max(req->offset(), 0));

    const auto entries =
        dlq_.snapshot(offset, static_cast<std::size_t>(limit));
    const auto now = std::chrono::steady_clock::now();
    for (const auto& entry : entries) {
        auto* out = resp->add_entries();
        out->set_message_id(entry.message.id);
        out->set_payload(std::string(entry.message.payload.begin(),
                                     entry.message.payload.end()));
        for (const auto& [k, v] : entry.message.headers) {
            (*out->mutable_headers())[k] = v;
        }
        out->set_reason(to_proto_reason(entry.reason));
        out->set_details(entry.details);
        out->set_dlq_age_ms(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - entry.dlq_time)
                .count());
        out->set_retry_count(entry.message.retry_count);
    }
    resp->set_total_size(
        static_cast<uint64_t>(dlq_.size()));
    return grpc::Status::OK;
}

} // namespace harbinger
