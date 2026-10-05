#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace harbinger::ml {

inline constexpr std::size_t kMaxPredictorKeyBytes = 256;

enum class DurationSummary { Median, P75, Mean };
enum class PredictionStatus { Predicted, Unready, ColdKey, HighSpread, InvalidKey };

/// Stable machine-readable status; Predicted is the only status without a fallback.
[[nodiscard]] std::string_view to_string(PredictionStatus status) noexcept;

/// Estimate and tier for one key; every non-Predicted status carries the default tier and no estimate.
struct DurationPrediction {
    std::optional<double> estimate_ms{};
    uint8_t bucket{0};
    PredictionStatus status{PredictionStatus::Unready};
    uint64_t key_observations{0};
};

/// Tier for an estimate: equality with a boundary enters the next tier, matching the offline bisect_right.
[[nodiscard]] uint8_t tier_of(const std::vector<double>& boundaries, double value) noexcept;

/// Narrow predictor boundary so a richer model can replace the per-key lookup without touching routing.
class DurationPredictor {
public:
    virtual ~DurationPredictor() = default;
    /// O(1) lookup; never blocks on queue or settlement state.
    [[nodiscard]] virtual DurationPrediction predict(std::string_view key) = 0;
    /// Learn one successful handler duration; false when the key or duration is rejected.
    virtual bool observe(std::string_view key, double duration_ms) = 0;
    [[nodiscard]] virtual std::string model_version() const = 0;
};

struct PerKeyPredictorConfig {
    uint8_t num_levels{3};
    uint8_t default_priority{1};
    DurationSummary summary{DurationSummary::Median};
    /// Observations a key needs before it is predicted (N_min).
    uint32_t min_samples{20};
    /// Keys whose p90/p50 exceeds this fall back to the default tier.
    double max_spread_ratio{16.0};
    /// Per-observation retention weight in (0, 1]; 0.995 forgets with a half-life near 138 samples.
    double decay{0.995};
    /// Retention weight of the global histogram that defines tier boundaries.
    double global_decay{0.9999};
    /// Fixed log-spaced layout shared by every histogram: [0, min_ms), geometric bins up to about max_ms, last bin open-ended.
    std::size_t histogram_bins{48};
    double min_ms{0.1};
    double max_ms{600000.0};
    /// Hard cap on tracked keys; extra keys feed the shared overflow bucket.
    std::size_t max_keys{16384};
    std::size_t shards{16};
    /// A full shard evicts its least recently updated key only if idle for this many shard observations.
    uint64_t idle_eviction_observations{100000};
    /// Recompute boundaries every this many global observations.
    uint32_t boundary_refresh_every{256};
    /// Global observations required before the first boundary snapshot is published.
    uint32_t global_min_samples{100};
    /// Relative band within which boundaries and a key's current tier are kept.
    double hysteresis{0.10};
};

/// Fixed-layout histogram with O(1) exponential decay (scaled increments) and interpolated quantiles.
class DecayingHistogram {
public:
    DecayingHistogram(std::size_t bins, double min_ms, double max_ms, double decay);
    void add(double value_ms);
    [[nodiscard]] double quantile(double q) const;
    [[nodiscard]] double mean() const;
    [[nodiscard]] double weight() const noexcept { return total_ / scale_; }
    [[nodiscard]] uint64_t observations() const noexcept { return observations_; }
    [[nodiscard]] std::size_t approximate_bytes() const noexcept;

private:
    [[nodiscard]] std::size_t bin(double value_ms) const noexcept;
    [[nodiscard]] double lower(std::size_t index) const noexcept;
    [[nodiscard]] double upper(std::size_t index) const noexcept;

    std::vector<double> counts_;
    double min_ms_, ratio_, log_ratio_, gain_;
    double scale_{1.0}, total_{0.0}, sum_{0.0};
    uint64_t observations_{0};
};

/// Counters for monitoring bounded behaviour.
struct PredictorStats {
    std::size_t keys{0};
    uint64_t observations{0};
    uint64_t rejected{0};
    uint64_t evictions{0};
    uint64_t overflow_observations{0};
    uint64_t snapshots{0};
    uint64_t degenerate_refreshes{0};
};

/// Immutable tier boundaries: exactly num_levels - 1 positive increasing values, equality enters the next tier.
struct BoundarySnapshot {
    uint64_t sequence{0};
    std::vector<double> boundaries_ms{};
};

/// Per-key decaying-histogram predictor; thread-safe, locks are never nested.
class PerKeyPredictor final : public DurationPredictor {
public:
    explicit PerKeyPredictor(PerKeyPredictorConfig config = {});

    [[nodiscard]] DurationPrediction predict(std::string_view key) override;
    bool observe(std::string_view key, double duration_ms) override;
    [[nodiscard]] std::string model_version() const override;

    [[nodiscard]] std::shared_ptr<const BoundarySnapshot> snapshot() const;
    [[nodiscard]] PredictorStats stats() const;
    [[nodiscard]] const PerKeyPredictorConfig& config() const noexcept { return config_; }
    /// Upper bound on histogram and key storage implied by max_keys, not whole-process memory.
    [[nodiscard]] std::size_t memory_bound_bytes() const noexcept;

private:
    struct Entry {
        DecayingHistogram histogram;
        std::list<const std::string*>::iterator position;
        uint64_t last_touch{0};
        int tier{-1};
    };
    struct Shard {
        std::mutex mutex;
        std::unordered_map<std::string, Entry> keys;
        std::list<const std::string*> recency;
        uint64_t clock{0};
        uint64_t evictions{0};
    };

    [[nodiscard]] DecayingHistogram make_histogram(double decay) const;
    [[nodiscard]] Shard& shard_for(std::string_view key) const;
    void refresh_boundaries_locked();
    [[nodiscard]] DurationPrediction fallback(PredictionStatus status, uint64_t seen) const;
    [[nodiscard]] DurationPrediction evaluate(const DecayingHistogram& histogram, Entry* entry,
        const BoundarySnapshot& snapshot) const;

    PerKeyPredictorConfig config_;
    mutable std::vector<std::unique_ptr<Shard>> shards_;
    std::size_t per_shard_cap_;

    mutable std::mutex global_mutex_;
    DecayingHistogram global_;
    std::atomic<std::shared_ptr<const BoundarySnapshot>> snapshot_;
    uint64_t global_observations_{0}, snapshots_{0}, degenerate_{0};

    mutable std::mutex overflow_mutex_;
    DecayingHistogram overflow_;

    std::atomic<uint64_t> rejected_{0}, overflow_count_{0}, observed_{0};
};

} // namespace harbinger::ml
