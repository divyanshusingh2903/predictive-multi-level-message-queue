#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
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
inline constexpr std::size_t kMaxJobKeyBytes = 128;

enum class DurationSummary { Median, P75, Mean };
enum class PredictionStatus { Predicted, Unready, ColdKey, HighSpread, Censored, StaleKey, InvalidKey };

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
    /// Cheap lookup; never touches queue or settlement state.
    [[nodiscard]] virtual DurationPrediction predict(std::string_view key) = 0;
    /// Learn one successful handler duration; false when the key or duration is rejected.
    virtual bool observe(std::string_view key, double duration_ms) = 0;
    /// Record that a delivered attempt on this key ran past its lease or TTL without a measurable duration.
    virtual bool observe_censored(std::string_view key) = 0;
    [[nodiscard]] virtual std::string model_version() const = 0;
};

struct PerKeyPredictorConfig {
    uint8_t num_levels{3};
    uint8_t default_priority{1};
    DurationSummary summary{DurationSummary::Median};
    /// Observations a key needs before it is predicted (N_min).
    uint32_t min_samples{20};
    /// Keys whose spread_quantile / p50 exceeds this fall back to the default tier. A high quantile is used
    /// so a minority of very long jobs on an otherwise short key is detected.
    double spread_quantile{0.99};
    double max_spread_ratio{16.0};
    /// Keys with more than this decayed fraction of censored (lease/TTL overrun) attempts fall back.
    double max_censored_fraction{0.05};
    /// Per-observation retention weight in (0, 1]; 0.995 forgets with a half-life near 138 samples.
    double decay{0.995};
    /// Retention weight of the global histogram that defines tier boundaries.
    double global_decay{0.9999};
    /// Wall-clock half-life applied to a key's history across idle gaps of at least one second; zero disables.
    std::chrono::milliseconds time_half_life{600000};
    /// A key idle this long is not predicted, and its history is discarded on its next observation; zero disables.
    std::chrono::milliseconds stale_after{1800000};
    /// Histograms share one layout: [0, min_ms), geometric bins up to max_ms, last bin includes everything above.
    /// Durations are integer milliseconds on the wire, so min_ms = 1 puts every sub-millisecond job in one bin.
    /// Longer durations are clamped to max_ms (counted as saturated) so the median and mean stay consistent.
    std::size_t histogram_bins{48};
    double min_ms{1.0};
    double max_ms{600000.0};
    /// Hard cap on tracked keys; observations for extra keys are counted as overflow and not learned per key.
    std::size_t max_keys{16384};
    std::size_t shards{16};
    /// A full shard recycles its least recently updated key if it is proven but idle this long...
    std::chrono::milliseconds idle_eviction{600000};
    /// ...or still unproven (< min_samples) and idle this long. The grace stops a working set larger than the cap
    /// from thrashing while still letting a flood of one-off keys be displaced.
    std::chrono::milliseconds cold_eviction_grace{30000};
    /// Recompute boundaries every this many global observations.
    uint32_t boundary_refresh_every{256};
    /// Global observations required before the first boundary snapshot is published.
    uint32_t global_min_samples{100};
    /// Relative band within which boundaries and a key's current tier are kept.
    double hysteresis{0.10};
    /// Test seam for the time source; defaults to the steady clock.
    std::function<std::chrono::steady_clock::time_point()> clock{};
};

/// Fixed-layout histogram with O(1) exponential decay (scaled increments) and interpolated quantiles.
class DecayingHistogram {
public:
    DecayingHistogram(std::size_t bins, double min_ms, double max_ms, double decay);
    /// Values above max_ms are clamped; returns true when clamping happened.
    bool add(double value_ms);
    void add_censored();
    /// Multiply all retained weight by factor in (0, 1], used for wall-clock decay across idle gaps.
    void shrink(double factor);
    [[nodiscard]] double quantile(double q) const;
    [[nodiscard]] double mean() const;
    [[nodiscard]] double censored_fraction() const;
    [[nodiscard]] double weight() const noexcept { return total_ / scale_; }
    [[nodiscard]] uint64_t observations() const noexcept { return observations_; }
    [[nodiscard]] std::size_t approximate_bytes() const noexcept;

private:
    friend class PerKeyPredictor;
    [[nodiscard]] std::size_t bin(double value_ms) const noexcept;
    [[nodiscard]] double lower(std::size_t index) const noexcept;
    [[nodiscard]] double upper(std::size_t index) const noexcept;
    void renormalize_if_needed();

    std::vector<double> counts_;
    double min_ms_, max_ms_, ratio_, log_ratio_, gain_;
    double scale_{1.0}, total_{0.0}, sum_{0.0}, censored_{0.0};
    uint64_t observations_{0};
};

/// Counters for monitoring bounded behaviour.
struct PredictorStats {
    std::size_t keys{0};
    uint64_t observations{0};
    uint64_t rejected{0};
    uint64_t evictions{0};
    /// Observations whose key could not be tracked (cap reached, nothing recyclable); counted, never predicted.
    uint64_t overflow_observations{0};
    uint64_t saturated{0};
    uint64_t censored{0};
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
    bool observe_censored(std::string_view key) override;
    [[nodiscard]] std::string model_version() const override;

    /// Current boundaries; a short internal lock may be taken (std::atomic<shared_ptr> is not lock-free).
    [[nodiscard]] std::shared_ptr<const BoundarySnapshot> snapshot() const;
    [[nodiscard]] PredictorStats stats() const;
    [[nodiscard]] const PerKeyPredictorConfig& config() const noexcept { return config_; }
    /// Upper bound on histogram and key storage implied by max_keys, not whole-process memory.
    [[nodiscard]] std::size_t memory_bound_bytes() const noexcept;

    /// Write all state to path atomically (temporary file, fsync, rename); an existing file becomes path.prev.
    /// context names the key policy and routing version; load() rejects a snapshot taken under another context.
    void save(const std::filesystem::path& path, std::string_view context) const;
    /// Replace all state from a snapshot written by save(). Validation completes before anything changes, so a
    /// corrupt, truncated or incompatible file throws and leaves the predictor as it was (cold at startup).
    /// Returns the number of keys restored; keys beyond the current per-shard cap are skipped.
    std::size_t load(const std::filesystem::path& path, std::string_view context);

private:
    using Clock = std::chrono::steady_clock;
    struct Entry {
        DecayingHistogram histogram;
        std::list<const std::string*>::iterator position;
        Clock::time_point last_update;
        Clock::time_point last_decay;
        int tier{-1};
    };
    struct StringHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
    };
    struct Shard {
        std::mutex mutex;
        std::unordered_map<std::string, Entry, StringHash, std::equal_to<>> keys;
        std::list<const std::string*> recency;
        uint64_t evictions{0};
    };

    [[nodiscard]] DecayingHistogram make_histogram(double decay) const;
    [[nodiscard]] Clock::time_point now() const;
    [[nodiscard]] Shard& shard_for(std::string_view key) const;
    [[nodiscard]] bool stale(const Entry& entry, Clock::time_point at) const;
    /// Find or admit the key and bring its history up to date; returns null when the key cannot be tracked.
    [[nodiscard]] Entry* touch_locked(Shard& shard, std::string_view key, Clock::time_point at);
    void refresh_boundaries_locked();
    [[nodiscard]] DurationPrediction fallback(PredictionStatus status, uint64_t seen) const;
    [[nodiscard]] DurationPrediction evaluate(Entry& entry, const BoundarySnapshot& snapshot) const;

    PerKeyPredictorConfig config_;
    mutable std::vector<std::unique_ptr<Shard>> shards_;
    std::size_t per_shard_cap_;

    mutable std::mutex global_mutex_;
    DecayingHistogram global_;
    std::atomic<std::shared_ptr<const BoundarySnapshot>> snapshot_;
    uint64_t global_observations_{0}, snapshots_{0}, degenerate_{0};

    std::atomic<uint64_t> rejected_{0}, overflow_count_{0}, observed_{0}, saturated_{0}, censored_{0};
};

/// How a message maps to a predictor key.
struct PredictorKeyPolicy {
    /// Optional producer-supplied header naming the job type; must be on the ingress allowlist when features are on.
    std::string job_header{"job_type"};
    /// Prefix the key with the broker-assigned producer id so one producer cannot claim another's history.
    bool scope_by_producer{true};
    std::string producer_header{"__producer_id"};
};

/// Key for a message, or nullopt when none can be formed (the caller then uses the default tier).
/// There is no queue-name key: the broker is one logical queue, so the default is the producer id.
[[nodiscard]] std::optional<std::string> derive_predictor_key(
    const std::unordered_map<std::string, std::string>& headers, const PredictorKeyPolicy& policy);

} // namespace harbinger::ml
