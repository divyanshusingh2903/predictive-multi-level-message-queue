// Broker-cost microbenchmark (#37; E4 in #46). Producers publish small messages to Harbinger (harbinger_server),
// RabbitMQ or Redis, and consumers acknowledge each one at once, so the numbers are the broker's and its client's own
// cost, with no work in the handlers. Producers and consumers run in this one process, so every latency is measured on
// one clock even when the broker runs on another host.
//
//   harbinger_brokerbench --kind harbinger|rabbitmq|redis --broker HOST:PORT --out NEW_FILE.json
//       [--rate MSG_PER_S (0 = as fast as possible)] [--seconds 20] [--warmup-s 2] [--drain-s 30]
//       [--producers 4] [--consumers 4] [--payload-bytes 64] [--raw NEW_FILE.tsv]
//
// At a fixed rate the schedule is open loop: message i is due at i / rate seconds, and the producers share the
// schedule round robin. A producer that falls behind sends late rather than skipping, and the lateness is reported,
// so a broker that cannot accept the rate shows it. With --rate 0 each producer sends as fast as it can until
// --seconds. Measured messages are those due (or sent) after the warm-up. Latency is publish start to delivery start.
// Harbinger needs a fresh server per run; RabbitMQ and Redis queues are emptied at the start.
#include "consumer/consumer.hpp"
#include "producer/producer.hpp"
#ifdef HARBINGER_BROKER_ADAPTERS
#include "broker_adapters.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace harbinger;
using Clock = std::chrono::steady_clock;

namespace {

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

/// Upper bound on messages in one run (as-fast-as-possible runs stop sending when they reach it).
constexpr std::size_t kMaxMessages = 8'000'000;

struct Stats { double p50, p90, p99, p999, max; };

Stats percentiles(std::vector<double> v) {
    if (v.empty()) return {0, 0, 0, 0, 0};
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) {
        return v[std::min(v.size() - 1, static_cast<std::size_t>(std::max(0.0, std::ceil(q * static_cast<double>(v.size())) - 1)))];
    };
    return {at(0.5), at(0.9), at(0.99), at(0.999), v.back()};
}

std::string json(const Stats& s) {
    std::ostringstream o;
    o << "{\"p50\":" << s.p50 << ",\"p90\":" << s.p90 << ",\"p99\":" << s.p99 << ",\"p999\":" << s.p999
      << ",\"max\":" << s.max << "}";
    return o.str();
}

} // namespace

int main(int argc, char** argv) try {
    std::map<std::string, std::string> args;
    for (int i = 1; i + 1 < argc; i += 2) args[argv[i]] = argv[i + 1];
    if (argc % 2 == 0 || !args.contains("--kind") || !args.contains("--broker") || !args.contains("--out"))
        throw std::invalid_argument("usage: harbinger_brokerbench --kind harbinger|rabbitmq|redis --broker HOST:PORT "
            "--out NEW_FILE.json [--rate 0] [--seconds 20] [--warmup-s 2] [--drain-s 30] [--producers 4] "
            "[--consumers 4] [--payload-bytes 64] [--raw NEW_FILE.tsv]");
    const auto number = [&](const char* key, double fallback) { return args.contains(key) ? std::stod(args[key]) : fallback; };
    const std::string kind = args["--kind"], address = args["--broker"];
    const double rate = number("--rate", 0), seconds = number("--seconds", 20), warmup = number("--warmup-s", 2);
    const double drain = number("--drain-s", 30);
    const int producers = static_cast<int>(number("--producers", 4)), consumers = static_cast<int>(number("--consumers", 4));
    const auto payload_bytes = static_cast<std::size_t>(number("--payload-bytes", 64));
    if (kind != "harbinger" && kind != "rabbitmq" && kind != "redis") throw std::invalid_argument("unknown --kind " + kind);
    if (rate < 0 || seconds <= warmup || producers < 1 || consumers < 1) throw std::invalid_argument("bad rate, duration or counts");
    const std::filesystem::path out = args["--out"];
    for (const char* key : {"--out", "--raw"})
        if (args.contains(key) && std::filesystem::exists(args[key])) throw std::invalid_argument(std::string("refusing to overwrite ") + args[key]);
#ifndef HARBINGER_BROKER_ADAPTERS
    if (kind != "harbinger") throw std::invalid_argument(kind + " needs a build with -DHARBINGER_BUILD_BROKER_ADAPTERS=ON");
#endif

    const std::size_t planned = rate > 0 ? static_cast<std::size_t>(rate * seconds) : kMaxMessages;
    if (planned > kMaxMessages) throw std::invalid_argument("rate x seconds exceeds the message cap");
    std::vector<std::atomic<int64_t>> sent(planned), delivered(planned);
    std::vector<int64_t> due(rate > 0 ? planned : 0);
    std::atomic<uint64_t> deliveries{0}, duplicates{0}, publish_errors{0};
    const std::string payload(payload_bytes, 'x');

    const auto on_delivery = [&](uint64_t seq) {
        const int64_t t = now_ns();
        if (seq >= planned) return true;
        int64_t expected = 0;
        if (!delivered[seq].compare_exchange_strong(expected, t)) duplicates.fetch_add(1);
        deliveries.fetch_add(1);
        return true;
    };

    // ── Consumers ───────────────────────────────────────────────────────────
    std::vector<std::shared_ptr<Consumer>> harbinger_consumers;
    std::string server_version = kind == "harbinger" ? "harbinger_server" : "", client_library = "harbinger gRPC client";
#ifdef HARBINGER_BROKER_ADAPTERS
    std::unique_ptr<bench::Workers> workers;
    const auto broker_kind = kind == "redis" ? bench::BrokerKind::Redis : bench::BrokerKind::RabbitMq;
    if (kind != "harbinger") {
        const auto endpoint = bench::parse_endpoint(address);
        server_version = bench::prepare_queue(broker_kind, endpoint, false);
        client_library = bench::client_library(broker_kind);
        workers = bench::start_workers(broker_kind, endpoint, consumers, on_delivery);
    }
#endif
    if (kind == "harbinger") {
        for (int i = 0; i < consumers; ++i)
            harbinger_consumers.push_back(Consumer::connect(address, [&](const ReceivedMessage& message) {
                on_delivery(std::stoull(message.headers.at("bench_seq")));
                return AckResult::SUCCESS;
            }, std::chrono::milliseconds{200}));
        for (auto& c : harbinger_consumers) c->start();
    }

    // ── Producers ───────────────────────────────────────────────────────────
    std::this_thread::sleep_for(std::chrono::milliseconds{300});  // let consumers connect before the clock starts
    const int64_t origin = now_ns() + 200'000'000;
    const int64_t stop_at = origin + static_cast<int64_t>(seconds * 1e9);
    for (std::size_t i = 0; i < due.size(); ++i) due[i] = origin + static_cast<int64_t>(static_cast<double>(i) * 1e9 / rate);
    std::atomic<std::size_t> next{0};  // as-fast-as-possible: shared sequence
    std::atomic<bool> capped{false};
    std::vector<std::thread> threads;
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            std::function<void(uint64_t)> publish;
            std::shared_ptr<Producer> harbinger_producer;
#ifdef HARBINGER_BROKER_ADAPTERS
            std::unique_ptr<bench::Publisher> publisher;
            if (kind != "harbinger") {
                publisher = bench::make_publisher(broker_kind, bench::parse_endpoint(address));
                publish = [&](uint64_t seq) { publisher->publish(seq, payload, std::nullopt); };
            }
#endif
            if (kind == "harbinger") {
                harbinger_producer = Producer::connect(address);
                publish = [&](uint64_t seq) {
                    harbinger_producer->send(std::vector<uint8_t>(payload.begin(), payload.end()), {{"bench_seq", std::to_string(seq)}});
                };
            }
            const auto send = [&](uint64_t seq) {
                sent[seq] = now_ns();
                try { publish(seq); } catch (...) { publish_errors.fetch_add(1); }
            };
            if (rate > 0) {
                for (std::size_t seq = static_cast<std::size_t>(p); seq < planned; seq += static_cast<std::size_t>(producers)) {
                    std::this_thread::sleep_until(Clock::time_point{std::chrono::nanoseconds{due[seq]}});
                    send(seq);
                }
                return;
            }
            std::this_thread::sleep_until(Clock::time_point{std::chrono::nanoseconds{origin}});
            while (now_ns() < stop_at) {
                const auto seq = next.fetch_add(1);
                if (seq >= planned) { capped = true; return; }
                send(seq);
            }
        });
    }
    for (auto& t : threads) t.join();
    const std::size_t total = rate > 0 ? planned : std::min(next.load(), planned);
    const int64_t sending_done = now_ns();
    while (deliveries.load() - duplicates.load() < total - publish_errors.load() &&
           now_ns() - sending_done < static_cast<int64_t>(drain * 1e9))
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    for (auto& c : harbinger_consumers) c->stop();
    std::string worker_error;
#ifdef HARBINGER_BROKER_ADAPTERS
    if (workers) { workers->stop(); worker_error = workers->error(); }
#endif

    // ── Results ─────────────────────────────────────────────────────────────
    const int64_t window_start = origin + static_cast<int64_t>(warmup * 1e9);
    std::vector<double> latency_ms, lateness_ms;
    std::size_t measured = 0, lost = 0, delivered_in_window = 0, sent_in_window = 0;
    for (std::size_t i = 0; i < total; ++i) {
        const int64_t s = sent[i].load(), d = delivered[i].load();
        if (d >= window_start && d < stop_at) ++delivered_in_window;
        if (s >= window_start && s < stop_at) ++sent_in_window;
        const bool in_window = rate > 0 ? due[i] >= window_start : s >= window_start;
        if (!in_window || s == 0) continue;
        ++measured;
        if (rate > 0) lateness_ms.push_back(static_cast<double>(s - due[i]) / 1e6);
        if (d == 0) { ++lost; continue; }
        latency_ms.push_back(static_cast<double>(d - s) / 1e6);
    }
    const double window_s = seconds - warmup;
    std::ofstream o(out);
    o << "{\"benchmark\":\"brokerbench-v1\",\"kind\":\"" << kind << "\",\"broker\":\"" << address
      << "\",\"server_version\":\"" << server_version << "\",\"client_library\":\"" << client_library
      << "\",\"rate\":" << rate << ",\"seconds\":" << seconds << ",\"warmup_s\":" << warmup
      << ",\"producers\":" << producers << ",\"consumers\":" << consumers << ",\"payload_bytes\":" << payload_bytes
      << ",\"messages\":" << total << ",\"measured\":" << measured << ",\"lost\":" << lost
      << ",\"undelivered_total\":" << (total - std::min<std::size_t>(total, deliveries.load() - duplicates.load()))
      << ",\"duplicates\":" << duplicates.load() << ",\"publish_errors\":" << publish_errors.load()
      << ",\"capped\":" << (capped ? "true" : "false")
      << ",\"send_rate_per_s\":" << static_cast<double>(sent_in_window) / window_s
      << ",\"delivery_rate_per_s\":" << static_cast<double>(delivered_in_window) / window_s
      << ",\"latency_ms\":" << json(percentiles(latency_ms));
    if (rate > 0) o << ",\"lateness_ms\":" << json(percentiles(lateness_ms));
    if (!worker_error.empty()) o << ",\"worker_error\":\"" << worker_error << "\"";
    o << "}\n";
    o.close();
    if (!o) throw std::runtime_error("cannot write " + out.string());
    if (args.contains("--raw")) {
        std::ofstream raw(args["--raw"]);
        raw << "seq\tdue_ns\tsent_ns\tdelivered_ns\n";
        for (std::size_t i = 0; i < total; ++i)
            raw << i << '\t' << (rate > 0 ? due[i] : 0) << '\t' << sent[i].load() << '\t' << delivered[i].load() << '\n';
    }
    const auto l = percentiles(latency_ms);
    std::cout << "[brokerbench] kind=" << kind << " rate=" << rate << " messages=" << total << " lost=" << lost
              << " send/s=" << static_cast<double>(sent_in_window) / window_s
              << " delivered/s=" << static_cast<double>(delivered_in_window) / window_s
              << " latency ms p50=" << l.p50 << " p99=" << l.p99 << " max=" << l.max << '\n';
    return lost == 0 && publish_errors.load() == 0 && worker_error.empty() ? 0 : 3;
} catch (const std::exception& error) {
    std::cerr << "[brokerbench] " << error.what() << '\n';
    return 2;
}
