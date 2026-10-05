#include "ml/duration_predictor.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>

namespace harbinger::ml {

namespace {

constexpr double kRenormalizeAt = 1e100;

PerKeyPredictorConfig validated(PerKeyPredictorConfig c) {
    if (c.num_levels < 1) throw std::invalid_argument("num_levels must be >= 1");
    if (c.default_priority >= c.num_levels) throw std::invalid_argument("default_priority must be < num_levels");
    if (c.min_samples < 1) throw std::invalid_argument("min_samples must be >= 1");
    if (!std::isfinite(c.spread_quantile) || c.spread_quantile <= 0.5 || c.spread_quantile > 1.0)
        throw std::invalid_argument("spread_quantile must be in (0.5, 1]");
    if (!std::isfinite(c.max_spread_ratio) || c.max_spread_ratio < 1.0)
        throw std::invalid_argument("max_spread_ratio must be >= 1");
    if (!std::isfinite(c.max_censored_fraction) || c.max_censored_fraction < 0.0 || c.max_censored_fraction > 1.0)
        throw std::invalid_argument("max_censored_fraction must be in [0, 1]");
    for (double d : {c.decay, c.global_decay})
        if (!std::isfinite(d) || d <= 0.0 || d > 1.0) throw std::invalid_argument("decay must be in (0, 1]");
    if (c.time_half_life.count() < 0 || c.stale_after.count() < 0 || c.idle_eviction.count() < 0 ||
        c.cold_eviction_grace.count() < 0)
        throw std::invalid_argument("durations must be non-negative");
    if (c.histogram_bins < 8 || c.histogram_bins > 1024) throw std::invalid_argument("histogram_bins must be 8..1024");
    if (!std::isfinite(c.min_ms) || !std::isfinite(c.max_ms) || c.min_ms <= 0.0 || c.max_ms <= c.min_ms)
        throw std::invalid_argument("require 0 < min_ms < max_ms");
    if (c.shards < 1 || c.shards > 256) throw std::invalid_argument("shards must be 1..256");
    if (c.max_keys < c.shards) throw std::invalid_argument("max_keys must be >= shards");
    if (c.boundary_refresh_every < 1) throw std::invalid_argument("boundary_refresh_every must be >= 1");
    if (!std::isfinite(c.hysteresis) || c.hysteresis < 0.0 || c.hysteresis >= 1.0)
        throw std::invalid_argument("hysteresis must be in [0, 1)");
    if (c.num_levels == 1 && c.default_priority != 0) throw std::invalid_argument("one level requires default_priority 0");
    if (!c.clock) c.clock = [] { return std::chrono::steady_clock::now(); };
    return c;
}

bool usable_key(std::string_view key) { return !key.empty() && key.size() <= kMaxPredictorKeyBytes; }

} // namespace

std::string_view to_string(PredictionStatus status) noexcept {
    switch (status) {
        case PredictionStatus::Predicted: return "predicted";
        case PredictionStatus::Unready: return "unready";
        case PredictionStatus::ColdKey: return "cold_key";
        case PredictionStatus::HighSpread: return "high_spread";
        case PredictionStatus::Censored: return "censored";
        case PredictionStatus::StaleKey: return "stale_key";
        case PredictionStatus::InvalidKey: return "invalid_key";
    }
    return "unknown";
}

uint8_t tier_of(const std::vector<double>& boundaries, double value) noexcept {
    return static_cast<uint8_t>(std::upper_bound(boundaries.begin(), boundaries.end(), value) - boundaries.begin());
}

DecayingHistogram::DecayingHistogram(std::size_t bins, double min_ms, double max_ms, double decay)
    : counts_(bins, 0.0), min_ms_(min_ms), max_ms_(max_ms),
      ratio_(std::pow(max_ms / min_ms, 1.0 / static_cast<double>(bins - 1))),
      log_ratio_(std::log(ratio_)), gain_(1.0 / decay) {}

std::size_t DecayingHistogram::bin(double value_ms) const noexcept {
    if (value_ms < min_ms_) return 0;
    const auto index = static_cast<std::size_t>(std::log(value_ms / min_ms_) / log_ratio_) + 1;
    return std::min(index, counts_.size() - 1);
}

double DecayingHistogram::lower(std::size_t index) const noexcept {
    return index == 0 ? 0.0 : min_ms_ * std::pow(ratio_, static_cast<double>(index - 1));
}

double DecayingHistogram::upper(std::size_t index) const noexcept {
    return min_ms_ * std::pow(ratio_, static_cast<double>(index));
}

void DecayingHistogram::renormalize_if_needed() {
    if (scale_ <= kRenormalizeAt) return;
    for (double& c : counts_) c /= scale_;
    total_ /= scale_;
    sum_ /= scale_;
    censored_ /= scale_;
    scale_ = 1.0;
}

bool DecayingHistogram::add(double value_ms) {
    const bool clamped = value_ms > max_ms_;
    const double value = clamped ? max_ms_ : value_ms;
    counts_[bin(value)] += scale_;
    total_ += scale_;
    sum_ += scale_ * value;
    ++observations_;
    scale_ *= gain_;
    renormalize_if_needed();
    return clamped;
}

void DecayingHistogram::add_censored() {
    censored_ += scale_;
    scale_ *= gain_;
    renormalize_if_needed();
}

void DecayingHistogram::shrink(double factor) {
    for (double& c : counts_) c *= factor;
    total_ *= factor;
    sum_ *= factor;
    censored_ *= factor;
}

double DecayingHistogram::quantile(double q) const {
    if (total_ <= 0.0) return 0.0;
    const double target = std::clamp(q, 0.0, 1.0) * total_;
    double seen = 0.0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
        if (counts_[i] <= 0.0) continue;
        if (seen + counts_[i] >= target) {
            const double fraction = (target - seen) / counts_[i];
            return lower(i) + fraction * (upper(i) - lower(i));
        }
        seen += counts_[i];
    }
    return upper(counts_.size() - 1);
}

double DecayingHistogram::mean() const { return total_ > 0.0 ? sum_ / total_ : 0.0; }

double DecayingHistogram::censored_fraction() const {
    const double all = total_ + censored_;
    return all > 0.0 ? censored_ / all : 0.0;
}

std::size_t DecayingHistogram::approximate_bytes() const noexcept {
    return sizeof(*this) + counts_.capacity() * sizeof(double);
}

PerKeyPredictor::PerKeyPredictor(PerKeyPredictorConfig config)
    : config_(validated(std::move(config))),
      per_shard_cap_((config_.max_keys + config_.shards - 1) / config_.shards),
      global_(make_histogram(config_.global_decay)) {
    shards_.reserve(config_.shards);
    for (std::size_t i = 0; i < config_.shards; ++i) shards_.push_back(std::make_unique<Shard>());
    if (config_.num_levels == 1) snapshot_.store(std::make_shared<const BoundarySnapshot>());
}

DecayingHistogram PerKeyPredictor::make_histogram(double decay) const {
    return DecayingHistogram(config_.histogram_bins, config_.min_ms, config_.max_ms, decay);
}

PerKeyPredictor::Clock::time_point PerKeyPredictor::now() const { return config_.clock(); }

PerKeyPredictor::Shard& PerKeyPredictor::shard_for(std::string_view key) const {
    return *shards_[std::hash<std::string_view>{}(key) % shards_.size()];
}

std::string PerKeyPredictor::model_version() const {
    const auto snap = snapshot();
    return "per-key-v2;boundaries=" + std::to_string(snap ? snap->sequence : 0);
}

std::shared_ptr<const BoundarySnapshot> PerKeyPredictor::snapshot() const { return snapshot_.load(); }

std::size_t PerKeyPredictor::memory_bound_bytes() const noexcept {
    const std::size_t per_key = make_histogram(config_.decay).approximate_bytes() + sizeof(Entry) +
        2 * (kMaxPredictorKeyBytes + sizeof(std::string) + 3 * sizeof(void*));
    return (per_shard_cap_ * config_.shards) * per_key + make_histogram(config_.global_decay).approximate_bytes();
}

PredictorStats PerKeyPredictor::stats() const {
    PredictorStats out;
    for (const auto& shard : shards_) {
        std::lock_guard lock{shard->mutex};
        out.keys += shard->keys.size();
        out.evictions += shard->evictions;
    }
    {
        std::lock_guard lock{global_mutex_};
        out.snapshots = snapshots_;
        out.degenerate_refreshes = degenerate_;
    }
    out.observations = observed_.load(std::memory_order_relaxed);
    out.rejected = rejected_.load(std::memory_order_relaxed);
    out.overflow_observations = overflow_count_.load(std::memory_order_relaxed);
    out.saturated = saturated_.load(std::memory_order_relaxed);
    out.censored = censored_.load(std::memory_order_relaxed);
    return out;
}

DurationPrediction PerKeyPredictor::fallback(PredictionStatus status, uint64_t seen) const {
    return {std::nullopt, config_.default_priority, status, seen};
}

bool PerKeyPredictor::stale(const Entry& entry, Clock::time_point at) const {
    return config_.stale_after.count() > 0 && at - entry.last_update > config_.stale_after;
}

PerKeyPredictor::Entry* PerKeyPredictor::touch_locked(Shard& shard, std::string_view key, Clock::time_point at) {
    auto found = shard.keys.find(key);
    if (found == shard.keys.end()) {
        if (shard.keys.size() >= per_shard_cap_) {
            // The least recently updated key is recycled only if it is idle: proven keys for idle_eviction, unproven
            // ones for the shorter cold_eviction_grace. Otherwise every tracked key is live and the newcomer is
            // counted as overflow rather than displacing one.
            const auto old = shard.keys.find(*shard.recency.back());
            const bool cold = old->second.histogram.observations() < config_.min_samples;
            const auto limit = cold ? config_.cold_eviction_grace : config_.idle_eviction;
            if (limit.count() <= 0 || at - old->second.last_update < limit) return nullptr;
            shard.recency.pop_back();
            shard.keys.erase(old);
            ++shard.evictions;
        }
        found = shard.keys.emplace(std::string(key),
            Entry{make_histogram(config_.decay), shard.recency.end(), at, at, -1}).first;
        shard.recency.push_front(&found->first);
        found->second.position = shard.recency.begin();
        return &found->second;
    }
    Entry& entry = found->second;
    if (stale(entry, at)) {
        entry.histogram = make_histogram(config_.decay);  // stale evidence is discarded, not blended
        entry.tier = -1;
        entry.last_decay = at;
    } else if (config_.time_half_life.count() > 0) {
        const auto gap = std::chrono::duration<double, std::milli>(at - entry.last_decay).count();
        if (gap >= 1000.0) {
            entry.histogram.shrink(std::pow(0.5, gap / static_cast<double>(config_.time_half_life.count())));
            entry.last_decay = at;
        }
    }
    return &entry;
}

DurationPrediction PerKeyPredictor::evaluate(Entry& entry, const BoundarySnapshot& snapshot) const {
    const auto& histogram = entry.histogram;
    const uint64_t seen = histogram.observations();
    if (seen < config_.min_samples) return fallback(PredictionStatus::ColdKey, seen);
    if (histogram.censored_fraction() > config_.max_censored_fraction)
        return fallback(PredictionStatus::Censored, seen);
    const double p50 = histogram.quantile(0.5);
    const double spread = histogram.quantile(config_.spread_quantile) / std::max(p50, config_.min_ms);
    if (spread > config_.max_spread_ratio) return fallback(PredictionStatus::HighSpread, seen);
    const double estimate = config_.summary == DurationSummary::Median ? p50
        : config_.summary == DurationSummary::P75 ? histogram.quantile(0.75) : histogram.mean();
    if (!std::isfinite(estimate) || estimate < 0.0) return fallback(PredictionStatus::HighSpread, seen);

    const auto& bounds = snapshot.boundaries_ms;
    int tier = tier_of(bounds, estimate);
    if (entry.tier >= 0 && entry.tier != tier && config_.hysteresis > 0.0) {
        const auto held = static_cast<std::size_t>(entry.tier);
        const double low = held == 0 ? 0.0 : bounds[held - 1] * (1.0 - config_.hysteresis);
        const double high = held >= bounds.size() ? std::numeric_limits<double>::infinity()
                                                  : bounds[held] * (1.0 + config_.hysteresis);
        if (estimate >= low && estimate < high) tier = entry.tier;
    }
    entry.tier = tier;
    return {estimate, static_cast<uint8_t>(tier), PredictionStatus::Predicted, seen};
}

DurationPrediction PerKeyPredictor::predict(std::string_view key) {
    if (!usable_key(key)) return fallback(PredictionStatus::InvalidKey, 0);
    const auto snap = snapshot_.load();
    if (!snap) return fallback(PredictionStatus::Unready, 0);
    const auto at = now();
    auto& shard = shard_for(key);
    std::lock_guard lock{shard.mutex};
    const auto found = shard.keys.find(key);
    if (found == shard.keys.end()) return fallback(PredictionStatus::ColdKey, 0);
    if (stale(found->second, at)) return fallback(PredictionStatus::StaleKey, found->second.histogram.observations());
    return evaluate(found->second, *snap);
}

bool PerKeyPredictor::observe(std::string_view key, double duration_ms) {
    if (!usable_key(key) || !std::isfinite(duration_ms) || duration_ms < 0.0) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto at = now();
    {
        auto& shard = shard_for(key);
        std::lock_guard lock{shard.mutex};
        if (Entry* entry = touch_locked(shard, key, at)) {
            entry->histogram.add(duration_ms);
            entry->last_update = at;
            shard.recency.splice(shard.recency.begin(), shard.recency, entry->position);
        } else {
            overflow_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    observed_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock{global_mutex_};
    if (global_.add(duration_ms)) saturated_.fetch_add(1, std::memory_order_relaxed);
    ++global_observations_;
    // First snapshot as soon as the global histogram is trusted, then every boundary_refresh_every samples.
    if (config_.num_levels > 1 && global_observations_ >= config_.global_min_samples &&
        (!snapshot_.load() || global_observations_ % config_.boundary_refresh_every == 0))
        refresh_boundaries_locked();
    return true;
}

bool PerKeyPredictor::observe_censored(std::string_view key) {
    if (!usable_key(key)) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto at = now();
    auto& shard = shard_for(key);
    std::lock_guard lock{shard.mutex};
    Entry* entry = touch_locked(shard, key, at);
    if (!entry) {
        overflow_count_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    entry->histogram.add_censored();
    entry->last_update = at;
    shard.recency.splice(shard.recency.begin(), shard.recency, entry->position);
    censored_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void PerKeyPredictor::refresh_boundaries_locked() {
    const std::size_t count = config_.num_levels - 1;
    std::vector<double> fresh(count);
    for (std::size_t i = 0; i < count; ++i)
        fresh[i] = global_.quantile(static_cast<double>(i + 1) / config_.num_levels);
    const auto valid = [](const std::vector<double>& b) {
        return b.front() > 0.0 && std::is_sorted(b.begin(), b.end(), std::less_equal<double>{});
    };
    if (!valid(fresh)) {
        ++degenerate_;
        return;
    }
    const auto current = snapshot_.load();
    if (current) {
        auto held = fresh;
        for (std::size_t i = 0; i < count; ++i) {
            const double old = current->boundaries_ms[i];
            if (std::abs(fresh[i] - old) <= config_.hysteresis * old) held[i] = old;
        }
        if (valid(held)) fresh = std::move(held);
        if (fresh == current->boundaries_ms) return;
    }
    snapshot_.store(std::make_shared<const BoundarySnapshot>(BoundarySnapshot{++snapshots_, std::move(fresh)}));
}

std::optional<std::string> derive_predictor_key(
    const std::unordered_map<std::string, std::string>& headers, const PredictorKeyPolicy& policy) {
    const auto text = [&](const std::string& name) -> const std::string* {
        if (name.empty()) return nullptr;
        const auto found = headers.find(name);
        return found == headers.end() || found->second.empty() ? nullptr : &found->second;
    };
    const std::string* job = text(policy.job_header);
    if (job && (job->size() > kMaxJobKeyBytes ||
        std::any_of(job->begin(), job->end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; })))
        job = nullptr;  // an unusable job label falls back to the producer scope rather than a hostile key
    const std::string* producer = text(policy.producer_header);
    std::string key;
    if (policy.scope_by_producer) {
        if (!producer) return std::nullopt;
        key = *producer;
        if (job) key += '\x1f' + *job;
    } else if (job) {
        key = *job;
    } else if (producer) {
        key = *producer;
    } else {
        return std::nullopt;
    }
    if (key.size() > kMaxPredictorKeyBytes) return std::nullopt;
    return key;
}

} // namespace harbinger::ml
