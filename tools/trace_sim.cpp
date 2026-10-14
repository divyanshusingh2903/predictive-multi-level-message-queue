// Trace-driven multi-level queue simulator (issue #25). Replays an invocation trace through non-preemptive workers
// with the broker's tier/aging rules; the predictive arms use the broker's own PerKeyPredictor on simulation time.
// Usage: harbinger_trace_sim TRACE CONFIG.json OUT.json
// Replay mode (v4-simvalidation, `"replay": true`) replays a measured run as it happened, without compressing arrivals.
// TRACE is the Azure Functions 2021 invocation file or any trace in the common harbinger-trace-v1 format (#47,
// analysis/trace_format.py); the header tells them apart.
#include "ml/duration_predictor.hpp"
#include "ml/predictive_routing.hpp"
#include "util/json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace harbinger;

namespace {

struct Invocation { double arrival_ms; uint32_t key; double duration_ms; };

struct Trace {
    std::vector<Invocation> rows;
    std::vector<std::string> keys;
    /// Per-row size hints in row order, NaN when unknown; empty when the trace has no size column.
    std::vector<double> sizes;
};

/// harbinger-trace-v1: arrival_ms,key,duration_ms[,size], sorted by arrival (ties keep file order).
Trace load_common(std::ifstream& in, bool sized) {
    Trace t;
    std::unordered_map<std::string, uint32_t> ids;
    std::string line;
    double previous = -INFINITY;
    for (std::size_t number = 2; std::getline(in, line); ++number) {
        const auto fail = [&](const char* what) {
            return std::runtime_error("line " + std::to_string(number) + ": " + what);
        };
        const auto a = line.find(','), b = line.find(',', a + 1), c = line.find(',', b + 1);
        if (b == std::string::npos || (sized ? c == std::string::npos || line.find(',', c + 1) != std::string::npos
                                             : c != std::string::npos))
            throw fail("wrong number of fields");
        const auto key = line.substr(a + 1, b - a - 1);
        if (key.empty() || key.find('"') != std::string::npos) throw fail("bad key");
        const double arrival = std::stod(line.substr(0, a));
        const double duration = std::stod(line.substr(b + 1, sized ? c - b - 1 : std::string::npos));
        if (!std::isfinite(arrival) || arrival < previous) throw fail("rows must be sorted by arrival_ms");
        if (!std::isfinite(duration) || duration < 0) throw fail("bad duration_ms");
        previous = arrival;
        if (sized) {
            const auto field = line.substr(c + 1);
            const double size = field.empty() ? NAN : std::stod(field);
            if (!field.empty() && (!std::isfinite(size) || size < 0)) throw fail("bad size");
            t.sizes.push_back(size);
        }
        const auto [it, fresh] = ids.emplace(key, static_cast<uint32_t>(t.keys.size()));
        if (fresh) t.keys.push_back(key);
        t.rows.push_back({arrival, it->second, duration});
    }
    return t;
}

Trace load(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("empty trace");
    if (line == "arrival_ms,key,duration_ms") return load_common(in, false);
    if (line == "arrival_ms,key,duration_ms,size") return load_common(in, true);
    if (line != "app,func,end_timestamp,duration") throw std::runtime_error("unexpected header");
    // Azure Functions 2021, read as for the published results: arrival reconstructed as end - duration.
    Trace t;
    std::unordered_map<std::string, uint32_t> ids;
    while (std::getline(in, line)) {
        const auto a = line.find(','), b = line.find(',', a + 1), c = line.find(',', b + 1);
        if (c == std::string::npos) throw std::runtime_error("bad row");
        const auto key = line.substr(0, a) + "/" + line.substr(a + 1, b - a - 1);
        const double end = std::stod(line.substr(b + 1, c - b - 1)), duration = std::stod(line.substr(c + 1));
        const auto [it, fresh] = ids.emplace(key, static_cast<uint32_t>(t.keys.size()));
        if (fresh) t.keys.push_back(key);
        t.rows.push_back({(end - duration) * 1000.0, it->second, duration * 1000.0});
    }
    std::sort(t.rows.begin(), t.rows.end(), [](const Invocation& x, const Invocation& y) { return x.arrival_ms < y.arrival_ms; });
    return t;
}

double quantile(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<std::size_t>(std::max(0.0, std::ceil(v.size() * q) - 1)))];
}

struct Result { std::vector<double> latency, wait, slowdown; std::vector<int> cls; };

/// Scheduling policy of one arm, as in the broker (#40).
struct Policy {
    /// Empty: strict priority. Otherwise worker-time share: each level has a virtual clock, a dispatch serves the
    /// non-empty level with the smallest clock and charges it expected cost / weight; a level that becomes non-empty
    /// starts no earlier than the floor of the active clocks.
    std::vector<double> weights;
    bool aging{true};
    /// Pausing aging: promote from L to L-1 unless L-1 is behind, i.e. still holds promoted messages and its head has
    /// been in the system (since arrival) for at least the threshold; at most one promotion per level per pass.
    bool pausing{false};
};

/// Non-preemptive multi-level queue with FIFO levels, strict priority or worker-time share, and aging (levels >= 1
/// promote one step after waiting `threshold` since their last placement, checked every `interval`, as in the broker).
/// `route` returns the tier and the predicted duration (0 = none); unpredicted work is charged its level's running
/// average of completed durations (EWMA 0.05), or 1 ms before any completion, as in the broker.
template <class Route, class Learn>
Result simulate(const std::vector<Invocation>& rows, double compress, int workers, int levels,
                double threshold, double interval, const Policy& policy, Route route, Learn learn) {
    struct Waiting { std::size_t index; double placed; bool promoted{false}; };
    std::vector<std::deque<Waiting>> queues(levels);
    std::vector<std::size_t> promoted_in(levels, 0);
    std::vector<double> predicted(rows.size(), 0.0), vclock(levels, 0.0), level_cost(levels, 0.0);
    std::vector<int> pulled_from(rows.size(), 0);
    double vfloor = 0.0;
    const bool weighted = !policy.weights.empty();
    auto place = [&](int level, Waiting w) {
        if (weighted && queues[level].empty()) vclock[level] = std::max(vclock[level], vfloor);
        promoted_in[level] += w.promoted;
        queues[level].push_back(w);
    };
    using Completion = std::pair<double, std::size_t>;
    std::priority_queue<Completion, std::vector<Completion>, std::greater<>> running;
    Result r;
    r.latency.assign(rows.size(), 0); r.wait.assign(rows.size(), 0); r.slowdown.assign(rows.size(), 0);
    const double origin = rows.front().arrival_ms;
    auto arrival = [&](std::size_t i) { return (rows[i].arrival_ms - origin) / compress; };
    std::size_t next = 0, done = 0, queued = 0;
    int idle = workers;
    double now = 0, next_aging = interval;
    auto start = [&](int level) {
        const auto w = queues[level].front();
        queues[level].pop_front();
        promoted_in[level] -= w.promoted;
        --queued; --idle;
        pulled_from[w.index] = level;
        r.wait[w.index] = now - arrival(w.index);
        running.push({now + rows[w.index].duration_ms, w.index});
        return w;
    };
    auto dispatch = [&] {
        while (idle > 0 && queued > 0) {
            if (weighted) {
                int best = -1;
                for (int level = 0; level < levels; ++level)
                    if (!queues[level].empty() && (best < 0 || vclock[level] < vclock[best])) best = level;
                vfloor = std::max(vfloor, vclock[best]);
                const auto w = start(best);
                const double cost = predicted[w.index] > 0 ? predicted[w.index]
                    : level_cost[best] > 0 ? level_cost[best] : 1.0;
                vclock[best] += std::max(cost, 1.0) / policy.weights[best];
                continue;
            }
            for (int level = 0; level < levels; ++level)
                if (!queues[level].empty()) { start(level); break; }
        }
    };
    while (done < rows.size()) {
        const double t_arrival = next < rows.size() ? arrival(next) : INFINITY;
        const double t_complete = running.empty() ? INFINITY : running.top().first;
        const double t_aging = queued && policy.aging ? next_aging : INFINITY;
        now = std::min({t_arrival, t_complete, t_aging});
        if (now == t_complete) {
            const auto [end, i] = running.top();
            running.pop();
            ++idle; ++done;
            r.latency[i] = end - arrival(i);
            r.slowdown[i] = r.latency[i] / std::max(rows[i].duration_ms, 1.0);
            auto& average = level_cost[pulled_from[i]];
            average = average > 0 ? average + 0.05 * (rows[i].duration_ms - average) : rows[i].duration_ms;
            learn(rows[i], now);
        } else if (now == t_arrival) {
            const auto [tier, estimate] = route(rows[next], now);
            predicted[next] = estimate;
            place(tier, {next, now});
            ++queued; ++next;
        } else {
            for (int level = levels - 1; level >= 1; --level) {  // bottom first, as in the broker
                auto& q = queues[static_cast<std::size_t>(level)];
                const auto& above = queues[static_cast<std::size_t>(level - 1)];
                if (policy.pausing && promoted_in[level - 1] && !above.empty() &&
                    now - arrival(above.front().index) >= threshold)
                    continue;
                while (!q.empty() && now - q.front().placed >= threshold) {
                    auto w = q.front();
                    q.pop_front();
                    promoted_in[level] -= w.promoted;
                    w.placed = now;
                    w.promoted = true;
                    place(level - 1, w);
                    if (policy.pausing) break;
                }
            }
            next_aging = now + interval;
        }
        dispatch();
        if (!queued) next_aging = now + interval;
    }
    return r;
}

/// Prediction outcomes per evaluation row (predictive arms only): status, and whether the key had work queued or
/// running at that moment. Used to separate keys that are truly idle from keys stuck behind a backlog (#38).
struct Outcomes {
    std::vector<uint8_t> status;
    std::vector<uint8_t> outstanding;
};

std::string outcome_metrics(const Outcomes& o, const Result& r, std::size_t begin, std::size_t end) {
    std::array<uint64_t, ml::kPredictionStatusCount> counts{};
    uint64_t stale_outstanding = 0, backlog = 0, backlog_stale = 0, backlog_predicted = 0, quiet_stale = 0;
    const auto stale = static_cast<uint8_t>(ml::PredictionStatus::StaleKey);
    const auto predicted = static_cast<uint8_t>(ml::PredictionStatus::Predicted);
    for (std::size_t i = begin; i < end; ++i) {
        ++counts[o.status[i]];
        stale_outstanding += o.status[i] == stale && o.outstanding[i];
        // "Backlog" = the message itself waited over a minute (descriptive, measured after the fact).
        if (r.wait[i] > 60'000.0) {
            ++backlog;
            backlog_stale += o.status[i] == stale;
            backlog_predicted += o.status[i] == predicted;
        } else {
            quiet_stale += o.status[i] == stale;
        }
    }
    std::ostringstream out;
    out << "{";
    for (std::size_t s = 0; s < counts.size(); ++s)
        out << (s ? "," : "") << '"' << ml::to_string(static_cast<ml::PredictionStatus>(s)) << "\":" << counts[s];
    out << ",\"stale_with_work_outstanding\":" << stale_outstanding << ",\"backlog_rows\":" << backlog
        << ",\"backlog_stale\":" << backlog_stale << ",\"backlog_predicted\":" << backlog_predicted
        << ",\"quiet_rows\":" << (end - begin - backlog) << ",\"quiet_stale\":" << quiet_stale << "}";
    return out.str();
}

/// `class_counts` (v3-traces rules only) adds short/medium/long_count, so a report can tell an empty class from a
/// class that never waited.
std::string window_metrics(const Result& r, const std::vector<int>& cls, std::size_t begin, std::size_t end,
                           bool class_counts = false) {
    std::vector<double> latency(r.latency.begin() + static_cast<long>(begin), r.latency.begin() + static_cast<long>(end));
    std::vector<double> slowdown(r.slowdown.begin() + static_cast<long>(begin), r.slowdown.begin() + static_cast<long>(end));
    double sum = 0, long_wait = 0;
    std::array<std::vector<double>, 3> by_class;
    for (std::size_t i = begin; i < end; ++i) {
        sum += r.latency[i];
        by_class[static_cast<std::size_t>(cls[i])].push_back(r.latency[i]);
        if (cls[i] == 2) long_wait = std::max(long_wait, r.wait[i]);
    }
    std::ostringstream o;
    o << "{\"count\":" << latency.size() << ",\"mean_latency_ms\":" << sum / static_cast<double>(latency.size())
      << ",\"p50_latency_ms\":" << quantile(latency, 0.5) << ",\"p95_latency_ms\":" << quantile(latency, 0.95)
      << ",\"p99_latency_ms\":" << quantile(latency, 0.99) << ",\"slowdown_p50\":" << quantile(slowdown, 0.5)
      << ",\"slowdown_p99\":" << quantile(slowdown, 0.99) << ",\"long_max_wait_ms\":" << long_wait;
    const char* names[] = {"short", "medium", "long"};
    for (std::size_t c = 0; c < 3; ++c) {
        double mean = 0;
        for (double v : by_class[c]) mean += v;
        o << ",\"" << names[c] << "_mean_ms\":" << (by_class[c].empty() ? 0.0 : mean / static_cast<double>(by_class[c].size()))
          << ",\"" << names[c] << "_p50_ms\":" << (by_class[c].empty() ? 0.0 : quantile(by_class[c], 0.5));
        if (class_counts) o << ",\"" << names[c] << "_count\":" << by_class[c].size();
    }
    o << "}";
    return o.str();
}

} // namespace

int main(int argc, char** argv) try {
    if (argc != 4) throw std::invalid_argument("usage: harbinger_trace_sim TRACE CONFIG.json OUT.json");
    std::ifstream config_file(argv[2]);
    const std::string config_text{std::istreambuf_iterator<char>(config_file), {}};
    const auto config = json::Parser(config_text).parse();
    const auto num = [&](const json::Json& j, const char* k) { return *j.get(k)->number(); };
    const int workers = static_cast<int>(num(config, "workers"));
    const int levels = static_cast<int>(num(config, "levels"));
    const auto default_tier = static_cast<int>(num(config, "default_priority"));
    const double threshold = num(*config.get("aging"), "threshold_ms"), interval = num(*config.get("aging"), "interval_ms");
    const double history_fraction = num(config, "history_fraction");
    const auto windows = static_cast<std::size_t>(num(config, "evaluation_days"));
    // Optional v3-traces rules (#46); configs without them keep the published behaviour byte for byte.
    //   history_split "time": history is the first history_fraction of the time span, not of the rows.
    //   window_split "time": evaluation windows of equal duration, not of equal row count.
    //   class_bounds_ms [a, b]: metric classes short < a <= medium < b <= long, instead of the history terciles.
    const auto option = [&](const char* key) {
        const auto* value = config.get(key);
        const std::string text = value ? *value->string() : "rows";
        if (text != "rows" && text != "time") throw std::invalid_argument(std::string(key) + " must be rows or time");
        return text;
    };
    const std::string history_split = option("history_split"), window_split = option("window_split");
    std::vector<double> class_bounds;
    if (const auto* list = config.get("class_bounds_ms"))
        for (const auto& b : std::get<json::Array>(list->value)) class_bounds.push_back(*b.number());
    if (!class_bounds.empty() && (class_bounds.size() != 2 || !(0 < class_bounds[0] && class_bounds[0] < class_bounds[1])))
        throw std::invalid_argument("class_bounds_ms needs two increasing positive values");
    const bool v3_rules = config.get("history_split") || config.get("window_split") || !class_bounds.empty();
    // Optional v4 rules (v4-simvalidation); configs without them reproduce the published results byte for byte.
    //   replay: arrivals keep their real spacing (no compression to a target load) and `utilizations` is not read.
    //   oracle_bounds_ms [a, b]: arm "oracle_bounds" ranks by the trace's size column against these fixed bounds.
    //   static_maps {arm: {key: tier}}: an arm named in it routes by that map, unknown keys to the default tier.
    //   per_message_dir: each arm also writes its per-message latency and wait there, one TSV per arm.
    const auto* replay_flag = config.get("replay");
    const bool replay = replay_flag && std::get_if<bool>(&replay_flag->value) && std::get<bool>(replay_flag->value);
    std::vector<double> oracle_bounds;
    if (const auto* list = config.get("oracle_bounds_ms"))
        for (const auto& b : std::get<json::Array>(list->value)) oracle_bounds.push_back(*b.number());
    if (!oracle_bounds.empty() && (oracle_bounds.size() != 2 || !(0 < oracle_bounds[0] && oracle_bounds[0] < oracle_bounds[1])))
        throw std::invalid_argument("oracle_bounds_ms needs two increasing positive values");
    const auto* static_maps = config.get("static_maps");
    std::filesystem::path per_message_dir;
    if (const auto* dir = config.get("per_message_dir")) {
        per_message_dir = *dir->string();
        std::filesystem::create_directories(per_message_dir);
    }
    std::vector<double> utilizations;
    if (replay) utilizations.push_back(1.0);  // one pass, labelled "replay" in the output
    else for (const auto& u : std::get<json::Array>(config.get("utilizations")->value)) utilizations.push_back(*u.number());

    const auto trace = load(argv[1]);
    const auto arrival_index = [](const std::vector<Invocation>& rows, double at) {
        return static_cast<std::size_t>(std::lower_bound(rows.begin(), rows.end(), at,
            [](const Invocation& row, double t) { return row.arrival_ms < t; }) - rows.begin());
    };
    const auto cut = history_split == "time"
        ? arrival_index(trace.rows, trace.rows.front().arrival_ms +
                        history_fraction * (trace.rows.back().arrival_ms - trace.rows.front().arrival_ms))
        : static_cast<std::size_t>(static_cast<double>(trace.rows.size()) * history_fraction);
    const std::vector<Invocation> history(trace.rows.begin(), trace.rows.begin() + static_cast<long>(cut));
    const std::vector<Invocation> evaluation(trace.rows.begin() + static_cast<long>(cut), trace.rows.end());

    // History statistics: global tercile boundaries and per-key medians (no look-ahead into the evaluation half).
    std::vector<double> history_durations;
    std::unordered_map<uint32_t, std::vector<double>> per_key;
    for (const auto& row : history) { history_durations.push_back(row.duration_ms); per_key[row.key].push_back(row.duration_ms); }
    std::vector<double> bounds;
    if (history_durations.empty()) {
        // Replay without a history half: there are no terciles, so the fixed class bounds stand in for them.
        if (class_bounds.empty()) throw std::invalid_argument("an empty history needs class_bounds_ms");
        bounds = class_bounds;
    } else {
        for (int i = 1; i < levels; ++i) bounds.push_back(quantile(history_durations, static_cast<double>(i) / levels));
    }
    std::unordered_map<uint32_t, int> history_tier, random_tier;
    std::mt19937 rng(static_cast<uint32_t>(num(config, "misconfigured_seed")));
    for (auto& [key, values] : per_key) {
        history_tier[key] = ml::tier_of(bounds, quantile(values, 0.5));
        random_tier[key] = static_cast<int>(rng() % static_cast<uint32_t>(levels));
    }
    std::vector<int> cls(evaluation.size());
    double work = 0;
    for (std::size_t i = 0; i < evaluation.size(); ++i) {
        cls[i] = std::min(2, static_cast<int>(ml::tier_of(class_bounds.empty() ? bounds : class_bounds, evaluation[i].duration_ms)));
        work += evaluation[i].duration_ms;
    }
    const double span = evaluation.back().arrival_ms - evaluation.front().arrival_ms;
    // Evaluation window w is rows [edges[w], edges[w + 1]).
    std::vector<std::size_t> edges{0};
    for (std::size_t w = 1; w < windows; ++w)
        edges.push_back(window_split == "time"
            ? arrival_index(evaluation, evaluation.front().arrival_ms + span * static_cast<double>(w) / static_cast<double>(windows))
            : evaluation.size() * w / windows);
    edges.push_back(evaluation.size());
    for (std::size_t w = 0; w < windows; ++w)
        if (edges[w] == edges[w + 1]) throw std::runtime_error("evaluation window " + std::to_string(w) + " is empty");

    std::ofstream out(argv[3]);
    out << "{\"version\":\"" << *config.get("version")->string() << "\",\"evaluation_rows\":" << evaluation.size()
        << ",\"history_rows\":" << history.size() << ",\"boundaries_ms\":[" << bounds[0] << "," << bounds[1] << "]";
    if (v3_rules) {
        out << ",\"history_split\":\"" << history_split << "\",\"window_split\":\"" << window_split << "\",\"class_bounds_ms\":[";
        const auto& used = class_bounds.empty() ? bounds : class_bounds;
        out << used[0] << "," << used[1] << "],\"window_rows\":[";
        for (std::size_t w = 0; w < windows; ++w) out << (w ? "," : "") << edges[w + 1] - edges[w];
        out << "]";
    }
    out << ",\"results\":{";
    bool first_u = true;
    for (const double utilization : utilizations) {
        // Compress arrivals so offered load = utilization x workers.
        const double compress = replay ? 1.0 : utilization * workers * span / work;
        if (replay) out << (first_u ? "" : ",") << "\"replay\":{\"compress\":" << compress;
        else out << (first_u ? "" : ",") << "\"" << utilization << "\":{\"compress\":" << compress;
        first_u = false;
        for (const auto& arm_json : std::get<json::Array>(config.get("arms")->value)) {
            const std::string arm_name = *arm_json.string();
            // "routing+flag+flag": flags weights | weights_alt (level_weights | sensitivity_weights), pausing, no_aging.
            const std::string arm = arm_name.substr(0, arm_name.find('+'));
            Policy policy;
            for (std::size_t at = arm_name.find('+'); at != std::string::npos;) {
                const auto next_plus = arm_name.find('+', at + 1);
                const auto flag = arm_name.substr(at + 1, next_plus == std::string::npos ? std::string::npos : next_plus - at - 1);
                const char* source = flag == "weights" ? "level_weights" : flag == "weights_alt" ? "sensitivity_weights" : nullptr;
                if (source) {
                    const auto* list = config.get(source);
                    if (!list) throw std::invalid_argument(std::string("arm needs ") + source);
                    for (const auto& w : std::get<json::Array>(list->value)) policy.weights.push_back(*w.number());
                    if (static_cast<int>(policy.weights.size()) != levels) throw std::invalid_argument("one weight per level");
                } else if (flag == "pausing") policy.pausing = true;
                else if (flag == "no_aging") policy.aging = false;
                else throw std::invalid_argument("unknown arm flag " + flag);
                at = next_plus;
            }
            ml::PerKeyPredictorConfig pc;
            pc.num_levels = static_cast<uint8_t>(levels);
            pc.default_priority = static_cast<uint8_t>(default_tier);
            // Optional, for exploratory runs only; frozen configs omit it and keep the predictor default.
            if (const auto* stale = config.get("stale_after_ms")) pc.stale_after = std::chrono::milliseconds(static_cast<int64_t>(*stale->number()));
            double sim_now = 0;
            pc.clock = [&sim_now] { return std::chrono::steady_clock::time_point{} + std::chrono::microseconds(static_cast<int64_t>(sim_now * 1000)); };
            ml::PerKeyPredictor predictor{pc};
            const bool predictive = arm.starts_with("predictive");
            Outcomes outcomes;
            std::vector<uint32_t> in_system(trace.keys.size(), 0);  // per key: queued + running
            if (predictive) {
                outcomes.status.assign(evaluation.size(), 0);
                outcomes.outstanding.assign(evaluation.size(), 0);
            }
            if (arm == "predictive_warm") {
                // History sits before the evaluation start on the same compressed clock (negative simulation time).
                const double e0 = evaluation.front().arrival_ms;
                for (const auto& row : history) {
                    sim_now = (row.arrival_ms - e0) / compress;
                    predictor.observe(trace.keys[row.key], row.duration_ms);
                }
            }
            if (arm == "oracle_bounds" && (oracle_bounds.empty() || trace.sizes.size() != trace.rows.size()))
                throw std::invalid_argument("oracle_bounds needs oracle_bounds_ms and a trace with a size column");
            std::unordered_map<uint32_t, int> static_tier;
            if (static_maps && static_maps->get(arm)) {
                std::unordered_map<std::string, uint32_t> key_id;
                for (uint32_t k = 0; k < trace.keys.size(); ++k) key_id.emplace(trace.keys[k], k);
                for (const auto& [name, tier] : std::get<json::Object>(static_maps->get(arm)->value)) {
                    const auto found = key_id.find(name);
                    if (found != key_id.end()) static_tier[found->second] = static_cast<int>(*tier.number());
                }
                if (static_tier.empty()) throw std::invalid_argument("static map for " + arm + " names no key of the trace");
            }
            const auto route = [&](const Invocation& row, double now) -> std::pair<int, double> {
                if (arm == "fifo") return {0, 0.0};
                if (arm == "oracle") return {ml::tier_of(bounds, row.duration_ms), 0.0};
                if (arm == "oracle_bounds") {
                    const double planned = trace.sizes[cut + static_cast<std::size_t>(&row - evaluation.data())];
                    return {ml::tier_of(oracle_bounds, planned), 0.0};
                }
                if (!static_tier.empty()) { const auto f = static_tier.find(row.key); return {f == static_tier.end() ? default_tier : f->second, 0.0}; }
                if (arm == "static_history") { const auto f = history_tier.find(row.key); return {f == history_tier.end() ? default_tier : f->second, 0.0}; }
                if (arm == "static_random") { const auto f = random_tier.find(row.key); return {f == random_tier.end() ? default_tier : f->second, 0.0}; }
                sim_now = now;
                const auto p = predictor.predict(trace.keys[row.key]);
                const auto index = static_cast<std::size_t>(&row - evaluation.data());
                outcomes.status[index] = static_cast<uint8_t>(p.status);
                outcomes.outstanding[index] = in_system[row.key] > 0;
                ++in_system[row.key];
                return {p.bucket, p.status == ml::PredictionStatus::Predicted && p.estimate_ms ? *p.estimate_ms : 0.0};
            };
            const auto learn = [&](const Invocation& row, double now) {
                if (!predictive) return;
                --in_system[row.key];
                sim_now = now;
                predictor.observe(trace.keys[row.key], row.duration_ms);
            };
            const auto started = std::chrono::steady_clock::now();
            const auto r = simulate(evaluation, compress, workers, levels, threshold, interval, policy, route, learn);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (!per_message_dir.empty()) {
                std::ofstream dump(per_message_dir / (arm_name + ".tsv"));
                dump << "index\tlatency_ms\twait_ms\n" << std::setprecision(12);
                for (std::size_t i = 0; i < evaluation.size(); ++i) dump << i << '\t' << r.latency[i] << '\t' << r.wait[i] << '\n';
            }
            out << ",\"" << arm_name << "\":{\"sim_seconds\":" << seconds << ",\"all\":" << window_metrics(r, cls, 0, evaluation.size(), v3_rules) << ",\"windows\":[";
            for (std::size_t w = 0; w < windows; ++w) {
                const auto begin = edges[w], end = edges[w + 1];
                out << (w ? "," : "") << window_metrics(r, cls, begin, end, v3_rules);
            }
            out << "]";
            if (predictive) {
                const auto s = predictor.stats();
                out << ",\"predictor\":{\"keys\":" << s.keys << ",\"observations\":" << s.observations << ",\"snapshots\":" << s.snapshots << "}";
                out << ",\"outcomes\":" << outcome_metrics(outcomes, r, 0, evaluation.size()) << ",\"outcome_windows\":[";
                for (std::size_t w = 0; w < windows; ++w) {
                    const auto begin = edges[w], end = edges[w + 1];
                    out << (w ? "," : "") << outcome_metrics(outcomes, r, begin, end);
                }
                out << "]";
            }
            out << "}";
            std::cerr << "[sim] utilization=" << utilization << " arm=" << arm_name << " " << seconds << "s\n";
        }
        out << "}";
    }
    out << "}}\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "[sim] " << e.what() << '\n';
    return 1;
}
