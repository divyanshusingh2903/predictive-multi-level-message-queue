#pragma once

#include "ml/features.hpp"

namespace harbinger::ml {

enum class RoutingMode { Disabled, Shadow, Predictive };
enum class FallbackReason {
    Timeout, Unavailable, Overloaded, Unready, IncompatibleVersion,
    InvalidPrediction, FeatureLimit, ClientError
};

/// Opt-in static-mode ingress capture; classifier and persistence wiring belong to later issues.
struct IngressFeatureConfig {
    FeatureSchema schema;
    std::string routing_policy_version;
};

/// Immutable when attached to a Message through shared_ptr<const RoutingContext>.
struct RoutingContext {
    std::string feature_schema_version;
    std::string routing_policy_version;
    std::optional<std::string> model_version{};
    std::optional<FeatureSnapshot> features{};
    FeatureValidity feature_validity{FeatureValidity::Valid};
    RoutingMode mode{RoutingMode::Disabled};
    std::optional<double> predicted_processing_time_ms{};
    std::optional<uint8_t> predicted_bucket{};
    uint8_t ingress_priority{0};
    std::optional<FallbackReason> fallback_reason{};
    std::optional<double> inference_elapsed_ms{};
};

} // namespace harbinger::ml
