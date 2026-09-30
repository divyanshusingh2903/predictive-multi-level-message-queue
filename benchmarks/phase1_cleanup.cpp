#include "consumer/consumer.hpp"
#include "harbinger_service.hpp"
#include "producer/producer.hpp"
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace harbinger;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {
int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

void report(std::vector<int64_t> samples, Clock::duration elapsed, std::size_t outcomes) {
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](std::size_t p) {
        return samples.empty() ? 0 : samples[(samples.size() - 1) * p / 100];
    };
    std::cout << "outcomes=" << outcomes
              << " elapsed_s=" << std::chrono::duration<double>(elapsed).count()
              << " throughput_per_s=" << outcomes / std::chrono::duration<double>(elapsed).count()
              << " p50_us=" << percentile(50) / 1000.0
              << " p95_us=" << percentile(95) / 1000.0
              << " p99_us=" << percentile(99) / 1000.0 << '\n';
}

void queue_run(std::size_t backlog, std::size_t iterations, std::size_t workers,
               const std::string& pattern) {
    if (pattern != "young" && pattern != "mixed" && pattern != "dense" && pattern != "off")
        throw std::invalid_argument("queue pattern must be young, mixed, dense, or off");
    MultiLevelQueue q{3, pattern == "off" ? std::nullopt
                                         : std::optional<AgingConfig>{{1h, 10ms}}};
    const auto place = [&](Message msg, std::size_t sequence) {
        msg.priority = 2;
        if (pattern == "dense" || (pattern == "mixed" && sequence % 100 == 0)) {
            msg.enqueue_time = Clock::now() - 2h;
            q.requeue_front(std::move(msg));
        } else {
            q.enqueue(std::move(msg));
        }
    };
    for (std::size_t i = 0; i < backlog; ++i) {
        Message msg;
        msg.id = std::to_string(i);
        msg.original_priority = 2;
        place(std::move(msg), i);
    }
    std::vector<std::vector<int64_t>> per_worker(workers);
    std::vector<std::thread> threads;
    std::atomic<std::size_t> min_backlog{backlog};
    const auto start = Clock::now();
    for (std::size_t w = 0; w < workers; ++w) {
        threads.emplace_back([&, w] {
            auto& samples = per_worker[w];
            samples.reserve(iterations);
            for (std::size_t i = 0; i < iterations; ++i) {
                const auto t0 = now_ns();
                auto msg = q.dequeue(1s);
                if (!msg) throw std::runtime_error("queue unexpectedly empty");
                const auto depth = q.size();
                auto minimum = min_backlog.load();
                while (depth < minimum && !min_backlog.compare_exchange_weak(minimum, depth)) {}
                place(std::move(*msg), i + w);
                samples.push_back(now_ns() - t0);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    const auto elapsed = Clock::now() - start;
    std::vector<int64_t> samples;
    for (auto& values : per_worker) samples.insert(samples.end(), values.begin(), values.end());
    std::cout << "mode=queue pattern=" << pattern << " backlog=" << backlog
              << " min_observed_backlog=" << min_backlog << " final_backlog=" << q.size()
              << " workers=" << workers << " iterations_per_worker=" << iterations
              << " aging_threshold_ms=3600000 aging_interval_ms=10\n";
    report(std::move(samples), elapsed, workers * iterations);
}

void delivery_run(std::size_t count, std::size_t payload_size,
                  std::size_t header_count, std::size_t workers) {
    HarbingerService service;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    if (!server || !port) throw std::runtime_error("cannot start benchmark broker");
    struct Guard {
        grpc::Server& server;
        ~Guard() { server.Shutdown(); server.Wait(); }
    } guard{*server};
    const auto address = "127.0.0.1:" + std::to_string(port);
    auto producer = Producer::connect(address);
    std::vector<uint8_t> payload(payload_size);
    std::mt19937 random{42};
    for (auto& byte : payload) byte = static_cast<uint8_t>(random());
    std::unordered_map<std::string, std::string> headers;
    for (std::size_t i = 0; i < header_count; ++i) headers["header-" + std::to_string(i)] = "value";
    std::vector<std::atomic<int64_t>> submitted(count);
    std::vector<std::vector<int64_t>> per_worker(workers);
    std::vector<std::shared_ptr<Consumer>> consumers;
    for (std::size_t w = 0; w < workers; ++w) {
        per_worker[w].reserve(count);
        consumers.push_back(Consumer::connect(address, [&, w](const ReceivedMessage& msg) {
            const auto sequence = std::stoull(msg.headers.at("__benchmark_sequence"));
            per_worker[w].push_back(now_ns() - submitted.at(sequence).load());
            return AckResult::SUCCESS;
        }, 100ms));
    }
    for (const auto& consumer : consumers) consumer->start();
    const auto start = Clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        headers["__benchmark_sequence"] = std::to_string(i);
        submitted[i] = now_ns();
        (void)producer->send(payload, headers);
    }
    const auto acked = [&] {
        std::size_t total = 0;
        for (const auto& consumer : consumers) total += consumer->messages_acked();
        return total;
    };
    const auto deadline = Clock::now() + 60s;
    while (acked() < count && Clock::now() < deadline) std::this_thread::sleep_for(1ms);
    const auto elapsed = Clock::now() - start;
    for (const auto& consumer : consumers) consumer->stop();
    if (acked() != count) throw std::runtime_error("benchmark did not confirm every Ack");
    std::vector<int64_t> samples;
    for (auto& values : per_worker) samples.insert(samples.end(), values.begin(), values.end());
    std::cout << "mode=delivery count=" << count << " payload_bytes=" << payload_size
              << " application_headers=" << header_count << " metadata_headers=2 workers=" << workers
              << " seed=42 handler_cost=0 aging=off lease_ms=30000\n";
    report(std::move(samples), elapsed, acked());
}
}

int main(int argc, char** argv) try {
    if (argc != 6) {
        std::cout << "Usage:\n  harbinger_cleanup_benchmark queue BACKLOG ITERATIONS WORKERS young|mixed|dense|off\n"
                  << "  harbinger_cleanup_benchmark delivery COUNT PAYLOAD_BYTES HEADERS WORKERS\n";
        return argc == 1 ? 0 : 1;
    }
    const auto a = std::stoull(argv[2]);
    const auto b = std::stoull(argv[3]);
    const auto c = std::stoull(argv[4]);
    if (std::string{argv[1]} == "queue") {
        if (a == 0 || b == 0 || c == 0 || a < c) throw std::invalid_argument("invalid queue dimensions");
        queue_run(a, b, c, argv[5]);
    } else if (std::string{argv[1]} == "delivery") {
        const auto workers = std::stoull(argv[5]);
        if (a == 0 || workers == 0) throw std::invalid_argument("count/workers must be positive");
        delivery_run(a, b, c, workers);
    } else throw std::invalid_argument("unknown mode");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
