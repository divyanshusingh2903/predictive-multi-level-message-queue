#include "ml/duration_predictor.hpp"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unistd.h>

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
    return config_.stale_after.count() > 0 && at - entry.last_activity > config_.stale_after;
}

PerKeyPredictor::Entry* PerKeyPredictor::touch_locked(Shard& shard, std::string_view key, Clock::time_point at) {
    auto found = shard.keys.find(key);
    if (found == shard.keys.end()) {
        if (shard.keys.size() >= per_shard_cap_) {
            // The least recently active key is recycled only if it is idle: proven keys for idle_eviction, unproven
            // ones for the shorter cold_eviction_grace. Otherwise every tracked key is live and the newcomer is
            // counted as overflow rather than displacing one.
            const auto old = shard.keys.find(*shard.recency.back());
            const bool cold = old->second.histogram.observations() < config_.min_samples;
            const auto limit = cold ? config_.cold_eviction_grace : config_.idle_eviction;
            if (limit.count() <= 0 || at - old->second.last_activity < limit) return nullptr;
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
    Entry& entry = found->second;
    if (stale(entry, at)) return fallback(PredictionStatus::StaleKey, entry.histogram.observations());
    // A fresh key that is still receiving work is active even when none of it has completed yet (a backlog), so its
    // history must not age out underneath it. A stale key is not revived here; only new evidence does that.
    entry.last_activity = at;
    shard.recency.splice(shard.recency.begin(), shard.recency, entry.position);
    return evaluate(entry, *snap);
}

bool PerKeyPredictor::record_key(std::string_view key, double duration_ms, Clock::time_point at) {
    auto& shard = shard_for(key);
    std::lock_guard lock{shard.mutex};
    Entry* entry = touch_locked(shard, key, at);
    if (!entry) {
        overflow_count_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    entry->histogram.add(duration_ms);
    entry->last_activity = at;
    shard.recency.splice(shard.recency.begin(), shard.recency, entry->position);
    return true;
}

bool PerKeyPredictor::observe(std::string_view key, double duration_ms) {
    return observe(key, std::string_view{}, duration_ms);
}

bool PerKeyPredictor::observe(std::string_view key, std::string_view parent, double duration_ms) {
    if (!usable_key(key) || (!parent.empty() && !usable_key(parent)) || !std::isfinite(duration_ms) ||
        duration_ms < 0.0) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto at = now();
    // Shard locks are taken one at a time, never nested.
    record_key(key, duration_ms, at);
    if (!parent.empty()) record_key(parent, duration_ms, at);
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
    entry->last_activity = at;
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

// Snapshots

namespace harbinger::ml {

namespace {

constexpr char kSnapshotMagic[8] = {'H', 'B', 'P', 'K', 'S', 'N', 'P', '1'};
constexpr uint32_t kSnapshotVersion = 1;
constexpr uint32_t kByteOrder = 0x01020304;
constexpr std::uintmax_t kMaxSnapshotBytes = 1ull << 30;

uint64_t fnv1a(std::string_view data) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : data) { hash ^= c; hash *= 1099511628211ull; }
    return hash;
}

class Writer {
public:
    template <class T> void put(T value) {
        static_assert(std::is_trivially_copyable_v<T>);
        out.append(reinterpret_cast<const char*>(&value), sizeof value);
    }
    void bytes(std::string_view text) { put<uint32_t>(static_cast<uint32_t>(text.size())); out.append(text); }
    std::string out;
};

class Reader {
public:
    explicit Reader(std::string_view data) : data_(data) {}
    template <class T> T get() {
        static_assert(std::is_trivially_copyable_v<T>);
        if (data_.size() - pos_ < sizeof(T)) throw std::runtime_error("snapshot truncated");
        T value;
        std::memcpy(&value, data_.data() + pos_, sizeof value);
        pos_ += sizeof value;
        return value;
    }
    double finite() {
        const double value = get<double>();
        if (!std::isfinite(value) || value < 0.0) throw std::runtime_error("snapshot contains an invalid number");
        return value;
    }
    std::string bytes(std::size_t limit) {
        const auto size = get<uint32_t>();
        if (size > limit || data_.size() - pos_ < size) throw std::runtime_error("snapshot string out of bounds");
        std::string text(data_.substr(pos_, size));
        pos_ += size;
        return text;
    }
    [[nodiscard]] bool done() const noexcept { return pos_ == data_.size(); }
private:
    std::string_view data_;
    std::size_t pos_{0};
};

void write_file_atomically(const std::filesystem::path& path, const std::string& data) {
    const auto temporary = std::filesystem::path(path.string() + ".tmp");
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("cannot create snapshot: " + std::string(std::strerror(errno)));
    std::size_t written = 0;
    while (written < data.size()) {
        const auto n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ::close(fd); throw std::runtime_error("cannot write snapshot"); }
        written += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) { ::close(fd); throw std::runtime_error("cannot sync snapshot"); }
    if (::close(fd) != 0) throw std::runtime_error("cannot close snapshot");
    std::error_code ignored;
    if (std::filesystem::exists(path)) std::filesystem::rename(path, path.string() + ".prev", ignored);
    std::filesystem::rename(temporary, path);
    if (const int dir = ::open(path.parent_path().empty() ? "." : path.parent_path().c_str(), O_RDONLY | O_CLOEXEC); dir >= 0) {
        ::fsync(dir);
        ::close(dir);
    }
}

} // namespace

void PerKeyPredictor::save(const std::filesystem::path& path, std::string_view context) const {
    struct Copy { std::string key; DecayingHistogram histogram; int64_t age_ms; int32_t tier; };
    const auto at = now();
    std::vector<Copy> keys;
    for (const auto& shard : shards_) {
        std::lock_guard lock{shard->mutex};
        for (const auto& [key, entry] : shard->keys)
            keys.push_back({key, entry.histogram,
                std::chrono::duration_cast<std::chrono::milliseconds>(at - entry.last_activity).count(), entry.tier});
    }
    std::optional<DecayingHistogram> global;
    uint64_t global_observations = 0;
    {
        std::lock_guard lock{global_mutex_};
        global = global_;
        global_observations = global_observations_;
    }
    const auto snap = snapshot_.load();

    Writer w;
    w.out.append(kSnapshotMagic, sizeof kSnapshotMagic);
    w.put(kSnapshotVersion);
    w.put(kByteOrder);
    w.bytes(context);
    w.put<uint32_t>(static_cast<uint32_t>(config_.histogram_bins));
    w.put(config_.min_ms);
    w.put(config_.max_ms);
    w.put<uint32_t>(config_.num_levels);
    const auto histogram = [&w](const DecayingHistogram& h) {
        for (double c : h.counts_) w.put(c);
        w.put(h.scale_); w.put(h.total_); w.put(h.sum_); w.put(h.censored_);
        w.put<uint64_t>(h.observations_);
    };
    w.put<uint8_t>(snap ? 1 : 0);
    if (snap) {
        w.put<uint64_t>(snap->sequence);
        w.put<uint32_t>(static_cast<uint32_t>(snap->boundaries_ms.size()));
        for (double b : snap->boundaries_ms) w.put(b);
    }
    histogram(*global);
    w.put<uint64_t>(global_observations);
    w.put<uint64_t>(keys.size());
    for (const auto& k : keys) {
        w.bytes(k.key);
        histogram(k.histogram);
        w.put<int64_t>(std::max<int64_t>(0, k.age_ms));
        w.put<int32_t>(k.tier);
    }
    w.put<uint64_t>(fnv1a(w.out));
    write_file_atomically(path, w.out);
}

std::size_t PerKeyPredictor::load(const std::filesystem::path& path, std::string_view context) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) throw std::runtime_error("cannot read snapshot: " + error.message());
    if (size > kMaxSnapshotBytes || size < sizeof kSnapshotMagic + 8) throw std::runtime_error("snapshot size out of bounds");
    std::string data(size, '\0');
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.read(data.data(), static_cast<std::streamsize>(size))) throw std::runtime_error("cannot read snapshot");
    }
    const std::string_view body(data.data(), data.size() - sizeof(uint64_t));
    uint64_t checksum = 0;
    std::memcpy(&checksum, data.data() + body.size(), sizeof checksum);
    if (std::memcmp(data.data(), kSnapshotMagic, sizeof kSnapshotMagic) != 0) throw std::runtime_error("not a predictor snapshot");
    if (fnv1a(body) != checksum) throw std::runtime_error("snapshot checksum mismatch");

    Reader r(body.substr(sizeof kSnapshotMagic));
    if (r.get<uint32_t>() != kSnapshotVersion) throw std::runtime_error("unsupported snapshot version");
    if (r.get<uint32_t>() != kByteOrder) throw std::runtime_error("snapshot byte order differs");
    if (r.bytes(4096) != context) throw std::runtime_error("snapshot was taken under a different key policy or routing version");
    if (r.get<uint32_t>() != config_.histogram_bins || r.get<double>() != config_.min_ms ||
        r.get<double>() != config_.max_ms || r.get<uint32_t>() != config_.num_levels)
        throw std::runtime_error("snapshot histogram layout or level count differs");
    const auto histogram = [&](double decay) {
        auto h = make_histogram(decay);
        double sum = 0.0;
        for (double& c : h.counts_) { c = r.finite(); sum += c; }
        h.scale_ = r.finite(); h.total_ = r.finite(); h.sum_ = r.finite(); h.censored_ = r.finite();
        h.observations_ = r.get<uint64_t>();
        if (h.scale_ < 1.0 || h.scale_ > kRenormalizeAt * 2 || std::abs(sum - h.total_) > 1e-6 * std::max(1.0, h.total_))
            throw std::runtime_error("snapshot histogram is inconsistent");
        return h;
    };
    std::shared_ptr<const BoundarySnapshot> boundaries;
    if (r.get<uint8_t>()) {
        BoundarySnapshot b;
        b.sequence = r.get<uint64_t>();
        const auto count = r.get<uint32_t>();
        if (count != static_cast<uint32_t>(config_.num_levels - 1)) throw std::runtime_error("snapshot boundary count differs");
        for (uint32_t i = 0; i < count; ++i) b.boundaries_ms.push_back(r.finite());
        if (count && (b.boundaries_ms.front() <= 0.0 ||
            !std::is_sorted(b.boundaries_ms.begin(), b.boundaries_ms.end(), std::less_equal<double>{})))
            throw std::runtime_error("snapshot boundaries are not positive and increasing");
        boundaries = std::make_shared<const BoundarySnapshot>(std::move(b));
    }
    auto global = histogram(config_.global_decay);
    const auto global_observations = r.get<uint64_t>();
    const auto key_count = r.get<uint64_t>();
    if (key_count > config_.max_keys + config_.shards) throw std::runtime_error("snapshot holds more keys than the cap");
    struct Restored { std::string key; DecayingHistogram histogram; int64_t age_ms; int32_t tier; };
    std::vector<Restored> keys;
    keys.reserve(key_count);
    for (uint64_t i = 0; i < key_count; ++i) {
        auto key = r.bytes(kMaxPredictorKeyBytes);
        if (key.empty()) throw std::runtime_error("snapshot contains an empty key");
        auto h = histogram(config_.decay);
        const auto age = r.get<int64_t>();
        const auto tier = r.get<int32_t>();
        if (age < 0 || tier < -1 || tier >= config_.num_levels) throw std::runtime_error("snapshot key metadata out of range");
        keys.push_back({std::move(key), std::move(h), age, tier});
    }
    if (!r.done()) throw std::runtime_error("snapshot has trailing data");

    // Validation is complete; apply.
    const auto at = now();
    std::size_t restored = 0;
    for (auto& shard : shards_) {
        std::lock_guard lock{shard->mutex};
        shard->keys.clear();
        shard->recency.clear();
    }
    // Oldest first so recency order (front = most recent) survives the restore.
    std::sort(keys.begin(), keys.end(), [](const Restored& a, const Restored& b) { return a.age_ms > b.age_ms; });
    for (auto& k : keys) {
        auto& shard = shard_for(k.key);
        std::lock_guard lock{shard.mutex};
        if (shard.keys.size() >= per_shard_cap_ || shard.keys.contains(k.key)) continue;
        const auto updated = at - std::chrono::milliseconds(k.age_ms);
        auto found = shard.keys.emplace(std::move(k.key), Entry{std::move(k.histogram), shard.recency.end(), updated, updated, k.tier}).first;
        shard.recency.push_front(&found->first);
        found->second.position = shard.recency.begin();
        ++restored;
    }
    {
        std::lock_guard lock{global_mutex_};
        global_ = std::move(global);
        global_observations_ = global_observations;
        if (boundaries) snapshots_ = std::max(snapshots_, boundaries->sequence);
    }
    if (boundaries || config_.num_levels > 1) snapshot_.store(boundaries);
    return restored;
}

std::string size_policy_label(const PredictorKeyPolicy& policy) {
    switch (policy.size_source) {
        case PredictorKeyPolicy::SizeSource::None: return "";
        case PredictorKeyPolicy::SizeSource::Payload: return "|size=payload";
        case PredictorKeyPolicy::SizeSource::Header: return "|size=header:" + policy.size_header;
    }
    return "";
}

std::string size_bin_label(double size) {
    const double clamped = std::max(size, 1.0);
    const int bin = std::min(63, static_cast<int>(std::floor(std::log2(clamped))));
    return "s" + std::to_string(bin);
}

std::optional<PredictorKeys> derive_predictor_keys(
    const std::unordered_map<std::string, std::string>& headers, const PredictorKeyPolicy& policy,
    uint64_t payload_size_bytes) {
    auto key = derive_predictor_key(headers, policy);
    if (!key) return std::nullopt;
    std::optional<double> size;
    if (policy.size_source == PredictorKeyPolicy::SizeSource::Payload) {
        size = static_cast<double>(payload_size_bytes);
    } else if (policy.size_source == PredictorKeyPolicy::SizeSource::Header && !policy.size_header.empty()) {
        const auto found = headers.find(policy.size_header);
        if (found != headers.end()) size = parse_size_hint(found->second);
    }
    return bin_predictor_key(std::move(*key), size);
}

std::optional<double> parse_size_hint(std::string_view text) {
    if (text.empty() || text.size() > 32) return std::nullopt;
    double value = 0;
    const auto* end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(value) || value < 0) return std::nullopt;
    return value;
}

PredictorKeys bin_predictor_key(std::string key, std::optional<double> size) {
    if (!size) return {std::move(key), std::nullopt};
    auto binned = key + '\x1f' + size_bin_label(*size);
    if (binned.size() > kMaxPredictorKeyBytes) return {std::move(key), std::nullopt};
    return {std::move(binned), std::move(key)};
}

} // namespace harbinger::ml
