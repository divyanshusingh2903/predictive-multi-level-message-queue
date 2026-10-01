#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace harbinger::ml {

inline constexpr std::size_t kMaxHeaders = 16;
inline constexpr std::size_t kMaxKeyBytes = 64;
inline constexpr std::size_t kMaxValueBytes = 256;
inline constexpr std::size_t kMaxVocabulary = 1024;
inline constexpr std::size_t kMaxFeatureBytes = 8192;

enum class FeatureType { Numeric, Categorical };
enum class CategoricalEncoding { Vocabulary, Hash };
enum class MissingReason { Absent, InvalidType, OutOfRange, InvalidEncoding, Oversized };
enum class FeatureValidity { Valid, FeatureLimit };

/// One explicitly allowlisted numeric or categorical application header.
struct HeaderFeature {
    std::string name;
    FeatureType type{FeatureType::Numeric};
    double minimum{0};
    double maximum{0};
    CategoricalEncoding encoding{CategoricalEncoding::Vocabulary};
    std::vector<std::string> vocabulary{};
};

/// Immutable specification shared with Python under an explicit version.
struct FeatureSchema {
    std::string version;
    std::vector<HeaderFeature> headers{};
};

using FeatureValue = std::variant<std::monostate, double, std::string>;

/// Typed ingress data containing no payload or unapproved header values.
struct FeatureSnapshot {
    uint64_t payload_size_bytes{0};
    std::map<std::string, FeatureValue> headers{};
    std::map<std::string, MissingReason> missing_reasons{};
};

/// Explicit absence of the complete snapshot when its encoded size exceeds the cap.
struct ExtractedFeatures {
    std::optional<FeatureSnapshot> features{};
    FeatureValidity validity{FeatureValidity::Valid};
};

/// Validate a nonempty bounded UTF-8 version identifier.
void validate_version(std::string_view version);
/// Validate structure and serialize deterministic UTF-8 JSON; oversized snapshots throw length_error.
[[nodiscard]] std::string snapshot_json(const FeatureSnapshot& snapshot);
/// Stable machine-readable missing-value reason.
[[nodiscard]] std::string_view to_string(MissingReason reason);

/// Own a validated schema and extract only its bounded allowlist at ingress.
class FeatureExtractor {
public:
    explicit FeatureExtractor(FeatureSchema schema);
    [[nodiscard]] const FeatureSchema& schema() const noexcept { return schema_; }
    [[nodiscard]] ExtractedFeatures extract(
        uint64_t payload_size_bytes,
        const std::unordered_map<std::string, std::string>& headers) const;

private:
    FeatureSchema schema_;
};

} // namespace harbinger::ml
