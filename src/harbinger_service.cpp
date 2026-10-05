#include "harbinger_service.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <cctype>
#include <random>
#include <limits>
#include <type_traits>

namespace harbinger {

namespace {

void mark_transition(std::optional<ml::FeedbackEvent>& event) noexcept {
#ifdef HARBINGER_BENCHMARK_SUPPORT
    if (event && event->benchmark_observation) event->benchmark_observation->time_ns = benchmark::now_ns();
#else
    (void)event;
#endif
}

HarbingerConfig validate_config(HarbingerConfig config) {
#ifdef HARBINGER_BENCHMARK_SUPPORT
    if (config.benchmark_options) {
        const auto& options = *config.benchmark_options;
        if (!options.observations) throw std::invalid_argument("benchmark requires observations");
        for (auto tier : options.job_tiers)
            if (tier >= config.num_levels) throw std::invalid_argument("benchmark tier out of range");
        if (options.policy != benchmark::Policy::Disabled && options.policy != benchmark::Policy::Fifo &&
            options.policy != benchmark::Policy::Static && options.policy != benchmark::Policy::RoundRobin &&
            options.policy != benchmark::Policy::Oracle)
            throw std::invalid_argument("invalid benchmark policy");
        for (const auto& [name, tier] : options.static_tiers)
            if (tier >= config.num_levels) throw std::invalid_argument("benchmark static tier out of range");
        if (config.predictive_routing && config.predictive_routing->mode == ml::RoutingMode::Predictive &&
            options.policy != benchmark::Policy::Disabled)
            throw std::invalid_argument("predictive routing cannot be combined with a benchmark ingress policy");
    }
#endif
    if (config.predictive_routing) {
        auto& routing = *config.predictive_routing;
        if (routing.mode == ml::RoutingMode::Disabled)
            throw std::invalid_argument("predictive_routing mode must be shadow or predictive; omit it to disable");
        ml::validate_version(routing.routing_policy_version);
        if (routing.snapshot_path && (routing.snapshot_path->empty() || routing.snapshot_interval.count() <= 0))
            throw std::invalid_argument("predictor snapshot needs a path and a positive interval");
        if (routing.key.job_header.starts_with("__"))
            throw std::invalid_argument("predictor job header must not be a reserved __ header");
        routing.predictor.num_levels = config.num_levels;
        routing.predictor.default_priority = config.default_priority;
    }
    if (config.feedback && !config.ingress_features)
        throw std::invalid_argument("feedback requires ingress_features");
    if (config.ingress_features) {
        ml::validate_version(config.ingress_features->routing_policy_version);
    }
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
    if (config.delivery_lease.count() <= 0 || config.lease_sweep_interval.count() <= 0 ||
        config.completion_retention.count() <= 0 || !config.completion_cache_max_entries ||
        !config.maintenance_batch_size) {
        throw std::invalid_argument("delivery maintenance settings must be positive");
    }
    const auto max_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::duration::max() / 2);
    if (config.delivery_lease > max_duration || config.lease_sweep_interval > max_duration ||
        config.completion_retention > max_duration || config.ttl_sweep_interval > max_duration)
        throw std::invalid_argument("maintenance duration too large");
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
      feature_extractor_(config_.ingress_features
          ? std::make_optional<ml::FeatureExtractor>(config_.ingress_features->schema) : std::nullopt),
      feedback_writer_(config_.feedback
          ? std::make_unique<ml::FeedbackWriter>(*config_.feedback, config_.delivery_lease.count()) : nullptr),
      queue_(config_.num_levels, config_.aging
#ifdef HARBINGER_BENCHMARK_SUPPORT
          , config_.benchmark_options && config_.benchmark_options->policy == benchmark::Policy::RoundRobin
              ? QueueSelection::RoundRobin : QueueSelection::StrictPriority
#endif
      ),
      dlq_(),
      proxy_([this](Message msg) { route_message(std::move(msg)); }),
      instance_token_(std::to_string(std::random_device{}()) + "-" + Proxy::generate_id()) {
    if (config_.predictive_routing) {
        const auto& routing = *config_.predictive_routing;
        predictor_ = std::make_unique<ml::PerKeyPredictor>(routing.predictor);
        snapshot_context_ = routing.routing_policy_version + "|key=" + routing.key.job_header +
            (routing.key.scope_by_producer ? "|scoped" : "|unscoped");
        if (routing.snapshot_path) {
            std::error_code missing;
            if (!std::filesystem::exists(*routing.snapshot_path, missing)) {
                routing_.snapshot_status = "absent; starting cold";
            } else {
                try {
                    const auto restored = predictor_->load(*routing.snapshot_path, snapshot_context_);
                    routing_.snapshot_loaded = true;
                    routing_.snapshot_status = "restored " + std::to_string(restored) + " keys";
                } catch (const std::exception& error) {
                    // A bad snapshot never blocks startup or leaks into routing: the predictor simply starts cold.
                    predictor_ = std::make_unique<ml::PerKeyPredictor>(routing.predictor);
                    routing_.snapshot_status = std::string("rejected; starting cold: ") + error.what();
                }
            }
        }
    }
    sweeper_thread_ = std::thread([this] { run_maintenance(); });
}

HarbingerService::~HarbingerService() {
    {
        std::lock_guard lock{sweeper_mutex_};
        stop_sweeper_.store(true, std::memory_order_release);
    }
    sweeper_cv_.notify_all();
    if (sweeper_thread_.joinable()) {
        sweeper_thread_.join();
    }
    if (predictor_ && config_.predictive_routing->snapshot_path) save_predictor_snapshot();
    if (feedback_writer_) feedback_writer_->close();
}

#ifdef HARBINGER_BENCHMARK_SUPPORT
void HarbingerService::benchmark_close_feedback() {
    {
        std::lock_guard lock{sweeper_mutex_};
        stop_sweeper_.store(true, std::memory_order_release);
    }
    sweeper_cv_.notify_all();
    if (sweeper_thread_.joinable()) sweeper_thread_.join();
    if (feedback_writer_) feedback_writer_->close();
}
#endif

void HarbingerService::run_maintenance() {
    auto next_lease = Clock::now() + config_.lease_sweep_interval;
    auto next_ttl = config_.ttl_sweep_interval.count() > 0
        ? Clock::now() + config_.ttl_sweep_interval : Clock::time_point::max();
    const bool snapshots = predictor_ && config_.predictive_routing->snapshot_path;
    auto next_snapshot = snapshots ? Clock::now() + config_.predictive_routing->snapshot_interval
                                   : Clock::time_point::max();
    while (!stop_sweeper_.load(std::memory_order_acquire)) {
        std::unique_lock lock{sweeper_mutex_};
        sweeper_cv_.wait_until(lock, std::min({next_lease, next_ttl, next_snapshot}), [this] {
            return stop_sweeper_.load(std::memory_order_acquire);
        });
        if (stop_sweeper_.load(std::memory_order_acquire)) break;
        lock.unlock();
        const auto now = Clock::now();
        if (now >= next_lease) {
            maintain_deliveries();
            next_lease = Clock::now() + config_.lease_sweep_interval;
        }
        if (now >= next_ttl) {
            dlq_swept(queue_.sweep_expired_batch(config_.maintenance_batch_size),
                      "TTL expired in queue (sweeper)");
            next_ttl = Clock::now() + config_.ttl_sweep_interval;
        }
        if (now >= next_snapshot) {
            save_predictor_snapshot();  // predictor locks only; no queue, settlement or feedback lock is held
            next_snapshot = Clock::now() + config_.predictive_routing->snapshot_interval;
        }
    }
}

void HarbingerService::dlq_swept(std::vector<Message> expired,
                             std::string details) {
    for (auto& msg : expired) {
        auto event = capture_event(msg, ml::FeedbackTrigger::TtlSweep);
        if (event) { event->outcome = ml::FeedbackOutcome::Dlq; event->dlq_reason = DLQReason::TTL_EXPIRED; }
        dlq_.push(std::move(msg), DLQReason::TTL_EXPIRED, details);
        mark_transition(event);
        publish_event(event);
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

ml::FeedbackStats HarbingerService::feedback_stats() const noexcept {
    return feedback_writer_ ? feedback_writer_->stats() : ml::FeedbackStats{};
}

std::optional<ml::FeedbackEvent> HarbingerService::capture_event(
    const Message& message, ml::FeedbackTrigger trigger) noexcept {
    if (!feedback_writer_
#ifdef HARBINGER_BENCHMARK_SUPPORT
        && !config_.benchmark_options
#endif
    ) return std::nullopt;
    try {
        auto event = ml::capture_feedback(message, trigger);
#ifdef HARBINGER_BENCHMARK_SUPPORT
        if (config_.benchmark_options) {
            event.benchmark_observation = benchmark::capture(message, benchmark::Kind::Ingress);
            if (!event.benchmark_observation)
                config_.benchmark_options->observations->drop();
        }
#endif
        return event;
    }
    catch (...) {
        if (feedback_writer_) feedback_writer_->record_drop(ml::FeedbackDrop::Capture);
#ifdef HARBINGER_BENCHMARK_SUPPORT
        if (config_.benchmark_options) config_.benchmark_options->observations->drop();
#endif
        return std::nullopt;
    }
}

void HarbingerService::publish_event(const std::optional<ml::FeedbackEvent>& event) noexcept {
    if (!event) return;
    if (feedback_writer_) feedback_writer_->publish(*event);
#ifdef HARBINGER_BENCHMARK_SUPPORT
    if (event->benchmark_observation && config_.benchmark_options) {
        auto record = *event->benchmark_observation;
        record.retries = event->retry_count;
        if (event->trigger != ml::FeedbackTrigger::Submit) {
            record.kind = event->dlq_reason == DLQReason::TTL_EXPIRED ? benchmark::Kind::Ttl
                : event->dlq_reason ? benchmark::Kind::MaxRetries
                : event->outcome == ml::FeedbackOutcome::Retry ? benchmark::Kind::Retry : benchmark::Kind::Ack;
        }
        config_.benchmark_options->observations->publish(record);
    }
#endif
}

ml::RoutingStats HarbingerService::routing_stats() const {
    ml::RoutingStats out;
    if (!predictor_) return out;
    out.enabled = true;
    out.mode = config_.predictive_routing->mode;
    out.lookups = routing_.lookups.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < out.outcomes.size(); ++i) out.outcomes[i] = routing_.outcomes[i].load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < out.latency_buckets.size(); ++i)
        out.latency_buckets[i] = routing_.latency[i].load(std::memory_order_relaxed);
    out.routed_by_prediction = routing_.routed.load(std::memory_order_relaxed);
    out.latency_ns_total = routing_.latency_total.load(std::memory_order_relaxed);
    out.latency_ns_max = routing_.latency_max.load(std::memory_order_relaxed);
    out.learned = routing_.learned.load(std::memory_order_relaxed);
    out.learn_rejected = routing_.learn_rejected.load(std::memory_order_relaxed);
    out.censored_observed = routing_.censored.load(std::memory_order_relaxed);
    out.snapshot_saves = routing_.snapshot_saves.load(std::memory_order_relaxed);
    out.snapshot_failures = routing_.snapshot_failures.load(std::memory_order_relaxed);
    {
        std::lock_guard lock{routing_.drift_mutex};
        out.scored = routing_.scored;
        out.tier_matches = routing_.matches;
        out.abs_log2_error_ewma = routing_.error_ewma;
        out.drift_alerts = routing_.drift_alerts;
        out.snapshot_loaded = routing_.snapshot_loaded;
        out.snapshot_status = routing_.snapshot_status;
    }
    out.predictor = predictor_->stats();
    return out;
}

bool HarbingerService::save_predictor_snapshot() noexcept {
    if (!predictor_ || !config_.predictive_routing->snapshot_path) return false;
    try {
        predictor_->save(*config_.predictive_routing->snapshot_path, snapshot_context_);
        routing_.snapshot_saves.fetch_add(1, std::memory_order_relaxed);
        return true;
    } catch (...) {
        routing_.snapshot_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

uint8_t HarbingerService::predict_route(Message& msg, ml::RoutingContext& context) noexcept {
    const auto& routing = *config_.predictive_routing;
    context.mode = routing.mode;
    context.routing_policy_version = routing.routing_policy_version;
    try {
        context.predictor_key = ml::derive_predictor_key(msg.headers, routing.key);
        context.model_version = predictor_->model_version();
    } catch (...) {
        context.predictor_key.reset();
    }
    const auto start = Clock::now();
    ml::DurationPrediction prediction{std::nullopt, config_.default_priority, ml::PredictionStatus::InvalidKey, 0};
    if (context.predictor_key) {
        try { prediction = predictor_->predict(*context.predictor_key); }
        catch (...) { prediction.status = ml::PredictionStatus::InvalidKey; }
    }
    const auto elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    context.inference_elapsed_ms = static_cast<double>(elapsed) / 1e6;

    routing_.lookups.fetch_add(1, std::memory_order_relaxed);
    routing_.latency_total.fetch_add(elapsed, std::memory_order_relaxed);
    for (auto seen = routing_.latency_max.load(std::memory_order_relaxed);
         elapsed > seen && !routing_.latency_max.compare_exchange_weak(seen, elapsed, std::memory_order_relaxed);) {}
    std::size_t bucket = 0;
    for (uint64_t limit = 1000; bucket + 1 < ml::kLatencyBuckets && elapsed >= limit; limit *= 10) ++bucket;
    routing_.latency[bucket].fetch_add(1, std::memory_order_relaxed);
    routing_.outcomes[static_cast<std::size_t>(prediction.status)].fetch_add(1, std::memory_order_relaxed);

    using ml::PredictionStatus;
    if (prediction.status == PredictionStatus::Predicted && prediction.bucket >= config_.num_levels)
        prediction.status = PredictionStatus::InvalidKey, context.fallback_reason = ml::FallbackReason::InvalidPrediction;
    switch (prediction.status) {
        case PredictionStatus::Predicted:
            context.predicted_processing_time_ms = prediction.estimate_ms;
            context.predicted_bucket = prediction.bucket;
            break;
        case PredictionStatus::Unready: context.fallback_reason = ml::FallbackReason::Unready; break;
        case PredictionStatus::ColdKey: context.fallback_reason = ml::FallbackReason::ColdKey; break;
        case PredictionStatus::HighSpread: context.fallback_reason = ml::FallbackReason::HighSpread; break;
        case PredictionStatus::Censored: context.fallback_reason = ml::FallbackReason::Censored; break;
        case PredictionStatus::StaleKey: context.fallback_reason = ml::FallbackReason::StaleKey; break;
        case PredictionStatus::InvalidKey:
            if (!context.fallback_reason) context.fallback_reason = ml::FallbackReason::InvalidKey;
            break;
    }
    if (routing.mode == ml::RoutingMode::Predictive && context.predicted_bucket) {
        routing_.routed.fetch_add(1, std::memory_order_relaxed);
        return *context.predicted_bucket;
    }
    return msg.priority;
}

void HarbingerService::learn(const PredictorUpdate& update) noexcept {
    try {
        if (update.censored) {
            if (predictor_->observe_censored(update.key)) routing_.censored.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!update.duration_ms || !predictor_->observe(update.key, *update.duration_ms)) {
            routing_.learn_rejected.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        routing_.learned.fetch_add(1, std::memory_order_relaxed);
        if (!update.predicted_ms || !update.predicted_bucket) return;
        const auto snapshot = predictor_->snapshot();
        const bool match = snapshot && ml::tier_of(snapshot->boundaries_ms, *update.duration_ms) == *update.predicted_bucket;
        // Error in octaves; 1 ms floor matches the wire resolution so zero-duration jobs stay finite.
        const double error = std::abs(std::log2(std::max(*update.predicted_ms, 1.0) / std::max(*update.duration_ms, 1.0)));
        std::lock_guard lock{routing_.drift_mutex};
        ++routing_.scored;
        routing_.matches += match;
        routing_.error_ewma = routing_.scored == 1 ? error : routing_.error_ewma + 0.01 * (error - routing_.error_ewma);
        auto& slot = routing_.window[routing_.window_next];
        if (routing_.window_size == routing_.window.size()) routing_.window_hits -= slot;
        else ++routing_.window_size;
        slot = match;
        routing_.window_hits += match;
        routing_.window_next = (routing_.window_next + 1) % routing_.window.size();
        if (routing_.window_size == routing_.window.size()) {
            const double agreement = static_cast<double>(routing_.window_hits) / static_cast<double>(routing_.window_size);
            if (!routing_.drift_latched && agreement < 0.5) { routing_.drift_latched = true; ++routing_.drift_alerts; }
            else if (routing_.drift_latched && agreement > 0.6) routing_.drift_latched = false;
        }
    } catch (...) {
        routing_.learn_rejected.fetch_add(1, std::memory_order_relaxed);
    }
}

void HarbingerService::route_message(Message msg) {
    msg.priority          = config_.default_priority;
#ifdef HARBINGER_BENCHMARK_SUPPORT
    if (config_.benchmark_options) {
        const auto& options = *config_.benchmark_options;
        if (options.policy == benchmark::Policy::Fifo) msg.priority = 0;
        else if (options.policy != benchmark::Policy::Disabled) {
            if (options.policy == benchmark::Policy::Oracle) {
                const auto tier = msg.headers.find(benchmark::kOracleTierHeader);
                if (tier != msg.headers.end() && tier->second.size() == 1 && tier->second[0] >= '0' &&
                    static_cast<uint8_t>(tier->second[0] - '0') < config_.num_levels)
                    msg.priority = static_cast<uint8_t>(tier->second[0] - '0');
            } else if (!options.static_tiers.empty()) {
                const auto job = msg.headers.find(options.static_header);
                if (job != msg.headers.end()) {
                    const auto mapped = options.static_tiers.find(job->second);
                    if (mapped != options.static_tiers.end()) msg.priority = mapped->second;
                }
            } else {
                const auto job = msg.headers.find("job");
                if (job != msg.headers.end()) {
                    for (std::size_t index = 0; index < 3; ++index)
                        if (job->second == std::string{"class"} + std::to_string(index))
                            msg.priority = options.job_tiers[index];
                }
            }
        }
    }
#endif
    msg.original_priority = msg.priority;
    msg.max_retries       = config_.default_max_retries;
    if (msg.ttl == kTtlUnset) {
        msg.ttl = config_.default_ttl;
    }
    if (feature_extractor_ || predictor_) {
        ml::RoutingContext context;
        if (feature_extractor_) {
            auto extracted = feature_extractor_->extract(msg.payload.size(), msg.headers);
            context.feature_schema_version = feature_extractor_->schema().version;
            context.routing_policy_version = config_.ingress_features->routing_policy_version;
            context.features = std::move(extracted.features);
            context.feature_validity = extracted.validity;
        } else {
            context.feature_schema_version = "none";
        }
        if (predictor_) msg.priority = predict_route(msg, context);
        msg.original_priority = msg.priority;
        context.ingress_priority = msg.priority;
        msg.routing_context = std::make_shared<const ml::RoutingContext>(std::move(context));
    }
    auto event = capture_event(msg, ml::FeedbackTrigger::Submit);
    queue_.enqueue(std::move(msg));
    mark_transition(event);
    publish_event(event);
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
        dlq_swept(queue_.sweep_expired_batch(config_.maintenance_batch_size), "TTL expired in queue");
        if (ctx->IsCancelled()) return grpc::Status::CANCELLED;
        if (Clock::now() >= deadline) break;

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        auto msg_opt = queue_.dequeue(std::min(remaining, kChunk));

        if (!msg_opt) continue;

        auto& msg = *msg_opt;

        if (msg.is_expired()) {
            auto event = capture_event(msg, ml::FeedbackTrigger::PullExpiry);
            if (event) { event->outcome = ml::FeedbackOutcome::Dlq; event->dlq_reason = DLQReason::TTL_EXPIRED; }
            dlq_.push(std::move(msg), DLQReason::TTL_EXPIRED,
                      "TTL expired at Pull time");
            mark_transition(event);
            publish_event(event);
            continue;
        }

        const std::string            msg_id  = msg.id;
        auto token = instance_token_ + "-" + Proxy::generate_id();
        auto owner = req->consumer_id();

        auto* pulled = resp->mutable_message();
        pulled->set_message_id(msg_id);
        pulled->set_attempt_token(token);
        pulled->set_lease_duration_ms(config_.delivery_lease.count());
        if (msg.payload.empty()) pulled->clear_payload();
        else pulled->set_payload(reinterpret_cast<const char*>(msg.payload.data()), msg.payload.size());
        for (const auto& [k, v] : msg.headers) {
            (*pulled->mutable_headers())[k] = v;
        }

#ifdef HARBINGER_BENCHMARK_SUPPORT
        std::optional<benchmark::Observation> dispatch;
#endif

        {
            std::lock_guard lock{in_flight_mutex_};
            if (ctx->IsCancelled()) {
                resp->Clear();
                queue_.requeue_front(std::move(msg));
                return grpc::Status::CANCELLED;
            }
            if (msg.delivery_count == std::numeric_limits<uint64_t>::max()) {
                resp->Clear(); queue_.requeue_front(std::move(msg));
                return {grpc::StatusCode::RESOURCE_EXHAUSTED, "Delivery ordinal exhausted"};
            }
            auto installed = in_flight_.end();
            static_assert(std::is_nothrow_move_assignable_v<Message>);
            static_assert(std::is_nothrow_move_assignable_v<std::string>);
            try {
                auto [entry, inserted] = in_flight_.try_emplace(msg_id);
                if (!inserted) throw std::logic_error("Duplicate in-flight message");
                installed = entry;
                auto lease = deadlines_.emplace(Clock::now() + config_.delivery_lease, msg_id);
                ++msg.delivery_count;
                entry->second.message = std::move(msg);
                entry->second.consumer_id = std::move(owner);
                entry->second.pull_time = Clock::now();
                entry->second.attempt_token = std::move(token);
                entry->second.deadline = lease;
                entry->second.attempt_id = entry->second.message.delivery_count;
#ifdef HARBINGER_BENCHMARK_SUPPORT
                if (config_.benchmark_options)
                    dispatch = benchmark::capture(entry->second.message, benchmark::Kind::Dispatch);
                if (config_.benchmark_options && !dispatch) config_.benchmark_options->observations->drop();
#endif
            } catch (...) {
                // Token allocation must occur before installation; remaining assignments are moves.
                if (installed != in_flight_.end()) in_flight_.erase(installed);
                resp->Clear(); queue_.requeue_front(std::move(msg));
                return {grpc::StatusCode::INTERNAL, "Delivery installation failed"};
            }
        }

#ifdef HARBINGER_BENCHMARK_SUPPORT
        if (dispatch) config_.benchmark_options->observations->publish(*dispatch);
#endif

        resp->set_timed_out(false);
        return grpc::Status::OK;
    }

    resp->set_timed_out(true);
    return grpc::Status::OK;
}

grpc::Status HarbingerService::Ack(grpc::ServerContext*,
                                const harbinger_rpc::AckRequest* req,
                                harbinger_rpc::AckResponse*) {
    return settle(req->consumer_id(), req->message_id(), req->attempt_token(),
                  Settlement::Ack, "Ack", req->processing_time_ms());
}

grpc::Status HarbingerService::Nack(grpc::ServerContext*,
                                 const harbinger_rpc::NackRequest* req,
                                 harbinger_rpc::NackResponse*) {
    return settle(req->consumer_id(), req->message_id(), req->attempt_token(),
                  Settlement::Nack, req->reason(), req->processing_time_ms());
}

void HarbingerService::prune_completions_locked(std::size_t budget) {
    const auto now = Clock::now();
    while (budget-- && !completions_.empty() &&
           (completions_.size() > config_.completion_cache_max_entries ||
            now - completions_.front().time >= config_.completion_retention)) {
        completed_.erase(completions_.front().token);
        completions_.pop_front();
    }
}

std::optional<ml::FeedbackEvent> HarbingerService::finish_locked(
    const std::string& id, Settlement kind, const std::string& reason,
    std::optional<int64_t> measurement, std::optional<PredictorUpdate>* update) {
    auto it = in_flight_.find(id);
    auto& entry = it->second;
    // Lock order is settlement -> queue/DLQ; no queue operation takes settlement.
    auto& msg = entry.message;
    const bool ttl_expired = msg.is_expired();
    // Learning rules: a successful Ack within the lease and TTL is a duration; a delivered attempt that overran its
    // lease is censored; Nack, Ack-after-TTL and anything without a key teach nothing. Applied after unlocking.
    const auto* context = msg.routing_context.get();
    if (update && predictor_ && context && context->predictor_key) {
        const bool features_ok = !feature_extractor_ || context->feature_validity == ml::FeatureValidity::Valid;
        if (kind == Settlement::Ack && !ttl_expired && features_ok && measurement && *measurement >= 0 &&
            *measurement <= config_.delivery_lease.count()) {
            *update = PredictorUpdate{*context->predictor_key, static_cast<double>(*measurement), false,
                                      context->predicted_processing_time_ms, context->predicted_bucket};
        } else if (kind == Settlement::Expired) {
            *update = PredictorUpdate{*context->predictor_key, std::nullopt, true, std::nullopt, std::nullopt};
        }
    }
    auto event = capture_event(msg, kind == Settlement::Expired
        ? ml::FeedbackTrigger::LeaseExpiry : ml::FeedbackTrigger::Settlement);
    if (event) {
        event->attempt_id = entry.attempt_id;
        event->processing_time_ms = measurement;
        if (kind != Settlement::Expired) event->operation = kind == Settlement::Ack
            ? ml::FeedbackOperation::Ack : ml::FeedbackOperation::Nack;
        event->outcome = ml::FeedbackOutcome::Ack;
    }
    if (ttl_expired) {
        if (event) { event->outcome = ml::FeedbackOutcome::Dlq; event->dlq_reason = DLQReason::TTL_EXPIRED; }
        dlq_.push(std::move(msg), DLQReason::TTL_EXPIRED, "TTL expired; " + reason);
    } else if (kind != Settlement::Ack) {
        ++msg.retry_count;
        if (event) event->retry_count = msg.retry_count;
        if (msg.retry_count >= msg.max_retries) {
            if (event) { event->outcome = ml::FeedbackOutcome::Dlq; event->dlq_reason = DLQReason::MAX_RETRIES_EXCEEDED; }
            const auto details = "Exceeded max_retries=" + std::to_string(msg.max_retries) +
                                 "; last reason: " + reason;
            dlq_.push(std::move(msg), DLQReason::MAX_RETRIES_EXCEEDED, details);
        } else {
            if (event) event->outcome = ml::FeedbackOutcome::Retry;
            msg.priority = msg.original_priority;
            queue_.enqueue(std::move(msg));
        }
    }
    completions_.push_back({id, entry.attempt_token, entry.consumer_id, kind, Clock::now()});
    completed_.emplace(entry.attempt_token, std::prev(completions_.end()));
    deadlines_.erase(entry.deadline);
    in_flight_.erase(it);
    prune_completions_locked(1);
    mark_transition(event);
    return event;
}

grpc::Status HarbingerService::settle(const std::string& owner, const std::string& id,
                                     const std::string& token, Settlement kind,
                                     const std::string& reason, int64_t processing_time_ms) {
    if (!is_registered_consumer(owner))
        return {grpc::StatusCode::PERMISSION_DENIED, "Unknown consumer"};
    if (token.empty() || token.size() > 128 ||
        !std::all_of(token.begin(), token.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-';
        })) return {grpc::StatusCode::INVALID_ARGUMENT, "Invalid attempt token"};
    std::unique_lock lock{in_flight_mutex_};
    auto prior = completed_.find(token);
    if (prior != completed_.end()) {
        const auto& record = *prior->second;
        if (Clock::now() - record.time >= config_.completion_retention) {
            completions_.erase(prior->second);
            completed_.erase(prior);
        } else if (record.message_id == id) {
            if (record.owner != owner)
                return {grpc::StatusCode::PERMISSION_DENIED, "Different delivery owner"};
            return record.kind == kind ? grpc::Status::OK
                : grpc::Status{grpc::StatusCode::FAILED_PRECONDITION, "Attempt already settled or expired"};
        }
    }
    const auto live = in_flight_.find(id);
    if (live == in_flight_.end())
        return {grpc::StatusCode::NOT_FOUND, "Unknown delivery"};
    if (live->second.consumer_id != owner)
        return {grpc::StatusCode::PERMISSION_DENIED, "Different delivery owner"};
    if (live->second.attempt_token != token)
        return {grpc::StatusCode::FAILED_PRECONDITION, "Stale attempt token"};
    std::optional<PredictorUpdate> update;
    if (Clock::now() >= live->second.deadline->first) {
        auto event = finish_locked(id, Settlement::Expired, "Delivery lease expired", std::nullopt, &update);
        lock.unlock(); publish_event(event);
        if (update) learn(*update);
        return {grpc::StatusCode::FAILED_PRECONDITION, "Delivery lease expired"};
    }
    auto event = finish_locked(id, kind, reason, processing_time_ms, &update);
    lock.unlock(); publish_event(event);
    if (update) learn(*update);
    return grpc::Status::OK;
}

void HarbingerService::maintain_deliveries() {
    std::vector<ml::FeedbackEvent> events;
    std::vector<PredictorUpdate> updates;
    bool collect = feedback_writer_ != nullptr;
#ifdef HARBINGER_BENCHMARK_SUPPORT
    collect = collect || config_.benchmark_options.has_value();
#endif
    if (collect) {
        try { events.reserve(config_.maintenance_batch_size); }
        catch (...) { collect = false; }
    }
    {
        std::lock_guard lock{in_flight_mutex_};
        std::size_t budget = config_.maintenance_batch_size;
        const auto now = Clock::now();
        while (budget-- && !deadlines_.empty() && deadlines_.begin()->first <= now) {
            const auto id = deadlines_.begin()->second;
            std::optional<PredictorUpdate> update;
            auto event = finish_locked(id, Settlement::Expired, "Delivery lease expired", std::nullopt,
                                       predictor_ ? &update : nullptr);
            if (update) {
                try { updates.push_back(std::move(*update)); } catch (...) {}
            }
            if (event) {
                if (collect) events.push_back(std::move(*event));
                else {
                    if (feedback_writer_) feedback_writer_->record_drop(ml::FeedbackDrop::Capture);
#ifdef HARBINGER_BENCHMARK_SUPPORT
                    if (config_.benchmark_options) config_.benchmark_options->observations->drop();
#endif
                }
            }
        }
        prune_completions_locked(config_.maintenance_batch_size);
    }
    for (const auto& event : events) publish_event(event);
    for (const auto& update : updates) learn(update);
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
        const auto& payload = entry.message.payload;
        if (payload.empty()) out->clear_payload();
        else out->set_payload(reinterpret_cast<const char*>(payload.data()), payload.size());
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
