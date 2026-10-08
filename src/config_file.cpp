#include "config_file.hpp"

#include "util/json.hpp"

#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace harbinger {

namespace {

using json::Json;
using json::Object;

/// Strict field reader: every lookup is recorded so leftover (unknown) fields can be rejected.
class Section {
public:
    Section(const Json& value, std::string path) : path_(std::move(path)) {
        object_ = std::get_if<Object>(&value.value);
        if (!object_) fail("must be an object");
    }
    ~Section() = default;
    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;

    [[noreturn]] void fail(const std::string& why) const { throw std::invalid_argument(path_ + ": " + why); }

    const Json* find(const std::string& key) {
        seen_.insert(key);
        const auto found = object_->find(key);
        return found == object_->end() ? nullptr : &found->second;
    }
    bool has(const std::string& key) { return find(key) != nullptr; }
    bool is_null(const std::string& key) {
        const Json* v = find(key);
        return v && std::holds_alternative<std::nullptr_t>(v->value);
    }

    uint64_t unsigned_value(const std::string& key, uint64_t low, uint64_t high) {
        const Json* v = find(key);
        const auto* i = v ? std::get_if<int64_t>(&v->value) : nullptr;
        if (!i || *i < 0 || static_cast<uint64_t>(*i) < low || static_cast<uint64_t>(*i) > high)
            fail(key + " must be an integer in [" + std::to_string(low) + ", " + std::to_string(high) + "]");
        return static_cast<uint64_t>(*i);
    }
    template <class T> void read(const std::string& key, T& out, uint64_t low, uint64_t high) {
        if (has(key)) out = static_cast<T>(unsigned_value(key, low, high));
    }
    void read_ms(const std::string& key, std::chrono::milliseconds& out, uint64_t low = 0,
                 uint64_t high = 365ull * 24 * 3600 * 1000) {
        if (has(key)) out = std::chrono::milliseconds(static_cast<int64_t>(unsigned_value(key, low, high)));
    }
    void read_double(const std::string& key, double& out, double low, double high) {
        if (!has(key)) return;
        const auto n = find(key)->number();
        if (!n || !std::isfinite(*n) || *n < low || *n > high)
            fail(key + " must be a number in [" + std::to_string(low) + ", " + std::to_string(high) + "]");
        out = *n;
    }
    void read_bool(const std::string& key, bool& out) {
        if (!has(key)) return;
        const auto* b = std::get_if<bool>(&find(key)->value);
        if (!b) fail(key + " must be true or false");
        out = *b;
    }
    std::string string(const std::string& key, std::size_t max_bytes = 4096) {
        const Json* v = find(key);
        if (!v || !v->string() || v->string()->empty() || v->string()->size() > max_bytes)
            fail(key + " must be a nonempty string of at most " + std::to_string(max_bytes) + " bytes");
        return *v->string();
    }
    void read_string(const std::string& key, std::string& out) { if (has(key)) out = string(key); }
    Section child(const std::string& key) {
        const Json* v = find(key);
        if (!v) fail(key + " is required");
        return Section(*v, path_ + "." + key);
    }
    const Json& raw(const std::string& key) {
        const Json* v = find(key);
        if (!v) fail(key + " is required");
        return *v;
    }
    /// Call after reading every supported field.
    void finish() const {
        for (const auto& [key, value] : *object_)
            if (!seen_.contains(key)) fail("unknown field '" + key + "'");
    }

private:
    const Object* object_{nullptr};
    std::string path_;
    std::set<std::string> seen_;
};

void apply_broker(Section s, HarbingerConfig& c) {
    s.read("num_levels", c.num_levels, 1, 255);
    s.read("default_priority", c.default_priority, 0, 254);
    s.read("default_max_retries", c.default_max_retries, 1, 1000000);
    s.read_ms("default_ttl_ms", c.default_ttl);
    s.read_ms("max_pull_wait_ms", c.max_pull_wait, 1);
    s.read_ms("ttl_sweep_interval_ms", c.ttl_sweep_interval);
    s.read_ms("delivery_lease_ms", c.delivery_lease, 1);
    s.read_ms("lease_sweep_interval_ms", c.lease_sweep_interval, 1);
    s.read_ms("completion_retention_ms", c.completion_retention, 1);
    s.read("completion_cache_max_entries", c.completion_cache_max_entries, 1, 1u << 30);
    s.read("maintenance_batch_size", c.maintenance_batch_size, 1, 1u << 20);
    if (s.has("level_weights")) {
        if (s.is_null("level_weights")) c.level_weights.reset();
        else {
            const auto* list = std::get_if<json::Array>(&s.raw("level_weights").value);
            if (!list || list->empty() || list->size() > 255) s.fail("level_weights must be an array of 1 to 255 integers");
            std::vector<uint32_t> weights;
            for (const auto& item : *list) {
                const auto* w = std::get_if<int64_t>(&item.value);
                if (!w || *w < 1 || *w > 1000) s.fail("level_weights entries must be integers in [1, 1000]");
                weights.push_back(static_cast<uint32_t>(*w));
            }
            c.level_weights = std::move(weights);
        }
    }
    if (s.has("aging")) {
        if (s.is_null("aging")) c.aging.reset();
        else {
            auto a = s.child("aging");
            AgingConfig aging = c.aging.value_or(AgingConfig{});
            a.read_ms("threshold_ms", aging.threshold, 1);
            a.read_ms("interval_ms", aging.interval, 1);
            if (a.has("pause_when_behind")) {
                bool pause = false;
                a.read_bool("pause_when_behind", pause);
                aging.pause_when_behind = pause;
            }
            a.finish();
            c.aging = aging;
        }
    }
    s.finish();
}

void apply_features(Section s, HarbingerConfig& c) {
    ml::IngressFeatureConfig features;
    features.schema.version = s.string("schema_version", 128);
    features.routing_policy_version = s.string("routing_policy_version", 128);
    if (s.has("headers")) {
        const auto* list = std::get_if<json::Array>(&s.raw("headers").value);
        if (!list || list->size() > ml::kMaxHeaders) s.fail("headers must be an array of at most 16 objects");
        for (std::size_t i = 0; i < list->size(); ++i) {
            Section h((*list)[i], "features.headers[" + std::to_string(i) + "]");
            ml::HeaderFeature field;
            field.name = h.string("name", ml::kMaxKeyBytes);
            const auto type = h.string("type");
            if (type == "numeric") {
                field.type = ml::FeatureType::Numeric;
                h.read_double("minimum", field.minimum, -1e300, 1e300);
                h.read_double("maximum", field.maximum, -1e300, 1e300);
            } else if (type == "categorical") {
                field.type = ml::FeatureType::Categorical;
                std::string encoding = "vocabulary";
                h.read_string("encoding", encoding);
                if (encoding == "hash") field.encoding = ml::CategoricalEncoding::Hash;
                else if (encoding != "vocabulary") h.fail("encoding must be vocabulary or hash");
                if (h.has("vocabulary")) {
                    const auto* values = std::get_if<json::Array>(&h.raw("vocabulary").value);
                    if (!values || values->size() > ml::kMaxVocabulary) h.fail("vocabulary must be an array of strings");
                    for (const auto& v : *values) {
                        if (!v.string()) h.fail("vocabulary must contain only strings");
                        field.vocabulary.push_back(*v.string());
                    }
                }
            } else {
                h.fail("type must be numeric or categorical");
            }
            h.finish();
            features.schema.headers.push_back(std::move(field));
        }
    }
    s.finish();
    ml::FeatureExtractor validate{features.schema};  // reuse the broker's own schema validation now, with context
    c.ingress_features = std::move(features);
}

void apply_feedback(Section s, HarbingerConfig& c) {
    ml::FeedbackConfig f;
    f.path = s.string("directory");
    s.read("buffer_records", f.buffer_records, 1, 1u << 24);
    s.read("buffer_bytes", f.buffer_bytes, 1, 1ull << 34);
    s.read("segment_bytes", f.segment_bytes, 1, 1ull << 34);
    s.read("retention_bytes", f.retention_bytes, 1, 1ull << 44);
    s.read("max_segments", f.max_segments, 1, 1u << 20);
    s.read_ms("retention_age_ms", f.retention_age, 1);
    s.read_ms("sync_interval_ms", f.sync_interval, 1);
    s.read_ms("shutdown_drain_ms", f.shutdown_drain, 1);
    s.finish();
    c.feedback = std::move(f);
}

void apply_predictor(Section s, ml::PerKeyPredictorConfig& p) {
    if (s.has("summary")) {
        const auto summary = s.string("summary");
        if (summary == "median") p.summary = ml::DurationSummary::Median;
        else if (summary == "p75") p.summary = ml::DurationSummary::P75;
        else if (summary == "mean") p.summary = ml::DurationSummary::Mean;
        else s.fail("summary must be median, p75 or mean");
    }
    s.read("min_samples", p.min_samples, 1, 1u << 30);
    s.read_double("spread_quantile", p.spread_quantile, 0.5, 1.0);
    s.read_double("max_spread_ratio", p.max_spread_ratio, 1.0, 1e9);
    s.read_double("max_censored_fraction", p.max_censored_fraction, 0.0, 1.0);
    s.read_double("decay", p.decay, 1e-9, 1.0);
    s.read_double("global_decay", p.global_decay, 1e-9, 1.0);
    s.read_ms("time_half_life_ms", p.time_half_life);
    s.read_ms("stale_after_ms", p.stale_after);
    s.read("histogram_bins", p.histogram_bins, 8, 1024);
    s.read_double("min_ms", p.min_ms, 1e-6, 1e12);
    s.read_double("max_ms", p.max_ms, 1e-6, 1e12);
    s.read("max_keys", p.max_keys, 1, 1u << 24);
    s.read("shards", p.shards, 1, 256);
    s.read_ms("idle_eviction_ms", p.idle_eviction);
    s.read_ms("cold_eviction_grace_ms", p.cold_eviction_grace);
    s.read("boundary_refresh_every", p.boundary_refresh_every, 1, 1u << 30);
    s.read("global_min_samples", p.global_min_samples, 0, 1u << 30);
    s.read_double("hysteresis", p.hysteresis, 0.0, 0.999);
    s.finish();
}

void apply_routing(Section s, HarbingerConfig& c) {
    ml::PredictiveRoutingConfig r;
    const auto mode = s.string("mode");
    if (mode == "shadow") r.mode = ml::RoutingMode::Shadow;
    else if (mode == "predictive") r.mode = ml::RoutingMode::Predictive;
    else s.fail("mode must be shadow or predictive (omit the routing section for static routing)");
    s.read_string("routing_policy_version", r.routing_policy_version);
    if (s.has("key")) {
        auto k = s.child("key");
        if (k.has("job_header")) {
            if (k.is_null("job_header")) r.key.job_header.clear();
            else r.key.job_header = k.string("job_header", ml::kMaxKeyBytes);
        }
        k.read_bool("scope_by_producer", r.key.scope_by_producer);
        if (k.has("size_source")) {
            const auto source = k.string("size_source");
            if (source == "none") r.key.size_source = ml::PredictorKeyPolicy::SizeSource::None;
            else if (source == "payload") r.key.size_source = ml::PredictorKeyPolicy::SizeSource::Payload;
            else if (source == "header") r.key.size_source = ml::PredictorKeyPolicy::SizeSource::Header;
            else k.fail("size_source must be none, payload or header");
        }
        if (k.has("size_header")) r.key.size_header = k.string("size_header", ml::kMaxKeyBytes);
        k.finish();
    }
    if (s.has("predictor")) apply_predictor(s.child("predictor"), r.predictor);
    if (s.has("snapshot")) {
        auto snap = s.child("snapshot");
        r.snapshot_path = snap.string("path");
        snap.read_ms("interval_ms", r.snapshot_interval, 1);
        snap.finish();
    }
    s.finish();
    c.predictive_routing = std::move(r);
}

} // namespace

void apply_config_document(std::string_view document, HarbingerConfig& config, ServerSettings& server) {
    if (document.size() > kMaxConfigFileBytes) throw std::invalid_argument("configuration exceeds 1 MiB");
    const Json root = json::Parser(document).parse();
    HarbingerConfig candidate = config;
    ServerSettings settings = server;
    Section s(root, "config");
    if (!s.has("config_version") || s.unsigned_value("config_version", 1, 1) != 1)
        s.fail("config_version must be 1");
    s.read_string("listen", settings.listen_address);
    s.read_ms("stats_interval_ms", settings.stats_interval);
    if (s.has("broker")) apply_broker(s.child("broker"), candidate);
    // Absent section: unchanged. null: explicitly disabled.
    if (s.has("features")) {
        if (s.is_null("features")) candidate.ingress_features.reset();
        else apply_features(s.child("features"), candidate);
    }
    if (s.has("feedback")) {
        if (s.is_null("feedback")) candidate.feedback.reset();
        else apply_feedback(s.child("feedback"), candidate);
    }
    if (s.has("routing")) {
        if (s.is_null("routing")) candidate.predictive_routing.reset();
        else apply_routing(s.child("routing"), candidate);
    }
    s.finish();
    config = std::move(candidate);
    server = std::move(settings);
}

void apply_config_file(const std::filesystem::path& path, HarbingerConfig& config, ServerSettings& server) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) throw std::invalid_argument("configuration file not found: " + path.string());
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > kMaxConfigFileBytes) throw std::invalid_argument("configuration file unreadable or larger than 1 MiB");
    std::ifstream in(path, std::ios::binary);
    std::string text(size, '\0');
    if (!in.read(text.data(), static_cast<std::streamsize>(size))) throw std::invalid_argument("cannot read configuration file");
    try {
        apply_config_document(text, config, server);
    } catch (const std::exception& e) {
        throw std::invalid_argument(path.string() + ": " + e.what());
    }
}

std::string describe_config(const HarbingerConfig& c, const ServerSettings& server) {
    std::ostringstream out;
    out << "listen=" << server.listen_address << " levels=" << int(c.num_levels)
        << " default_priority=" << int(c.default_priority) << " max_retries=" << c.default_max_retries
        << " default_ttl_ms=" << c.default_ttl.count() << " aging="
        << (c.aging ? std::to_string(c.aging->threshold.count()) + "/" + std::to_string(c.aging->interval.count()) + "ms" +
                (c.aging->pause_when_behind.value_or(c.level_weights.has_value()) ? "(pausing)" : "") : "off")
        << " level_weights=" << [&] {
               if (!c.level_weights) return std::string("off");
               std::string text;
               for (const auto w : *c.level_weights) text += (text.empty() ? "" : "/") + std::to_string(w);
               return text;
           }()
        << " delivery_lease_ms=" << c.delivery_lease.count()
        << " features=" << (c.ingress_features ? c.ingress_features->schema.version : "off")
        << " feedback=" << (c.feedback ? c.feedback->path.string() : "off") << " routing=";
    if (!c.predictive_routing) out << "static";
    else {
        const auto& r = *c.predictive_routing;
        out << (r.mode == ml::RoutingMode::Predictive ? "predictive" : "shadow") << " policy=" << r.routing_policy_version
            << " key=" << (r.key.scope_by_producer ? "producer" : "") << (r.key.job_header.empty() ? "" : "+" + r.key.job_header)
            << (r.key.size_source == ml::PredictorKeyPolicy::SizeSource::Payload ? "+size(payload)"
                : r.key.size_source == ml::PredictorKeyPolicy::SizeSource::Header ? "+size(" + r.key.size_header + ")" : "")
            << " snapshot=" << (r.snapshot_path ? r.snapshot_path->string() : "off");
    }
    out << " stats_interval_ms=" << server.stats_interval.count();
    return out.str();
}

std::string stats_json(const HarbingerService& service) {
    std::ostringstream out;
    out << "{\"queued\":" << service.queue_size() << ",\"in_flight\":" << service.in_flight_count()
        << ",\"dlq\":" << service.dlq_size();
    const auto f = service.feedback_stats();
    uint64_t dropped = 0;
    for (auto d : f.dropped) dropped += d;
    out << ",\"feedback\":{\"enabled\":" << (f.enabled ? "true" : "false") << ",\"written\":" << f.written
        << ",\"dropped\":" << dropped << ",\"pending\":" << f.pending_records << "}";
    if (const auto share = service.level_share()) {
        const auto list = [&](const auto& values) {
            out << '[';
            for (std::size_t i = 0; i < values.size(); ++i) out << (i ? "," : "") << values[i];
            out << ']';
        };
        out << ",\"levels\":{\"weights\":"; list(share->weights);
        out << ",\"pulls\":"; list(share->pulls);
        out << ",\"charged_ms\":"; list(share->charged_ms);
        out << ",\"average_cost_ms\":"; list(share->average_cost_ms);
        out << "}";
    }
    const auto r = service.routing_stats();
    out << ",\"routing\":{\"enabled\":" << (r.enabled ? "true" : "false");
    if (r.enabled) {
        static constexpr const char* names[] = {"predicted", "unready", "cold_key", "high_spread", "censored",
                                                "stale_key", "invalid_key"};
        out << ",\"lookups\":" << r.lookups << ",\"routed\":" << r.routed_by_prediction
            << ",\"parent_fallbacks\":" << r.parent_fallbacks << ",\"outcomes\":{";
        for (std::size_t i = 0; i < r.outcomes.size(); ++i) out << (i ? "," : "") << '"' << names[i] << "\":" << r.outcomes[i];
        const double mean_us = r.lookups ? static_cast<double>(r.latency_ns_total) / static_cast<double>(r.lookups) / 1000.0 : 0.0;
        out << "},\"lookup_mean_us\":" << mean_us << ",\"lookup_max_us\":" << static_cast<double>(r.latency_ns_max) / 1000.0
            << ",\"learned\":" << r.learned << ",\"censored\":" << r.censored_observed
            << ",\"scored\":" << r.scored << ",\"tier_agreement\":"
            << (r.scored ? static_cast<double>(r.tier_matches) / static_cast<double>(r.scored) : 0.0)
            << ",\"abs_log2_error\":" << r.abs_log2_error_ewma << ",\"drift_alerts\":" << r.drift_alerts
            << ",\"keys\":" << r.predictor.keys << ",\"overflow\":" << r.predictor.overflow_observations
            << ",\"evictions\":" << r.predictor.evictions << ",\"snapshot_saves\":" << r.snapshot_saves
            << ",\"snapshot_failures\":" << r.snapshot_failures;
    }
    out << "}}";
    return out.str();
}

} // namespace harbinger
