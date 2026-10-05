// Replays a temporal feedback export through the broker's PerKeyPredictor and emits JSONL predictions.
#include "ml/duration_predictor.hpp"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

using namespace harbinger::ml;

namespace {

struct Json;
using Object = std::map<std::string, Json>;
using Array = std::vector<Json>;
struct Json {
    std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object> value{nullptr};
    const Json* get(const std::string& key) const {
        const auto* object = std::get_if<Object>(&value);
        if (!object) return nullptr;
        const auto found = object->find(key);
        return found == object->end() ? nullptr : &found->second;
    }
    const std::string* string() const { return std::get_if<std::string>(&value); }
    std::optional<double> number() const {
        if (const auto* i = std::get_if<int64_t>(&value)) return static_cast<double>(*i);
        if (const auto* d = std::get_if<double>(&value)) return *d;
        return std::nullopt;
    }
    std::optional<int64_t> integer() const {
        if (const auto* i = std::get_if<int64_t>(&value)) return *i;
        return std::nullopt;
    }
};

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}
    Json parse() {
        Json out = value();
        skip();
        if (pos_ != text_.size()) fail("trailing characters");
        return out;
    }

private:
    [[noreturn]] void fail(const char* why) const {
        throw std::runtime_error(std::string("invalid JSON: ") + why);
    }
    void skip() { while (pos_ < text_.size() && std::string_view(" \t\r\n").find(text_[pos_]) != std::string_view::npos) ++pos_; }
    char peek() { skip(); if (pos_ >= text_.size()) fail("unexpected end"); return text_[pos_]; }
    void expect(char c) { if (peek() != c) fail("unexpected token"); ++pos_; }
    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }
    Json value(int depth = 0) {
        if (depth > 32) fail("nesting too deep");
        switch (peek()) {
            case '{': return object(depth);
            case '[': return array(depth);
            case '"': return Json{str()};
            case 't': if (literal("true")) return Json{true}; fail("bad literal");
            case 'f': if (literal("false")) return Json{false}; fail("bad literal");
            case 'n': if (literal("null")) return Json{nullptr}; fail("bad literal");
            default: return number();
        }
    }
    Json object(int depth) {
        Object out;
        expect('{');
        if (peek() == '}') { ++pos_; return Json{std::move(out)}; }
        for (;;) {
            std::string key = str();
            expect(':');
            if (!out.emplace(std::move(key), value(depth + 1)).second) fail("duplicate key");
            if (peek() == ',') { ++pos_; continue; }
            expect('}');
            return Json{std::move(out)};
        }
    }
    Json array(int depth) {
        Array out;
        expect('[');
        if (peek() == ']') { ++pos_; return Json{std::move(out)}; }
        for (;;) {
            out.push_back(value(depth + 1));
            if (peek() == ',') { ++pos_; continue; }
            expect(']');
            return Json{std::move(out)};
        }
    }
    std::string str() {
        expect('"');
        std::string out;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
            if (c != '\\') { out.push_back(c); continue; }
            if (pos_ >= text_.size()) break;
            const char e = text_[pos_++];
            switch (e) {
                case '"': case '\\': case '/': out.push_back(e); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) fail("short unicode escape");
                    unsigned code = 0;
                    const auto parsed = std::from_chars(text_.data() + pos_, text_.data() + pos_ + 4, code, 16);
                    if (parsed.ptr != text_.data() + pos_ + 4) fail("bad unicode escape");
                    pos_ += 4;
                    if (code >= 0xD800 && code <= 0xDFFF) fail("surrogate escapes unsupported");
                    if (code < 0x80) out.push_back(static_cast<char>(code));
                    else if (code < 0x800) { out.push_back(static_cast<char>(0xC0 | code >> 6)); out.push_back(static_cast<char>(0x80 | (code & 0x3F))); }
                    else { out.push_back(static_cast<char>(0xE0 | code >> 12)); out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (code & 0x3F))); }
                    break;
                }
                default: fail("bad escape");
            }
        }
        fail("unterminated string");
    }
    Json number() {
        const std::size_t start = pos_;
        bool floating = false;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        while (pos_ < text_.size() && std::string_view("0123456789.eE+-").find(text_[pos_]) != std::string_view::npos) {
            floating |= std::string_view(".eE").find(text_[pos_]) != std::string_view::npos;
            ++pos_;
        }
        const char* begin = text_.data() + start;
        const char* end = text_.data() + pos_;
        if (begin == end) fail("expected value");
        if (!floating) {
            int64_t integer = 0;
            const auto parsed = std::from_chars(begin, end, integer);
            if (parsed.ec == std::errc{} && parsed.ptr == end) return Json{integer};
        }
        double real = 0;
        const auto parsed = std::from_chars(begin, end, real);
        if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(real)) fail("bad number");
        return Json{real};
    }

    std::string_view text_;
    std::size_t pos_{0};
};

struct Options {
    std::string export_dir;
    std::string output{"-"};
    std::string key_header{"job"};
    PerKeyPredictorConfig predictor;
    std::string policy_version{"per-key-replay-v2"};
};

[[noreturn]] void usage(const char* program, const std::string& error = {}) {
    if (!error.empty()) std::cerr << "error: " << error << "\n";
    std::cerr << "usage: " << program << " --export DIR [--output FILE] [--key-header NAME | --key-header '']\n"
        "  [--levels N] [--default-priority N] [--summary median|p75|mean] [--min-samples N]\n"
        "  [--spread-quantile X] [--max-spread X] [--max-censored X] [--decay X] [--global-decay X]\n"
        "  [--time-half-life-ms N] [--stale-after-ms N] [--idle-eviction-ms N] [--cold-grace-ms N]\n"
        "  [--max-keys N] [--shards N] [--refresh-every N] [--global-min-samples N] [--hysteresis X]\n"
        "  [--min-ms X] [--max-ms X] [--bins N]\n"
        "Reads DIR/events.jsonl; an empty --key-header uses one constant key. Time-based decay, staleness and\n"
        "eviction run on the export's benchmark_time_ns, not on wall-clock replay time.\n";
    std::exit(error.empty() ? 0 : 2);
}

double to_double(const std::string& s, const char* name) {
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (s.empty() || *end != '\0' || !std::isfinite(v)) throw std::invalid_argument(std::string("invalid ") + name);
    return v;
}

uint64_t to_count(const std::string& s, const char* name) {
    uint64_t v = 0;
    const auto parsed = std::from_chars(s.data(), s.data() + s.size(), v);
    if (s.empty() || parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size())
        throw std::invalid_argument(std::string("invalid ") + name);
    return v;
}

Options parse_options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--help" || flag == "-h") usage(argv[0]);
        if (i + 1 >= argc) usage(argv[0], "missing value for " + flag);
        const std::string value = argv[++i];
        auto& p = o.predictor;
        const auto ms = [&](const char* name) { return std::chrono::milliseconds(to_count(value, name)); };
        if (flag == "--export") o.export_dir = value;
        else if (flag == "--output") o.output = value;
        else if (flag == "--key-header") o.key_header = value;
        else if (flag == "--policy-version") o.policy_version = value;
        else if (flag == "--levels") p.num_levels = static_cast<uint8_t>(to_count(value, "levels"));
        else if (flag == "--default-priority") p.default_priority = static_cast<uint8_t>(to_count(value, "default-priority"));
        else if (flag == "--summary") {
            if (value == "median") p.summary = DurationSummary::Median;
            else if (value == "p75") p.summary = DurationSummary::P75;
            else if (value == "mean") p.summary = DurationSummary::Mean;
            else usage(argv[0], "summary must be median, p75 or mean");
        }
        else if (flag == "--min-samples") p.min_samples = static_cast<uint32_t>(to_count(value, "min-samples"));
        else if (flag == "--spread-quantile") p.spread_quantile = to_double(value, "spread-quantile");
        else if (flag == "--max-spread") p.max_spread_ratio = to_double(value, "max-spread");
        else if (flag == "--max-censored") p.max_censored_fraction = to_double(value, "max-censored");
        else if (flag == "--decay") p.decay = to_double(value, "decay");
        else if (flag == "--global-decay") p.global_decay = to_double(value, "global-decay");
        else if (flag == "--time-half-life-ms") p.time_half_life = ms("time-half-life-ms");
        else if (flag == "--stale-after-ms") p.stale_after = ms("stale-after-ms");
        else if (flag == "--idle-eviction-ms") p.idle_eviction = ms("idle-eviction-ms");
        else if (flag == "--cold-grace-ms") p.cold_eviction_grace = ms("cold-grace-ms");
        else if (flag == "--max-keys") p.max_keys = to_count(value, "max-keys");
        else if (flag == "--shards") p.shards = to_count(value, "shards");
        else if (flag == "--refresh-every") p.boundary_refresh_every = static_cast<uint32_t>(to_count(value, "refresh-every"));
        else if (flag == "--global-min-samples") p.global_min_samples = static_cast<uint32_t>(to_count(value, "global-min-samples"));
        else if (flag == "--hysteresis") p.hysteresis = to_double(value, "hysteresis");
        else if (flag == "--min-ms") p.min_ms = to_double(value, "min-ms");
        else if (flag == "--max-ms") p.max_ms = to_double(value, "max-ms");
        else if (flag == "--bins") p.histogram_bins = to_count(value, "bins");
        else usage(argv[0], "unknown option " + flag);
    }
    if (o.export_dir.empty()) usage(argv[0], "--export is required");
    return o;
}

/// Only the fields replay needs, so memory grows with events, not with their JSON trees.
struct Event {
    std::string run, instance, message, split, id, key, trigger, label_status;
    int64_t time{0};
    bool ingress{false};
    bool delivered_attempt{false};
    std::optional<double> duration_ms;
};

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(c)); }
        else if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
        else out.push_back(static_cast<char>(c));
    }
    return out + "\"";
}

std::string text(const Json& row, const char* field) {
    const Json* v = row.get(field);
    if (!v || !v->string()) throw std::runtime_error(std::string("event missing string field ") + field);
    return *v->string();
}

std::string optional_text(const Json& row, const char* field) {
    const Json* v = row.get(field);
    return v && v->string() ? *v->string() : std::string{};
}

std::string number_text(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

std::string boundaries_text(const std::vector<double>& boundaries) {
    std::string out = "[";
    for (std::size_t i = 0; i < boundaries.size(); ++i) out += (i ? "," : "") + number_text(boundaries[i]);
    return out + "]";
}

} // namespace

int main(int argc, char** argv) try {
    Options options = parse_options(argc, argv);
    // Time-based decay, staleness and eviction follow the export's chronology, never the replay's wall clock.
    std::chrono::steady_clock::time_point replay_now{};
    options.predictor.clock = [&replay_now] { return replay_now; };
    (void)PerKeyPredictor{options.predictor};  // reject an invalid configuration before reading input

    std::ifstream input(options.export_dir + "/events.jsonl");
    if (!input) throw std::runtime_error("cannot open " + options.export_dir + "/events.jsonl");
    std::vector<Event> events;
    std::unordered_map<std::string, std::size_t> seen;  // (run, event_id) -> hash of its serialized row
    std::string line;
    uint64_t duplicates = 0;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const Json row = Parser(line).parse();
        const std::string type = text(row, "event_type");
        if (type != "ingress" && type != "outcome") throw std::runtime_error("unknown event_type " + type);
        Event e;
        e.run = text(row, "run");
        e.id = text(row, "event_id");
        // Same rule as the Python dataset: an identical repeat is ignored, a differing one invalidates the export.
        const auto fingerprint = std::hash<std::string>{}(line);
        const auto [position, fresh] = seen.emplace(e.run + '\n' + e.id, fingerprint);
        if (!fresh) {
            if (position->second != fingerprint) throw std::runtime_error("conflicting duplicate event " + e.id);
            ++duplicates;
            continue;
        }
        e.ingress = type == "ingress";
        const Json* time = row.get("benchmark_time_ns");
        if (!time || !time->integer() || *time->integer() < 0) throw std::runtime_error("event missing benchmark_time_ns");
        e.time = *time->integer();
        e.instance = text(row, "broker_instance_id");
        e.message = text(row, "message_id");
        e.split = text(row, "split");
        e.trigger = optional_text(row, "trigger");
        e.label_status = optional_text(row, "label_status");
        if (const Json* attempt = row.get("attempt_id")) e.delivered_attempt = !std::holds_alternative<std::nullptr_t>(attempt->value);
        if (const Json* duration = row.get("processing_time_ms")) e.duration_ms = duration->number();
        e.key = "default";
        if (!options.key_header.empty()) {
            e.key.clear();
            const Json* routing = row.get("routing");
            const Json* features = routing ? routing->get("features") : nullptr;
            const Json* headers = features ? features->get("headers") : nullptr;
            const Json* header = headers ? headers->get(options.key_header) : nullptr;
            if (header && header->string()) e.key = *header->string();
        }
        events.push_back(std::move(e));
    }
    seen.clear();
    // Runs have independent clock origins, so each is replayed alone with a fresh predictor. Within a run the
    // order matches the Python dataset: time, ingress before equal-time feedback, then event id.
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        if (const int by_run = a.run.compare(b.run); by_run != 0) return by_run < 0;
        if (a.time != b.time) return a.time < b.time;
        if (a.ingress != b.ingress) return a.ingress;
        if (a.id.size() != b.id.size()) return a.id.size() < b.id.size();
        return a.id < b.id;
    });

    std::ofstream file;
    std::ostream* out = &std::cout;
    if (options.output != "-") {
        file.open(options.output, std::ios::trunc);
        if (!file) throw std::runtime_error("cannot open output " + options.output);
        out = &file;
    }

    std::unique_ptr<PerKeyPredictor> predictor;
    std::string current_run;
    uint64_t ingress = 0, learned = 0, censored = 0;
    const auto finish_run = [&] {
        if (!predictor) return;
        const auto snapshot = predictor->snapshot();
        const auto stats = predictor->stats();
        *out << "{\"kind\":\"summary\",\"run\":" << quote(current_run) << ",\"model_version\":"
             << quote(predictor->model_version()) << ",\"boundaries_ms\":"
             << boundaries_text(snapshot ? snapshot->boundaries_ms : std::vector<double>{})
             << ",\"ingress\":" << ingress << ",\"learned\":" << learned << ",\"censored\":" << censored
             << ",\"duplicate_events\":" << duplicates << ",\"keys\":" << stats.keys
             << ",\"evictions\":" << stats.evictions << ",\"overflow_observations\":"
             << stats.overflow_observations << ",\"saturated\":" << stats.saturated
             << ",\"rejected\":" << stats.rejected << ",\"snapshots\":" << stats.snapshots << "}\n";
    };
    for (const Event& event : events) {
        if (!predictor || event.run != current_run) {
            finish_run();
            current_run = event.run;
            predictor = std::make_unique<PerKeyPredictor>(options.predictor);
            ingress = learned = censored = 0;
        }
        replay_now = std::chrono::steady_clock::time_point{} + std::chrono::nanoseconds(event.time);
        if (event.ingress) {
            ++ingress;
            const auto snapshot = predictor->snapshot();  // single-threaded replay: same snapshot predict will use
            const DurationPrediction p = predictor->predict(event.key);
            *out << "{\"kind\":\"prediction\",\"run\":" << quote(current_run)
                 << ",\"broker_instance_id\":" << quote(event.instance)
                 << ",\"message_id\":" << quote(event.message)
                 << ",\"split\":" << quote(event.split) << ",\"arrival_ns\":" << event.time
                 << ",\"model_version\":" << quote(predictor->model_version())
                 << ",\"policy_version\":" << quote(options.policy_version) << ",\"duration_ms\":"
                 << (p.estimate_ms ? number_text(*p.estimate_ms) : "null") << ",\"bucket\":" << int(p.bucket)
                 << ",\"fallback\":"
                 << (p.status == PredictionStatus::Predicted ? "null" : quote(std::string(to_string(p.status))))
                 << ",\"updates\":" << p.key_observations << ",\"backoff\":false,\"boundaries_ms\":"
                 << boundaries_text(snapshot ? snapshot->boundaries_ms : std::vector<double>{}) << "}\n";
            continue;
        }
        if (event.split != "train" && event.split != "evaluation") continue;
        if (event.label_status == "eligible" && event.duration_ms) {
            if (predictor->observe(event.key, *event.duration_ms)) ++learned;
        } else if (event.trigger == "lease_expiry" && event.delivered_attempt) {
            // A delivered attempt overran its lease: no duration, but the key is not as fast as its successes say.
            if (predictor->observe_censored(event.key)) ++censored;
        }
    }
    finish_run();
    out->flush();
    return out->good() ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
}
