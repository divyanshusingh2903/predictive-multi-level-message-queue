#include "ml/feedback_event.hpp"

#include <charconv>
#include <cmath>
#include <ctime>
#include <stdexcept>

namespace harbinger::ml {
namespace {
class Json {
public:
    explicit Json(std::size_t limit) : limit_(limit) {}
    void add(std::string_view text) {
        if (text.size() > limit_ - data.size()) throw std::length_error("event_limit");
        data.append(text);
    }
    void quoted(std::string_view text) {
        add("\"");
        for (unsigned char c : text) {
            if (c == '"') add("\\\"");
            else if (c == '\\') add("\\\\");
            else if (c < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                const char escaped[]{'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
                add(std::string_view(escaped, sizeof escaped));
            } else {
                const char byte = static_cast<char>(c);
                add(std::string_view(&byte, 1));
            }
        }
        add("\"");
    }
    void number(double value) {
        if (!std::isfinite(value)) throw std::invalid_argument("nonfinite feedback number");
        char buffer[64];
        const auto converted = std::to_chars(buffer, buffer + sizeof buffer,
            value == 0 ? 0 : value, std::chars_format::scientific, 16);
        if (converted.ec != std::errc{}) throw std::invalid_argument("feedback conversion");
        add(std::string_view(buffer, static_cast<std::size_t>(converted.ptr - buffer)));
    }
    std::string data;
private:
    std::size_t limit_;
};

std::string_view name(FeedbackTrigger value) {
    switch (value) {
        case FeedbackTrigger::Submit: return "submit";
        case FeedbackTrigger::Settlement: return "settlement";
        case FeedbackTrigger::LeaseExpiry: return "lease_expiry";
        case FeedbackTrigger::PullExpiry: return "pull_expiry";
        case FeedbackTrigger::TtlSweep: return "ttl_sweep";
    }
    throw std::invalid_argument("feedback trigger");
}
std::string_view name(FeedbackOutcome value) {
    switch (value) {
        case FeedbackOutcome::Ack: return "ack";
        case FeedbackOutcome::Retry: return "retry";
        case FeedbackOutcome::Dlq: return "dlq";
    }
    throw std::invalid_argument("feedback outcome");
}
std::string_view name(DLQReason value) {
    switch (value) {
        case DLQReason::TTL_EXPIRED: return "TTL_EXPIRED";
        case DLQReason::MAX_RETRIES_EXCEEDED: return "MAX_RETRIES_EXCEEDED";
        case DLQReason::PROCESSING_ERROR: return "PROCESSING_ERROR";
    }
    throw std::invalid_argument("feedback DLQ reason");
}
std::string_view name(RoutingMode value) {
    switch (value) {
        case RoutingMode::Disabled: return "disabled";
        case RoutingMode::Shadow: return "shadow";
        case RoutingMode::Predictive: return "predictive";
    }
    throw std::invalid_argument("feedback mode");
}
std::string_view name(FallbackReason value) {
    switch (value) {
        case FallbackReason::Timeout: return "timeout";
        case FallbackReason::Unavailable: return "unavailable";
        case FallbackReason::Overloaded: return "overloaded";
        case FallbackReason::Unready: return "unready";
        case FallbackReason::IncompatibleVersion: return "incompatible_version";
        case FallbackReason::InvalidPrediction: return "invalid_prediction";
        case FallbackReason::FeatureLimit: return "feature_limit";
        case FallbackReason::ClientError: return "client_error";
    }
    throw std::invalid_argument("feedback fallback");
}
void context_json(Json& out, const RoutingContext& context) {
    validate_version(context.feature_schema_version);
    validate_version(context.routing_policy_version);
    out.add("{\"feature_schema_version\":"); out.quoted(context.feature_schema_version);
    out.add(",\"routing_policy_version\":"); out.quoted(context.routing_policy_version);
    out.add(",\"model_version\":");
    if (context.model_version) { validate_version(*context.model_version); out.quoted(*context.model_version); }
    else out.add("null");
    out.add(",\"mode\":"); out.quoted(name(context.mode));
    out.add(",\"features\":");
    if (context.features) out.add(snapshot_json(*context.features)); else out.add("null");
    out.add(",\"feature_validity\":");
    out.quoted(context.feature_validity == FeatureValidity::Valid ? "valid" : "feature_limit");
    out.add(",\"predicted_processing_time_ms\":");
    if (context.predicted_processing_time_ms) out.number(*context.predicted_processing_time_ms); else out.add("null");
    out.add(",\"predicted_bucket\":");
    if (context.predicted_bucket) out.add(std::to_string(*context.predicted_bucket)); else out.add("null");
    out.add(",\"ingress_priority\":"); out.add(std::to_string(context.ingress_priority));
    out.add(",\"fallback_reason\":");
    if (context.fallback_reason) out.quoted(name(*context.fallback_reason)); else out.add("null");
    out.add(",\"inference_elapsed_ms\":");
    if (context.inference_elapsed_ms) out.number(*context.inference_elapsed_ms); else out.add("null");
    out.add("}");
}
} // namespace

FeedbackEvent capture_feedback(const Message& message, FeedbackTrigger trigger) {
    FeedbackEvent event;
    if (message.id.empty() || message.id.size() > 128) throw std::invalid_argument("feedback message id");
    event.message_id = message.id;
    event.routing = message.routing_context;
    event.trigger = trigger;
    event.retry_count = message.retry_count;
    event.collected_at = std::chrono::system_clock::now();
    event.elapsed_since_arrival_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - message.arrival_time).count();
    return event;
}

LabelStatus classify_label(const FeedbackEvent& event, int64_t lease) noexcept {
    if (!event.processing_time_ms) return LabelStatus::Missing;
    if (*event.processing_time_ms < 0) return LabelStatus::Negative;
    if (*event.processing_time_ms > lease) return LabelStatus::OutOfRange;
    if (!event.routing || !event.routing->features || event.routing->feature_validity != FeatureValidity::Valid)
        return LabelStatus::InvalidFeatures;
    if (event.dlq_reason == DLQReason::TTL_EXPIRED) return LabelStatus::Censored;
    if (event.operation == FeedbackOperation::Nack) return LabelStatus::Failure;
    return event.outcome == FeedbackOutcome::Ack ? LabelStatus::Eligible : LabelStatus::Missing;
}

std::string_view to_string(LabelStatus status) {
    switch (status) {
        case LabelStatus::Eligible: return "eligible";
        case LabelStatus::Failure: return "failure";
        case LabelStatus::Censored: return "censored";
        case LabelStatus::Missing: return "missing";
        case LabelStatus::Negative: return "negative";
        case LabelStatus::OutOfRange: return "out_of_range";
        case LabelStatus::InvalidFeatures: return "invalid_features";
    }
    throw std::invalid_argument("feedback label");
}

std::string feedback_json(const FeedbackEvent& event, std::string_view instance,
                          uint64_t sequence, int64_t lease, std::size_t max_bytes) {
    validate_version(instance);
    validate_version(event.message_id);
    if (!sequence || !event.routing || event.message_id.empty() || event.message_id.size() > 128 ||
        max_bytes > kMaxEventBytes) throw std::invalid_argument("feedback identity/context/limit");
    Json out(max_bytes);
    out.add("{\"record_version\":1,\"event_id\":");
    out.quoted(std::string(instance) + ":" + std::to_string(sequence));
    out.add(",\"broker_instance_id\":"); out.quoted(instance);
    out.add(",\"message_id\":"); out.quoted(event.message_id);
    out.add(",\"event_type\":"); out.quoted(event.trigger == FeedbackTrigger::Submit ? "ingress" : "outcome");
    out.add(",\"attempt_id\":");
    if (event.attempt_id) out.quoted(std::to_string(*event.attempt_id)); else out.add("null");
    const auto timestamp = std::chrono::system_clock::to_time_t(event.collected_at);
    std::tm utc{};
    if (!gmtime_r(&timestamp, &utc)) throw std::invalid_argument("feedback timestamp");
    char buffer[32];
    if (!std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &utc))
        throw std::invalid_argument("feedback timestamp");
    out.add(",\"collected_at_utc\":"); out.quoted(buffer);
    out.add(",\"routing\":"); context_json(out, *event.routing);
    out.add(",\"settlement_operation\":");
    if (event.operation) out.quoted(*event.operation == FeedbackOperation::Ack ? "ack" : "nack"); else out.add("null");
    out.add(",\"trigger\":"); out.quoted(name(event.trigger));
    out.add(",\"outcome\":");
    if (event.outcome) out.quoted(name(*event.outcome)); else out.add("null");
    out.add(",\"dlq_reason\":");
    if (event.dlq_reason) out.quoted(name(*event.dlq_reason)); else out.add("null");
    out.add(",\"retry_count\":"); out.add(std::to_string(event.retry_count));
    out.add(",\"processing_time_ms\":");
    if (event.processing_time_ms) out.add(std::to_string(*event.processing_time_ms)); else out.add("null");
    out.add(",\"label_status\":"); out.quoted(to_string(classify_label(event, lease)));
    out.add(",\"elapsed_since_arrival_ms\":"); out.add(std::to_string(event.elapsed_since_arrival_ms));
    out.add("}");
    return std::move(out.data);
}
} // namespace harbinger::ml
