// Trace-driven multi-level queue simulator (issue #25). Replays an invocation trace through non-preemptive workers
// with the broker's tier/aging rules; the predictive arms use the broker's own PerKeyPredictor on simulation time.
// Usage: harbinger_trace_sim AZURE_TRACE.txt CONFIG.json OUT.json
#include "ml/duration_predictor.hpp"
#include "util/json.hpp"

#include <algorithm>
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

/// Non-preemptive multi-level queue with FIFO levels, strict priority and aging (levels >= 1 promote one step
/// after waiting `threshold` since their last placement, checked every `interval`, as in the broker).
template <class Route, class Learn>
Result simulate(const std::vector<Invocation>& rows, double compress, int workers, int levels,
                double threshold, double interval, Route route, Learn learn) {
    struct Waiting { std::size_t index; double placed; };
    std::vector<std::deque<Waiting>> queues(levels);
    using Completion = std::pair<double, std::size_t>;
    std::priority_queue<Completion, std::vector<Completion>, std::greater<>> running;
    Result r;
    r.latency.assign(rows.size(), 0); r.wait.assign(rows.size(), 0); r.slowdown.assign(rows.size(), 0);
    const double origin = rows.front().arrival_ms;
    auto arrival = [&](std::size_t i) { return (rows[i].arrival_ms - origin) / compress; };
    std::size_t next = 0, done = 0, queued = 0;
    int idle = workers;
    double now = 0, next_aging = interval;
    auto dispatch = [&] {
        while (idle > 0 && queued > 0) {
            for (auto& level : queues) {
                if (level.empty()) continue;
                const auto w = level.front();
                level.pop_front();
                --queued; --idle;
                r.wait[w.index] = now - arrival(w.index);
                running.push({now + rows[w.index].duration_ms, w.index});
                break;
            }
        }
    };
    while (done < rows.size()) {
        const double t_arrival = next < rows.size() ? arrival(next) : INFINITY;
        const double t_complete = running.empty() ? INFINITY : running.top().first;
        const double t_aging = queued ? next_aging : INFINITY;
        now = std::min({t_arrival, t_complete, t_aging});
        if (now == t_complete) {
            const auto [end, i] = running.top();
            running.pop();
            ++idle; ++done;
            r.latency[i] = end - arrival(i);
            r.slowdown[i] = r.latency[i] / std::max(rows[i].duration_ms, 1.0);
            learn(rows[i], now);
        } else if (now == t_arrival) {
            const int tier = route(rows[next], now);
            queues[static_cast<std::size_t>(tier)].push_back({next, now});
            ++queued; ++next;
        } else {
            for (int level = 1; level < levels; ++level) {
                auto& q = queues[static_cast<std::size_t>(level)];
                while (!q.empty() && now - q.front().placed >= threshold) {
                    auto w = q.front();
                    q.pop_front();
                    w.placed = now;
                    queues[static_cast<std::size_t>(level - 1)].push_back(w);
                }
            }
            next_aging = now + interval;
        }
        dispatch();
        if (!queued) next_aging = now + interval;
    }
    return r;
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
            const std::string arm = *arm_json.string();
            ml::PerKeyPredictorConfig pc;
            pc.num_levels = static_cast<uint8_t>(levels);
            pc.default_priority = static_cast<uint8_t>(default_tier);
            double sim_now = 0;
            pc.clock = [&sim_now] { return std::chrono::steady_clock::time_point{} + std::chrono::microseconds(static_cast<int64_t>(sim_now * 1000)); };
            ml::PerKeyPredictor predictor{pc};
            const bool predictive = arm.starts_with("predictive");
            if (arm == "predictive_warm") {
                // History sits before the evaluation start on the same compressed clock (negative simulation time).
                const double e0 = evaluation.front().arrival_ms;
                for (const auto& row : history) {
                    sim_now = (row.arrival_ms - e0) / compress;
                    predictor.observe(trace.keys[row.key], row.duration_ms);
                }
            }
            const auto route = [&](const Invocation& row, double now) -> int {
                if (arm == "fifo") return 0;
                if (arm == "oracle") return ml::tier_of(bounds, row.duration_ms);
                if (arm == "static_history") { const auto f = history_tier.find(row.key); return f == history_tier.end() ? default_tier : f->second; }
                if (arm == "static_random") { const auto f = random_tier.find(row.key); return f == random_tier.end() ? default_tier : f->second; }
                sim_now = now;
                const auto p = predictor.predict(trace.keys[row.key]);
                return p.bucket;
            };
            const auto learn = [&](const Invocation& row, double now) {
                if (!predictive) return;
                sim_now = now;
                predictor.observe(trace.keys[row.key], row.duration_ms);
            };
            const auto started = std::chrono::steady_clock::now();
            const auto r = simulate(evaluation, compress, workers, levels, threshold, interval, route, learn);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            out << ",\"" << arm << "\":{\"sim_seconds\":" << seconds << ",\"all\":" << window_metrics(r, cls, 0, evaluation.size()) << ",\"windows\":[";
            for (std::size_t w = 0; w < windows; ++w) {
                const auto begin = evaluation.size() * w / windows, end = evaluation.size() * (w + 1) / windows;
                out << (w ? "," : "") << window_metrics(r, cls, begin, end);
            }
            out << "]";
            if (predictive) {
                const auto s = predictor.stats();
                out << ",\"predictor\":{\"keys\":" << s.keys << ",\"observations\":" << s.observations << ",\"snapshots\":" << s.snapshots << "}";
            }
            out << "}";
            std::cerr << "[sim] utilization=" << utilization << " arm=" << arm << " " << seconds << "s\n";
        }
        out << "}";
    }
    out << "}}\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "[sim] " << e.what() << '\n';
    return 1;
}
