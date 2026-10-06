#pragma once

#include "ml/duration_predictor.hpp"
#include "ml/routing_context.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace harbinger::ml {

/// Opt-in in-process prediction at ingress; absent means static routing with no predictor.
struct PredictiveRoutingConfig {
    /// Shadow records predictions but routes to default_priority; Predictive routes by them. Disabled is rejected:
    /// leave the whole config unset instead.
    RoutingMode mode{RoutingMode::Shadow};
    PredictorKeyPolicy key{};
    /// num_levels and default_priority are always taken from HarbingerConfig.
    PerKeyPredictorConfig predictor{};
    /// Identity of this routing policy; boundaries are versioned separately by the model version.
    std::string routing_policy_version{"per-key-v2"};
    /// Optional predictor state file loaded at startup and rewritten atomically every snapshot_interval.
    std::optional<std::filesystem::path> snapshot_path{};
    std::chrono::milliseconds snapshot_interval{60000};
};

inline constexpr std::size_t kPredictionStatusCount = 7;
inline constexpr std::size_t kLatencyBuckets = 6;

/// Bounded-cardinality routing and learning counters; no message ids or key values.
struct RoutingStats {
    bool enabled{false};
    RoutingMode mode{RoutingMode::Disabled};
    uint64_t lookups{0};
    /// Indexed by PredictionStatus.
    std::array<uint64_t, kPredictionStatusCount> outcomes{};
    uint64_t routed_by_prediction{0};
    /// Lookups whose size-binned key was cold and whose un-binned parent key supplied the prediction.
    uint64_t parent_fallbacks{0};
    /// Lookup latency: <1us, <10us, <100us, <1ms, <10ms, >=10ms.
    std::array<uint64_t, kLatencyBuckets> latency_buckets{};
    uint64_t latency_ns_total{0};
    uint64_t latency_ns_max{0};
    uint64_t learned{0};
    uint64_t learn_rejected{0};
    uint64_t censored_observed{0};
    /// Learned labels whose ingress prediction carried an estimate.
    uint64_t scored{0};
    /// Of those, labels whose actual tier (current boundaries) equals the predicted tier.
    uint64_t tier_matches{0};
    /// Exponentially weighted mean of |log2(predicted / actual)| over scored labels (alpha 0.01).
    double abs_log2_error_ewma{0.0};
    /// Times the rolling tier agreement fell below the drift threshold; informational only, never changes routing.
    uint64_t drift_alerts{0};
    uint64_t snapshot_saves{0};
    uint64_t snapshot_failures{0};
    bool snapshot_loaded{false};
    std::string snapshot_status{};
    PredictorStats predictor{};
};

} // namespace harbinger::ml
