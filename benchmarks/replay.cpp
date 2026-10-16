// Real traces through the real broker (docs/v4-replay-preregistration.md). The trace's arrivals, keys and durations
// are replayed against a broker over gRPC: producers submit each message at its scheduled time and 64 consumers run
// handlers that SLEEP for the trace's duration. Nothing else runs in the handlers, so this tests the broker, its
// predictor and its aging against a real arrival and duration structure, not an application.
//
// Time. Offered load is set as in the simulator (v3-traces-1): arrivals are compressed by a factor c per load so that
// the mean offered load is u x workers. Everything is then divided by the time scale f: arrival offsets by c*f and
// durations by f, and every time constant of the broker (aging, staleness, idle half-life) by f, so the system behaves
// as it would at real speed. f = 1 for Huawei. Reported times are multiplied back by f by replay_report.py.
//
// Commands:
//   harbinger_replay prepare TRACE CONFIG.json OUT_DIR          history statistics, per-load warm snapshots, plan.json
//   harbinger_replay run --arm ARM --load U --trace TRACE --config CONFIG.json --prep DIR --out NEW_DIR
//                        [--role all|broker|client] [--broker HOST:PORT] [--listen ADDR] [--rows N] [--seconds S]
//
// --rows and --seconds replay only a prefix of the evaluation half (the first N messages, or those scheduled within S
// seconds of real time); they are for smoke runs and the overhead probe, and are recorded in run.json.
// Roles are as in harbinger_prodflow (#47): `all` embeds the broker on loopback and writes run.json; `broker` serves
// until SIGINT/SIGTERM and writes broker.json; `client` runs producers and workers against --broker and writes
// client.json and messages.tsv. benchmarks/merge_split_run.py joins a split run. Every timestamp is taken on the
// client host, so the hosts' clocks never mix.
#include "harbinger_service.hpp"
#include "benchmark/hooks.hpp"
#include "consumer/consumer.hpp"
#include "producer/producer.hpp"
#include "config_file.hpp"
#include "ml/duration_predictor.hpp"
#include "ml/predictive_routing.hpp"
#include "util/json.hpp"
#include "trace_io.hpp"

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <pthread.h>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace harbinger;
using namespace harbinger::trace_io;
using Clock = std::chrono::steady_clock;

namespace {

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

std::string escape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

/// The key a message carries on the wire. Trace keys can exceed the broker's 128-byte job-key limit (Azure's are 129),
/// and an over-long key is silently unpredicted, so each key travels as "k" + its index in the trace's order of first
/// appearance. `prepare` and `run` both use it, so the warm snapshot, the static map and the live keys agree.
std::string wire_key(const Trace& trace, uint32_t key) { (void)trace; return "k" + std::to_string(key); }

// ── Configuration ───────────────────────────────────────────────────────────

struct Config {
    std::string version;
    double f{1.0};  // time scale
    int workers{64};
    uint8_t levels{3}, default_priority{1};
    double history_fraction{0.5};
    int64_t aging_threshold_ms{5000}, aging_interval_ms{500};
    int64_t stale_after_ms{1800000}, time_half_life_ms{600000};
    std::vector<uint32_t> weights;
    std::vector<double> loads;     // the registered loads, then the overhead probe's load if it has one
    int producers{8};
    double drain_s{3600};
};

Config read_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    const auto json = json::Parser(text).parse();
    const auto num = [&](const json::Json& j, const char* k) {
        const auto* v = j.get(k);
        if (!v || !v->number()) throw std::invalid_argument(std::string("config needs a number: ") + k);
        return *v->number();
    };
    Config c;
    c.version = *json.get("version")->string();
    c.f = num(json, "time_scale");
    c.workers = static_cast<int>(num(json, "workers"));
    c.levels = static_cast<uint8_t>(num(json, "levels"));
    c.default_priority = static_cast<uint8_t>(num(json, "default_priority"));
    c.history_fraction = num(json, "history_fraction");
    c.aging_threshold_ms = static_cast<int64_t>(num(*json.get("aging"), "threshold_ms"));
    c.aging_interval_ms = static_cast<int64_t>(num(*json.get("aging"), "interval_ms"));
    c.stale_after_ms = static_cast<int64_t>(num(*json.get("predictor"), "stale_after_ms"));
    c.time_half_life_ms = static_cast<int64_t>(num(*json.get("predictor"), "time_half_life_ms"));
    for (const auto& w : std::get<json::Array>(json.get("level_weights")->value)) c.weights.push_back(static_cast<uint32_t>(*w.number()));
    for (const auto& u : std::get<json::Array>(json.get("utilizations")->value)) c.loads.push_back(*u.number());
    if (const auto* probe = json.get("probe")) {
        const double load = num(*probe, "load");
        if (std::find(c.loads.begin(), c.loads.end(), load) == c.loads.end()) c.loads.push_back(load);
    }
    if (json.get("producer_threads")) c.producers = static_cast<int>(num(json, "producer_threads"));
    if (json.get("drain_s")) c.drain_s = num(json, "drain_s");
    if (!(c.f >= 1.0)) throw std::invalid_argument("time_scale must be at least 1");
    if (c.weights.size() != c.levels) throw std::invalid_argument("one level weight per level");
    return c;
}

/// A time constant divided by the time scale, in whole milliseconds as the broker requires; zero stays zero (off).
int64_t scaled_ms(int64_t ms, double f) {
    if (ms == 0) return 0;
    return std::max<int64_t>(1, std::llround(static_cast<double>(ms) / f));
}

/// The predictor's configuration with the scaled time constants; the broker and `prepare` must agree on it.
ml::PerKeyPredictorConfig predictor_config(const Config& c) {
    ml::PerKeyPredictorConfig p;
    p.num_levels = c.levels;
    p.default_priority = c.default_priority;
    p.stale_after = std::chrono::milliseconds{scaled_ms(c.stale_after_ms, c.f)};
    p.time_half_life = std::chrono::milliseconds{scaled_ms(c.time_half_life_ms, c.f)};
    return p;
}

ml::PredictorKeyPolicy key_policy() {
    ml::PredictorKeyPolicy policy;  // the trace key travels in the job_type header and is the whole key
    policy.scope_by_producer = false;
    return policy;
}

/// What a snapshot's context must equal; mirrors HarbingerService's construction of it.
std::string snapshot_context(const ml::PredictiveRoutingConfig& routing) {
    return routing.routing_policy_version + "|key=" + routing.key.job_header +
        (routing.key.scope_by_producer ? "|scoped" : "|unscoped") + ml::size_policy_label(routing.key);
}

// ── Arms ────────────────────────────────────────────────────────────────────

struct Arm {
    std::string name, route;  // route: fifo | oracle | static_history | predictive_warm | predictive_cold
    bool weights{false};
    bool no_aging{false};  // strict priority, nothing is promoted (v4-replay-aging)
};

Arm parse_arm(const std::string& name) {
    Arm arm{name, name.substr(0, name.find('+')), false};
    bool pausing = false;
    for (auto at = name.find('+'); at != std::string::npos;) {
        const auto next = name.find('+', at + 1);
        const auto flag = name.substr(at + 1, next == std::string::npos ? std::string::npos : next - at - 1);
        if (flag == "weights") arm.weights = true;
        else if (flag == "pausing") pausing = true;  // the broker turns pausing aging on with weights
        else if (flag == "no_aging") arm.no_aging = true;
        else throw std::invalid_argument("unknown arm flag " + flag);
        at = next;
    }
    if (arm.weights != pausing) throw std::invalid_argument("weights and pausing go together: " + name);
    if (arm.no_aging && arm.weights) throw std::invalid_argument("no_aging cannot be combined with weights and pausing: " + name);
    static const std::vector<std::string> routes{"fifo", "oracle", "static_history", "predictive_warm", "predictive_cold"};
    if (std::find(routes.begin(), routes.end(), arm.route) == routes.end()) throw std::invalid_argument("unknown arm " + name);
    return arm;
}

// ── The plan written by `prepare` ───────────────────────────────────────────

struct Plan {
    double f{1.0};
    std::size_t cut{0}, eval_rows{0};
    double span_ms{0}, work_ms{0}, max_duration_ms{0};
    std::vector<double> bounds;
    std::map<std::string, double> compress;       // by the load as printed
    std::map<std::string, std::string> snapshot;  // file name by the load as printed
    std::map<std::string, uint8_t> static_history;
};

std::string load_label(double u) {
    std::ostringstream o;
    o << u;
    return o.str();
}

Plan read_plan(const std::filesystem::path& dir) {
    std::ifstream in(dir / "plan.json");
    if (!in) throw std::runtime_error("no plan.json in " + dir.string() + ": run `prepare` first");
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    const auto json = json::Parser(text).parse();
    const auto num = [&](const char* k) { return *json.get(k)->number(); };
    Plan p;
    p.f = num("time_scale");
    p.cut = static_cast<std::size_t>(num("cut"));
    p.eval_rows = static_cast<std::size_t>(num("eval_rows"));
    p.span_ms = num("span_ms");
    p.work_ms = num("work_ms");
    p.max_duration_ms = num("max_duration_ms");
    for (const auto& b : std::get<json::Array>(json.get("boundaries_ms")->value)) p.bounds.push_back(*b.number());
    for (const auto& [load, entry] : std::get<json::Object>(json.get("loads")->value)) {
        p.compress[load] = *entry.get("compress")->number();
        p.snapshot[load] = *entry.get("snapshot")->string();
    }
    for (const auto& [key, tier] : std::get<json::Object>(json.get("static_history")->value))
        p.static_history[key] = static_cast<uint8_t>(*tier.number());
    return p;
}

std::size_t arrival_index(const std::vector<Invocation>& rows, double at) {
    return static_cast<std::size_t>(std::lower_bound(rows.begin(), rows.end(), at,
        [](const Invocation& row, double t) { return row.arrival_ms < t; }) - rows.begin());
}

/// The history/evaluation split of the registered rules: by time, at history_fraction of the span.
std::size_t history_cut(const Trace& trace, double fraction) {
    return arrival_index(trace.rows, trace.rows.front().arrival_ms +
                         fraction * (trace.rows.back().arrival_ms - trace.rows.front().arrival_ms));
}

// ── prepare ─────────────────────────────────────────────────────────────────

int prepare(const std::string& trace_path, const std::string& config_path, const std::filesystem::path& out) {
    const auto config = read_config(config_path);
    const auto trace = load(trace_path);
    if (trace.rows.empty()) throw std::runtime_error("empty trace");
    std::filesystem::create_directories(out);
    if (std::filesystem::exists(out / "plan.json")) throw std::invalid_argument("refusing to overwrite " + (out / "plan.json").string());
    const auto cut = history_cut(trace, config.history_fraction);
    if (cut == 0 || cut >= trace.rows.size()) throw std::runtime_error("history or evaluation half is empty");
    const std::vector<Invocation> history(trace.rows.begin(), trace.rows.begin() + static_cast<long>(cut));
    const std::vector<Invocation> evaluation(trace.rows.begin() + static_cast<long>(cut), trace.rows.end());

    // Tier boundaries and per-key medians of the history half only, as in the simulator (no look-ahead).
    std::vector<double> durations;
    std::unordered_map<uint32_t, std::vector<double>> per_key;
    for (const auto& row : history) { durations.push_back(row.duration_ms); per_key[row.key].push_back(row.duration_ms); }
    std::vector<double> bounds;
    for (int i = 1; i < config.levels; ++i) bounds.push_back(quantile(durations, static_cast<double>(i) / config.levels));
    double work = 0, longest = 0;
    for (const auto& row : evaluation) { work += row.duration_ms; longest = std::max(longest, row.duration_ms); }
    const double span = evaluation.back().arrival_ms - evaluation.front().arrival_ms;
    if (span <= 0 || work <= 0) throw std::runtime_error("evaluation half has no span or no work");

    std::ostringstream plan;
    plan << std::setprecision(17) << "{\"version\":\"" << escape(config.version) << "\",\"time_scale\":" << config.f
         << ",\"trace\":\"" << escape(std::filesystem::path(trace_path).filename().string()) << "\",\"cut\":" << cut
         << ",\"history_rows\":" << history.size() << ",\"eval_rows\":" << evaluation.size() << ",\"span_ms\":" << span
         << ",\"work_ms\":" << work << ",\"max_duration_ms\":" << longest << ",\"boundaries_ms\":[";
    for (std::size_t i = 0; i < bounds.size(); ++i) plan << (i ? "," : "") << bounds[i];
    plan << "],\"loads\":{";

    ml::PredictiveRoutingConfig routing;
    routing.key = key_policy();
    const auto context = snapshot_context(routing);
    const auto policy = key_policy();
    bool first = true;
    for (const double u : config.loads) {
        // Offered load u x workers over the evaluation span, exactly as the simulator sets it.
        const double compress = u * config.workers * span / work;
        auto pc = predictor_config(config);
        double clock_ms = 0;  // replay time in real milliseconds; the history sits before the evaluation start (negative)
        pc.clock = [&clock_ms] { return Clock::time_point{} + std::chrono::microseconds(static_cast<int64_t>(clock_ms * 1000)); };
        ml::PerKeyPredictor predictor{pc};
        const double e0 = evaluation.front().arrival_ms;
        std::size_t learned = 0;
        for (const auto& row : history) {
            clock_ms = (row.arrival_ms - e0) / compress / config.f;
            // What the broker would be told: the consumer reports whole milliseconds, truncated.
            const auto key = ml::derive_predictor_key({{"job_type", wire_key(trace, row.key)}}, policy);
            if (key) learned += predictor.observe(*key, std::floor(row.duration_ms / config.f));
        }
        clock_ms = 0;
        const auto label = load_label(u);
        const auto file = "snapshot-" + label + ".bin";
        predictor.save(out / file, context);
        plan << (first ? "" : ",") << "\"" << label << "\":{\"compress\":" << compress << ",\"snapshot\":\"" << file
             << "\",\"learned\":" << learned << "}";
        first = false;
        std::cerr << "[replay] load " << label << ": compress " << compress << ", warm snapshot of " << predictor.stats().keys
                  << " keys from " << learned << " observations\n";
    }
    plan << "},\"static_history\":{";
    bool first_key = true;
    std::vector<std::pair<std::string, uint8_t>> tiers;
    for (auto& [key, values] : per_key) tiers.emplace_back(wire_key(trace, key), ml::tier_of(bounds, quantile(values, 0.5)));
    std::sort(tiers.begin(), tiers.end());
    for (const auto& [key, tier] : tiers) {
        plan << (first_key ? "" : ",") << "\"" << escape(key) << "\":" << static_cast<int>(tier);
        first_key = false;
    }
    plan << "}}\n";
    std::ofstream(out / "plan.json") << plan.str();
    std::cerr << "[replay] plan written: " << evaluation.size() << " evaluation rows, " << history.size() << " history rows\n";
    return 0;
}

// ── Broker ──────────────────────────────────────────────────────────────────

HarbingerConfig broker_config(const Config& c, const Plan& plan, const Arm& arm, const std::filesystem::path& out) {
    HarbingerConfig config;
    config.num_levels = c.levels;
    config.default_priority = c.default_priority;
    config.aging = AgingConfig{std::chrono::milliseconds{scaled_ms(c.aging_threshold_ms, c.f)},
                               std::chrono::milliseconds{scaled_ms(c.aging_interval_ms, c.f)}};
    // No lease may expire and nothing is retried: the lease outlasts the longest scaled job. TTL stays off.
    config.delivery_lease = std::chrono::milliseconds{
        static_cast<int64_t>(std::ceil(plan.max_duration_ms / c.f * 1.25)) + 60000};
    if (arm.weights) config.level_weights = c.weights;  // the broker turns pausing aging on with weights
    if (arm.no_aging) config.aging = std::nullopt;      // strict priority with no aging thread
    benchmark::Options options;
    options.observations = std::make_shared<benchmark::Observations>(1);
    options.static_header = "job_type";
    if (arm.route == "fifo") options.policy = benchmark::Policy::Fifo;
    else if (arm.route == "oracle") options.policy = benchmark::Policy::Oracle;
    else if (arm.route == "static_history") {
        options.policy = benchmark::Policy::Static;
        for (const auto& [key, tier] : plan.static_history) options.static_tiers[key] = tier;
    } else {
        ml::PredictiveRoutingConfig routing;
        routing.mode = ml::RoutingMode::Predictive;
        routing.key = key_policy();
        routing.predictor = predictor_config(c);
        if (arm.route == "predictive_warm") {
            // The run copies the load's prepared snapshot here: the broker saves its predictor to this path on a timer
            // and at shutdown, and must not overwrite the prepared file.
            routing.snapshot_path = out / "predictor-snapshot.bin";
            routing.snapshot_interval = std::chrono::hours{24};
        }
        config.predictive_routing = routing;
    }
    config.benchmark_options = options;
    return config;
}

int run_broker(HarbingerConfig config, const std::string& arm, const std::string& listen, const std::filesystem::path& out) {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) throw std::runtime_error("cannot block signals");
    HarbingerService service{std::move(config)};
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen, grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    if (!server || !port) throw std::runtime_error("cannot listen on " + listen);
    std::cout << "[replay] broker ready arm=" << arm << " port=" << port << std::endl;
    int signal = 0;
    if (sigwait(&signals, &signal) != 0) throw std::runtime_error("sigwait failed");
    const auto stats = stats_json(service);
    const auto dlq = service.dlq_size();
    server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds{2});
    std::ofstream(out / "broker.json") << "{\"arm\":\"" << arm << "\",\"dlq\":" << dlq << ",\"broker\":" << stats << "}\n";
    std::cout << "[replay] broker stopped arm=" << arm << " dlq=" << dlq << '\n';
    return 0;
}

// ── Client ──────────────────────────────────────────────────────────────────

struct Record {
    std::atomic<int64_t> submit_ns{0}, start_ns{0}, end_ns{0};
    std::atomic<int> attempts{0};
    std::atomic<bool> done{false};
};

double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<std::size_t>(std::max(0.0, std::ceil(static_cast<double>(v.size()) * q) - 1)))];
}

struct ClientResult { uint64_t completed, dlq, submit_errors, leases_lost; };

/// Producers and workers against `address`. Returns when every message completed, or the drain limit passed.
ClientResult run_client(const Config& c, const Plan& plan, const Arm& arm, double compress, const Trace& trace,
                        std::size_t rows, const std::string& address, const std::filesystem::path& out, const std::string& role,
                        const std::function<uint64_t()>& embedded_dlq, bool& all_done_flag) {
    const auto first = plan.cut;
    const double e0 = trace.rows[first].arrival_ms;
    std::vector<Record> records(rows);
    std::vector<int64_t> sched_ns(rows);
    const auto origin_ns_offset = std::chrono::milliseconds{3000};  // consumers connect before the first arrival
    const auto origin = Clock::now() + origin_ns_offset;
    const int64_t origin_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(origin.time_since_epoch()).count();
    for (std::size_t i = 0; i < rows; ++i)
        sched_ns[i] = origin_ns + static_cast<int64_t>((trace.rows[first + i].arrival_ms - e0) / compress / c.f * 1e6);

    std::unique_ptr<harbinger_rpc::Broker::Stub> remote;
    if (role == "client") {
        auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
        if (!channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds{30}))
            throw std::runtime_error("cannot reach broker at " + address);
        remote = harbinger_rpc::Broker::NewStub(channel);
    }
    uint64_t remote_dlq = 0;
    auto last_poll = Clock::now() - std::chrono::seconds{1};
    const auto dlq_size = [&]() -> uint64_t {
        if (embedded_dlq) return embedded_dlq();
        if (!remote) return 0;
        if (Clock::now() - last_poll >= std::chrono::seconds{1}) {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds{5});
            harbinger_rpc::InspectDlqRequest request;
            request.set_limit(1);
            harbinger_rpc::InspectDlqResponse response;
            if (remote->InspectDlq(&context, request, &response).ok()) remote_dlq = response.total_size();
            last_poll = Clock::now();
        }
        return remote_dlq;
    };

    std::atomic<std::size_t> completed{0};
    std::vector<std::shared_ptr<Consumer>> consumers;
    for (int w = 0; w < c.workers; ++w) {
        consumers.push_back(Consumer::connect(address, [&](const ReceivedMessage& message) {
            const auto found = message.headers.find("replay_idx");
            const auto idx = static_cast<std::size_t>(std::stoull(found->second));
            auto& record = records[idx];
            record.attempts.fetch_add(1);
            record.start_ns = now_ns();
            // The handler's whole job: occupy a worker for the trace's duration, in real time (scaled by f).
            std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(trace.rows[first + idx].duration_ms / c.f));
            record.end_ns = now_ns();
            if (!record.done.exchange(true)) completed.fetch_add(1);
            return AckResult::SUCCESS;
        }, std::chrono::milliseconds{200}));
    }
    for (auto& consumer : consumers) consumer->start();

    std::atomic<uint64_t> submit_errors{0};
    std::vector<std::thread> producers;
    const int threads = std::max(1, c.producers);
    for (int t = 0; t < threads; ++t) {
        producers.emplace_back([&, t] {
            auto producer = Producer::connect(address);
            for (std::size_t i = static_cast<std::size_t>(t); i < rows; i += static_cast<std::size_t>(threads)) {
                const auto& row = trace.rows[first + i];
                std::this_thread::sleep_until(Clock::time_point{} + std::chrono::nanoseconds(sched_ns[i]));
                std::unordered_map<std::string, std::string> headers{
                    {"job_type", wire_key(trace, row.key)}, {"replay_idx", std::to_string(i)}};
                if (arm.route == "oracle") headers[benchmark::kOracleTierHeader] = std::to_string(ml::tier_of(plan.bounds, row.duration_ms));
                records[i].submit_ns = now_ns();
                try {
                    producer->send(std::vector<uint8_t>(8, 0), std::move(headers));
                } catch (...) {
                    submit_errors.fetch_add(1);
                }
            }
        });
    }
    for (auto& p : producers) p.join();
    const auto arrivals_done = Clock::now();
    while (completed.load() + dlq_size() < rows &&
           Clock::now() - arrivals_done < std::chrono::duration<double>(c.drain_s))
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    all_done_flag = completed.load() == rows;
    uint64_t lost = 0;
    for (auto& consumer : consumers) lost += consumer->leases_lost();
    const auto dlq_before_stop = dlq_size();
    for (auto& consumer : consumers) consumer->stop();

    std::ofstream tsv(out / "messages.tsv");
    tsv << "idx\tkey\ttrace_arrival_ms\ttrace_duration_ms\tsched_ns\tsubmit_ns\tstart_ns\tend_ns\tattempts\tdone\n" << std::setprecision(15);
    std::vector<double> lateness_ms;
    for (std::size_t i = 0; i < rows; ++i) {
        const auto& row = trace.rows[first + i];
        const auto& r = records[i];
        tsv << i << '\t' << trace.keys[row.key] << '\t' << row.arrival_ms << '\t' << row.duration_ms << '\t' << sched_ns[i]
            << '\t' << r.submit_ns.load() << '\t' << r.start_ns.load() << '\t' << r.end_ns.load() << '\t' << r.attempts.load()
            << '\t' << (r.done.load() ? 1 : 0) << '\n';
        if (r.submit_ns.load()) lateness_ms.push_back(static_cast<double>(r.submit_ns.load() - sched_ns[i]) / 1e6);
    }
    std::ofstream timing(out / "timing.json");
    timing << "{\"origin_ns\":" << origin_ns << ",\"lateness_p50_ms\":" << percentile(lateness_ms, 0.5)
           << ",\"lateness_p99_ms\":" << percentile(lateness_ms, 0.99) << ",\"lateness_max_ms\":" << percentile(lateness_ms, 1.0)
           << "}\n";
    return {completed.load(), dlq_before_stop, submit_errors.load(), lost};
}

int run(std::map<std::string, std::string>& args) {
    const std::string role = args.contains("--role") ? args["--role"] : "all";
    // The broker never reads the trace.
    for (const char* needed : {"--arm", "--load", "--trace", "--config", "--prep", "--out"})
        if (!args.contains(needed) && !(role == "broker" && std::string(needed) == "--trace"))
            throw std::invalid_argument(std::string("missing ") + needed);
    if (role != "all" && role != "broker" && role != "client") throw std::invalid_argument("--role is all, broker or client");
    if (role == "client" && !args.contains("--broker")) throw std::invalid_argument("--role client needs --broker HOST:PORT");
    const auto config = read_config(args["--config"]);
    const auto plan = read_plan(args["--prep"]);
    const auto arm = parse_arm(args["--arm"]);
    const auto label = load_label(std::stod(args["--load"]));
    if (!plan.compress.contains(label)) throw std::invalid_argument("load " + label + " is not in the plan");
    if (plan.f != config.f) throw std::invalid_argument("the plan was prepared with another time scale");
    const std::filesystem::path out = args["--out"];
    if (std::filesystem::exists(out)) throw std::invalid_argument("refusing to overwrite " + out.string());
    std::filesystem::create_directories(out);

    // The warm arm's snapshot is the plan's file for this load, copied into the run's own directory.
    auto make_config = [&] {
        auto hc = broker_config(config, plan, arm, out);
        if (hc.predictive_routing && hc.predictive_routing->snapshot_path) {
            std::filesystem::copy_file(std::filesystem::path(args["--prep"]) / plan.snapshot.at(label),
                                       *hc.predictive_routing->snapshot_path, std::filesystem::copy_options::overwrite_existing);
        }
        return hc;
    };
    if (role == "broker")
        return run_broker(make_config(), arm.name, args.contains("--listen") ? args["--listen"] : "0.0.0.0:50051", out);

    const auto trace = load(args["--trace"]);
    if (trace.rows.size() < plan.cut + plan.eval_rows) throw std::runtime_error("the trace is shorter than the plan expects");
    if (history_cut(trace, config.history_fraction) != plan.cut) throw std::runtime_error("the trace is not the one the plan was made from");
    const double compress = plan.compress.at(label);
    auto rows = args.contains("--rows") ? std::min<std::size_t>(std::stoull(args["--rows"]), plan.eval_rows) : plan.eval_rows;
    if (args.contains("--seconds")) {
        // Messages scheduled within S seconds of real time: arrival offset / (compress x scale) <= S.
        const double limit = trace.rows[plan.cut].arrival_ms + std::stod(args["--seconds"]) * 1000.0 * compress * config.f;
        const auto within = static_cast<std::size_t>(std::upper_bound(trace.rows.begin() + static_cast<long>(plan.cut),
            trace.rows.begin() + static_cast<long>(plan.cut + plan.eval_rows), limit,
            [](double t, const Invocation& row) { return t < row.arrival_ms; }) - (trace.rows.begin() + static_cast<long>(plan.cut)));
        rows = std::min(rows, within);
    }
    if (rows == 0) throw std::invalid_argument("no message falls in the requested prefix");

    std::unique_ptr<HarbingerService> service;
    std::unique_ptr<grpc::Server> server;
    std::string address;
    if (role == "all") {
        service = std::make_unique<HarbingerService>(make_config());
        int port = 0;
        grpc::ServerBuilder builder;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(service.get());
        server = builder.BuildAndStart();
        if (!server || !port) throw std::runtime_error("cannot start broker");
        address = "127.0.0.1:" + std::to_string(port);
    } else {
        address = args["--broker"];
    }
    bool all_done = false;
    const auto result = run_client(config, plan, arm, compress, trace, rows, address, out, role,
                                   service ? std::function<uint64_t()>([&] { return static_cast<uint64_t>(service->dlq_size()); })
                                           : std::function<uint64_t()>{}, all_done);
    const auto stats = service ? stats_json(*service) : std::string{};
    if (server) server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds{2});

    std::ifstream timing_file(out / "timing.json");
    const std::string timing{std::istreambuf_iterator<char>(timing_file), {}};
    std::ofstream run(out / (service ? "run.json" : "client.json"));
    run << "{\"benchmark\":\"replay-v1\",\"config_version\":\"" << escape(config.version) << "\",\"arm\":\"" << escape(arm.name)
        << "\",\"load\":" << label << ",\"seconds_limit\":" << (args.contains("--seconds") ? args["--seconds"] : "null")
        << ",\"rows_limit\":" << (args.contains("--rows") ? args["--rows"] : "null") << ",\"compress\":" << compress << ",\"time_scale\":" << config.f
        << ",\"workers\":" << config.workers << ",\"messages\":" << rows << ",\"completed\":" << result.completed
        << ",\"dlq\":" << result.dlq << ",\"submit_errors\":" << result.submit_errors << ",\"leases_lost\":" << result.leases_lost
        << ",\"timing\":" << (timing.empty() ? "{}" : timing.substr(0, timing.find_last_not_of('\n') + 1));
    if (service) run << ",\"broker\":" << stats;
    else run << ",\"role\":\"client\",\"broker_address\":\"" << escape(address) << "\"";
    run << "}\n";
    std::filesystem::remove(out / "timing.json");
    std::cout << "[replay] arm=" << arm.name << " load=" << label << " messages=" << rows << " completed=" << result.completed
              << " dlq=" << result.dlq << " leases_lost=" << result.leases_lost << '\n';
    return all_done && result.dlq == 0 ? 0 : 3;
}

} // namespace

int main(int argc, char** argv) try {
    const std::string usage =
        "usage: harbinger_replay prepare TRACE CONFIG.json OUT_DIR\n"
        "       harbinger_replay run --arm ARM --load U --trace TRACE --config CONFIG.json --prep DIR --out NEW_DIR\n"
        "                            [--role all|broker|client] [--broker HOST:PORT] [--listen ADDR] [--rows N] [--seconds S]";
    if (argc >= 2 && std::string(argv[1]) == "prepare") {
        if (argc != 5) throw std::invalid_argument(usage);
        return prepare(argv[2], argv[3], argv[4]);
    }
    if (argc >= 2 && std::string(argv[1]) == "run" && argc % 2 == 0) {
        std::map<std::string, std::string> args;
        for (int i = 2; i + 1 < argc; i += 2) args[argv[i]] = argv[i + 1];
        return run(args);
    }
    throw std::invalid_argument(usage);
} catch (const std::exception& error) {
    std::cerr << "[replay] " << error.what() << '\n';
    return 2;
}
