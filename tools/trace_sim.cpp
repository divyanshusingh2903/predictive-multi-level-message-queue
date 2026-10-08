// Trace-driven multi-level queue simulator (issue #25). Replays an invocation trace through non-preemptive workers
// with the broker's tier/aging rules; the predictive arms use the broker's own PerKeyPredictor on simulation time.
// Usage: harbinger_trace_sim AZURE_TRACE.txt CONFIG.json OUT.json
#include "ml/duration_predictor.hpp"
#include "ml/predictive_routing.hpp"
#include "util/json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
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
};

Trace load(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line) || line != "app,func,end_timestamp,duration") throw std::runtime_error("unexpected header");
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
    /// Pausing aging: promote from L to L-1 only while L-1's oldest placement has waited less than the threshold.
    bool pausing{false};
};

/// Non-preemptive multi-level queue with FIFO levels, strict priority or worker-time share, and aging (levels >= 1
/// promote one step after waiting `threshold` since their last placement, checked every `interval`, as in the broker).
/// `route` returns the tier and the predicted duration (0 = none); unpredicted work is charged its level's running
/// average of completed durations (EWMA 0.05), or 1 ms before any completion, as in the broker.
template <class Route, class Learn>
Result simulate(const std::vector<Invocation>& rows, double compress, int workers, int levels,
                double threshold, double interval, const Policy& policy, Route route, Learn learn) {
    struct Waiting { std::size_t index; double placed; };
    std::vector<std::deque<Waiting>> queues(levels);
    std::vector<double> predicted(rows.size(), 0.0), vclock(levels, 0.0), level_cost(levels, 0.0);
    std::vector<int> pulled_from(rows.size(), 0);
    double vfloor = 0.0;
    const bool weighted = !policy.weights.empty();
    auto place = [&](int level, Waiting w) {
        if (weighted && queues[level].empty()) vclock[level] = std::max(vclock[level], vfloor);
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
            for (int level = 1; level < levels; ++level) {
                auto& q = queues[static_cast<std::size_t>(level)];
                const auto& above = queues[static_cast<std::size_t>(level - 1)];
                if (policy.pausing && !above.empty() && now - above.front().placed >= threshold) continue;
                while (!q.empty() && now - q.front().placed >= threshold) {
                    auto w = q.front();
                    q.pop_front();
                    w.placed = now;
                    place(level - 1, w);
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

std::string window_metrics(const Result& r, const std::vector<int>& cls, std::size_t begin, std::size_t end) {
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
    }
    o << "}";
    return o.str();
}

} // namespace

int main(int argc, char** argv) try {
    if (argc != 4) throw std::invalid_argument("usage: harbinger_trace_sim TRACE.txt CONFIG.json OUT.json");
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
    std::vector<double> utilizations;
    for (const auto& u : std::get<json::Array>(config.get("utilizations")->value)) utilizations.push_back(*u.number());

    const auto trace = load(argv[1]);
    const auto cut = static_cast<std::size_t>(static_cast<double>(trace.rows.size()) * history_fraction);
    const std::vector<Invocation> history(trace.rows.begin(), trace.rows.begin() + static_cast<long>(cut));
    const std::vector<Invocation> evaluation(trace.rows.begin() + static_cast<long>(cut), trace.rows.end());

    // History statistics: global tercile boundaries and per-key medians (no look-ahead into the evaluation half).
    std::vector<double> history_durations;
    std::unordered_map<uint32_t, std::vector<double>> per_key;
    for (const auto& row : history) { history_durations.push_back(row.duration_ms); per_key[row.key].push_back(row.duration_ms); }
    std::vector<double> bounds;
    for (int i = 1; i < levels; ++i) bounds.push_back(quantile(history_durations, static_cast<double>(i) / levels));
    std::unordered_map<uint32_t, int> history_tier, random_tier;
    std::mt19937 rng(static_cast<uint32_t>(num(config, "misconfigured_seed")));
    for (auto& [key, values] : per_key) {
        history_tier[key] = ml::tier_of(bounds, quantile(values, 0.5));
        random_tier[key] = static_cast<int>(rng() % static_cast<uint32_t>(levels));
    }
    std::vector<int> cls(evaluation.size());
    double work = 0;
    for (std::size_t i = 0; i < evaluation.size(); ++i) {
        cls[i] = std::min(2, static_cast<int>(ml::tier_of(bounds, evaluation[i].duration_ms)));
        work += evaluation[i].duration_ms;
    }
    const double span = evaluation.back().arrival_ms - evaluation.front().arrival_ms;

    std::ofstream out(argv[3]);
    out << "{\"version\":\"" << *config.get("version")->string() << "\",\"evaluation_rows\":" << evaluation.size()
        << ",\"history_rows\":" << history.size() << ",\"boundaries_ms\":[" << bounds[0] << "," << bounds[1] << "]"
        << ",\"results\":{";
    bool first_u = true;
    for (const double utilization : utilizations) {
        // Compress arrivals so offered load = utilization x workers.
        const double compress = utilization * workers * span / work;
        out << (first_u ? "" : ",") << "\"" << utilization << "\":{\"compress\":" << compress;
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
            const auto route = [&](const Invocation& row, double now) -> std::pair<int, double> {
                if (arm == "fifo") return {0, 0.0};
                if (arm == "oracle") return {ml::tier_of(bounds, row.duration_ms), 0.0};
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
            out << ",\"" << arm_name << "\":{\"sim_seconds\":" << seconds << ",\"all\":" << window_metrics(r, cls, 0, evaluation.size()) << ",\"windows\":[";
            for (std::size_t w = 0; w < windows; ++w) {
                const auto begin = evaluation.size() * w / windows, end = evaluation.size() * (w + 1) / windows;
                out << (w ? "," : "") << window_metrics(r, cls, begin, end);
            }
            out << "]";
            if (predictive) {
                const auto s = predictor.stats();
                out << ",\"predictor\":{\"keys\":" << s.keys << ",\"observations\":" << s.observations << ",\"snapshots\":" << s.snapshots << "}";
                out << ",\"outcomes\":" << outcome_metrics(outcomes, r, 0, evaluation.size()) << ",\"outcome_windows\":[";
                for (std::size_t w = 0; w < windows; ++w) {
                    const auto begin = evaluation.size() * w / windows, end = evaluation.size() * (w + 1) / windows;
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
