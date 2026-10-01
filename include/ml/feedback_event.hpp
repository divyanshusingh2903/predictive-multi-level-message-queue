#pragma once

#include "ml/routing_context.hpp"
#include "queue/message.hpp"

#include <optional>
#include <string_view>

namespace harbinger::ml {

inline constexpr std::size_t kMaxEventBytes = 16384;
enum class FeedbackTrigger { Submit, Settlement, LeaseExpiry, PullExpiry, TtlSweep };
enum class FeedbackOperation { Ack, Nack };
enum class FeedbackOutcome { Ack, Retry, Dlq };
enum class LabelStatus { Eligible, Failure, Censored, Missing, Negative, OutOfRange, InvalidFeatures };

/// Bounded broker-only snapshot prepared before moving a message and published after transition.
struct FeedbackEvent {
    std::string message_id;
    std::shared_ptr<const RoutingContext> routing;
    std::optional<uint64_t> attempt_id;
    FeedbackTrigger trigger{FeedbackTrigger::Submit};
    std::optional<FeedbackOperation> operation;
    std::optional<FeedbackOutcome> outcome;
    std::optional<DLQReason> dlq_reason;
    uint32_t retry_count{0};
    std::optional<int64_t> processing_time_ms;
    std::chrono::system_clock::time_point collected_at;
    int64_t elapsed_since_arrival_ms{0};
};

/// Prepare a privacy-safe snapshot; allocation failures are handled by the caller.
[[nodiscard]] FeedbackEvent capture_feedback(const Message& message, FeedbackTrigger trigger);
/// Apply ordered measurement/feature/outcome rules without changing broker disposition.
[[nodiscard]] LabelStatus classify_label(const FeedbackEvent& event, int64_t delivery_lease_ms) noexcept;
/// Stable machine-readable label status.
[[nodiscard]] std::string_view to_string(LabelStatus status);
/// Encode version-1 event JSON, excluding newline, with a hard bounded byte limit.
[[nodiscard]] std::string feedback_json(const FeedbackEvent& event, std::string_view instance,
                                      uint64_t sequence, int64_t delivery_lease_ms,
                                      std::size_t max_bytes = kMaxEventBytes);

} // namespace harbinger::ml
