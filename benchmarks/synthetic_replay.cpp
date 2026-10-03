#include "harbinger_service.hpp"
#include "benchmark/hooks.hpp"
#include <grpcpp/grpcpp.h>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>

using namespace harbinger;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {
uint64_t integer(const std::string& value) {
    uint64_t number{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::invalid_argument("invalid unsigned decimal: " + value);
    return number;
}
struct Job { uint64_t sequence, arrival_us, cost_us; std::string job; uint64_t failures, abandons; };
struct Submission { std::string id; int status{-1}; int64_t start{0}, end{0}; };
struct Runtime { uint64_t sequence, attempt; int64_t start, end; int status; std::string operation; };
void deadline(grpc::ClientContext& ctx, int ms = 5000) {
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds{ms});
}
void check(const grpc::Status& status) {
    if (!status.ok()) throw std::runtime_error(status.error_message());
}
std::map<std::string, std::string> configuration(const std::string& path) {
    std::ifstream stream(path);
    if (!stream) throw std::invalid_argument("cannot open config");
    std::map<std::string, std::string> config;
    std::string line;
    while (std::getline(stream, line)) {
        const auto separator = line.find(' ');
        if (separator == std::string::npos || separator == 0 || separator + 1 == line.size())
            throw std::invalid_argument("invalid config row");
        if (!config.emplace(line.substr(0, separator), line.substr(separator + 1)).second)
            throw std::invalid_argument("duplicate config key");
    }
    const std::vector<std::string> required{"policy", "workers", "submitters", "drain_ms", "lease_ms",
        "ttl_ms", "max_retries", "aging_threshold_ms", "aging_interval_ms", "ttl_sweep_ms",
        "lease_sweep_ms", "observation_capacity", "feedback", "tier0", "tier1", "tier2", "levels", "default_priority"};
    if (config.size() != required.size()) throw std::invalid_argument("wrong config fields");
    for (const auto& name : required) if (!config.contains(name)) throw std::invalid_argument("missing config field");
    return config;
}
std::vector<Job> trace(const std::string& path) {
    std::ifstream stream(path);
    std::string line;
    if (!std::getline(stream, line) || line != "trace-v1") throw std::invalid_argument("trace version");
    std::vector<Job> jobs;
    while (std::getline(stream, line)) {
        std::istringstream row(line);
        std::string seq, arrival, cost, job, failures, abandons, extra;
        if (!(row >> seq >> arrival >> cost >> job >> failures >> abandons) || row >> extra)
            throw std::invalid_argument("trace row");
        Job item{integer(seq), integer(arrival), integer(cost), job, integer(failures), integer(abandons)};
        if (item.sequence != jobs.size() || item.arrival_us > 3600000000ULL ||
            (!jobs.empty() && item.arrival_us < jobs.back().arrival_us) || !item.cost_us ||
            item.cost_us > 60000000 || (job != "class0" && job != "class1" && job != "class2") ||
            item.failures > 100 || item.abandons > 100 || jobs.size() >= 1000000)
            throw std::invalid_argument("invalid trace dimensions");
        jobs.push_back(item);
    }
    if (jobs.empty()) throw std::invalid_argument("empty trace");
    return jobs;
}
} // namespace

int main(int argc, char** argv) try {
    if (argc != 4) throw std::invalid_argument("usage: harbinger_synthetic_benchmark TRACE CONFIG EXISTING_OUTPUT_DIR");
    const auto jobs = trace(argv[1]);
    const auto values = configuration(argv[2]);
    const auto number = [&](const char* name) { return integer(values.at(name)); };
    const auto workers = number("workers"), submitters = number("submitters");
    if (!workers || workers > 256 || !submitters || submitters > 256 ||
        !number("drain_ms") || number("drain_ms") > 3600000 || number("lease_ms") > 3600000 ||
        number("ttl_ms") > 3600000 || number("max_retries") > 100 ||
        !number("levels") || number("levels") > 255 || number("default_priority") >= number("levels") ||
        number("aging_threshold_ms") > 3600000 || number("aging_interval_ms") > 3600000 ||
        number("ttl_sweep_ms") > 3600000 || number("lease_sweep_ms") > 3600000 ||
        number("observation_capacity") > 10000000)
        throw std::invalid_argument("invalid runner dimensions");
    const std::filesystem::path output{argv[3]};
    if (!std::filesystem::is_directory(output)) throw std::invalid_argument("output directory must exist");
    for (const auto* name : {"observations.tsv", "submissions.tsv", "runtimes.tsv", "summary.tsv"})
        if (std::filesystem::exists(output / name)) throw std::invalid_argument("refusing to overwrite prior results");
    HarbingerConfig config;
    config.num_levels = static_cast<uint8_t>(number("levels"));
    config.default_priority = static_cast<uint8_t>(number("default_priority"));
    config.delivery_lease = std::chrono::milliseconds{number("lease_ms")};
    config.default_ttl = std::chrono::milliseconds{number("ttl_ms")};
    config.default_max_retries = static_cast<uint32_t>(number("max_retries"));
    config.ttl_sweep_interval = std::chrono::milliseconds{number("ttl_sweep_ms")};
    config.lease_sweep_interval = std::chrono::milliseconds{number("lease_sweep_ms")};
    if (number("aging_threshold_ms"))
        config.aging = AgingConfig{std::chrono::milliseconds{number("aging_threshold_ms")},
                                  std::chrono::milliseconds{number("aging_interval_ms")}};
    else if (number("aging_interval_ms")) throw std::invalid_argument("aging off requires zero interval");
    auto observations = std::make_shared<benchmark::Observations>(number("observation_capacity"));
    benchmark::Options options;
    options.observations = observations;
    const std::map<std::string, benchmark::Policy> policies{{"fifo", benchmark::Policy::Fifo},
        {"disabled", benchmark::Policy::Disabled}, {"static", benchmark::Policy::Static},
        {"round_robin", benchmark::Policy::RoundRobin}};
    options.policy = policies.at(values.at("policy"));
    for (std::size_t i = 0; i < 3; ++i) {
        const auto tier = integer(values.at("tier" + std::to_string(i)));
        if (tier >= config.num_levels) throw std::invalid_argument("tier outside levels");
        options.job_tiers[i] = static_cast<uint8_t>(tier);
    }
    config.benchmark_options = options;
    config.ingress_features = ml::IngressFeatureConfig{
        .schema = {.version = "synthetic-job-v1", .headers = {{.name = "job",
            .type = ml::FeatureType::Categorical, .vocabulary = {"class0", "class1", "class2"}}}},
        .routing_policy_version = "baseline-v1-" + values.at("policy") + "-l" + values.at("levels") +
            "-d" + values.at("default_priority") + "-t" + values.at("tier0") + "-" + values.at("tier1") + "-" + values.at("tier2")};
    if (values.at("feedback") != "off") config.feedback = ml::FeedbackConfig{.path = values.at("feedback")};
    HarbingerService service{config};
    grpc::ServerBuilder builder;
    int port{};
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    if (!server || !port) throw std::runtime_error("broker startup failed");
    struct Guard { grpc::Server& server; ~Guard() { server.Shutdown(); server.Wait(); } } guard{*server};
    const auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
    auto admin = harbinger_rpc::Broker::NewStub(channel);
    harbinger_rpc::RegisterProducerRequest producer_req;
    harbinger_rpc::RegisterProducerResponse producer_resp;
    { grpc::ClientContext ctx; deadline(ctx); check(admin->RegisterProducer(&ctx, producer_req, &producer_resp)); }
    std::vector<std::string> owners;
    for (std::size_t i = 0; i < workers; ++i) {
        harbinger_rpc::RegisterConsumerRequest req; harbinger_rpc::RegisterConsumerResponse resp;
        grpc::ClientContext ctx; deadline(ctx); check(admin->RegisterConsumer(&ctx, req, &resp));
        owners.push_back(resp.consumer_id());
    }
    std::vector<Submission> submissions(jobs.size());
    std::vector<uint64_t> attempts(jobs.size());
    std::mutex attempts_mutex, runtime_mutex, submission_mutex, pull_mutex;
    std::vector<grpc::ClientContext*> active_pulls(workers, nullptr);
    std::vector<Runtime> runtimes;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> pull_errors{0};
    const auto origin = Clock::now() + 100ms;
    const auto origin_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(origin.time_since_epoch()).count();
    std::deque<std::size_t> pending;
    std::mutex pending_mutex;
    std::condition_variable pending_cv;
    bool producer_done{false};
    std::vector<std::jthread> consumers;
    std::vector<std::jthread> producers;
    struct ThreadGuard {
        std::atomic<bool>& stop;
        std::mutex& mutex;
        std::condition_variable& cv;
        bool& done;
        ~ThreadGuard() {
            stop = true;
            { std::lock_guard lock{mutex}; done = true; }
            cv.notify_all();
        }
    } thread_guard{stop, pending_mutex, pending_cv, producer_done};
    for (std::size_t worker = 0; worker < workers; ++worker) consumers.emplace_back([&, worker] {
        auto stub = harbinger_rpc::Broker::NewStub(channel);
        std::this_thread::sleep_until(origin);
        while (!stop.load()) {
            harbinger_rpc::PullRequest req; harbinger_rpc::PullResponse resp;
            req.set_consumer_id(owners[worker]); req.set_timeout_ms(50);
            grpc::ClientContext ctx; deadline(ctx, 550);
            { std::lock_guard lock{pull_mutex};
              if (stop.load()) break;
              active_pulls[worker] = &ctx; }
            const auto status = stub->Pull(&ctx, req, &resp);
            { std::lock_guard lock{pull_mutex}; active_pulls[worker] = nullptr; }
            if (!status.ok()) {
                if (stop.load()) break;
                ++pull_errors;
                if (status.error_code() != grpc::StatusCode::UNAVAILABLE &&
                    status.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED) break;
                for (int pause = 0; pause < 20 && !stop.load(); ++pause) std::this_thread::sleep_for(10ms);
                continue;
            }
            if (resp.timed_out()) continue;
            const auto& msg = resp.message();
            const auto sequence = integer(msg.headers().at("__benchmark_sequence"));
            const auto& job = jobs.at(sequence);
            uint64_t attempt;
            { std::lock_guard lock{attempts_mutex}; attempt = ++attempts[sequence]; }
            const auto start = benchmark::now_ns();
            std::this_thread::sleep_for(std::chrono::microseconds{job.cost_us});
            const auto end = benchmark::now_ns();
            Runtime runtime{sequence, attempt, start, end, 0, "ack"};
            if (attempt <= job.abandons) runtime.operation = "abandon";
            else {
                const bool fail = attempt <= job.abandons + job.failures;
                runtime.operation = fail ? "nack" : "ack";
                // Replay the identical settlement on transient failures, without rerunning the handler.
                for (int retry = 0; retry < 3; ++retry) {
                    grpc::ClientContext settle; deadline(settle);
                    grpc::Status settled;
                    if (fail) {
                        harbinger_rpc::NackRequest request; harbinger_rpc::NackResponse response;
                        request.set_consumer_id(owners[worker]); request.set_message_id(msg.message_id());
                        request.set_attempt_token(msg.attempt_token()); request.set_processing_time_ms((end - start) / 1000000);
                        settled = stub->Nack(&settle, request, &response);
                    } else {
                        harbinger_rpc::AckRequest request; harbinger_rpc::AckResponse response;
                        request.set_consumer_id(owners[worker]); request.set_message_id(msg.message_id());
                        request.set_attempt_token(msg.attempt_token()); request.set_processing_time_ms((end - start) / 1000000);
                        settled = stub->Ack(&settle, request, &response);
                    }
                    runtime.status = settled.error_code();
                    if (settled.ok() || (settled.error_code() != grpc::StatusCode::UNAVAILABLE &&
                                        settled.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds{100 * (retry + 1)});
                }
            }
            std::lock_guard lock{runtime_mutex}; runtimes.push_back(runtime);
            if (runtime.status != 0 && runtime.status != grpc::StatusCode::FAILED_PRECONDITION) break;
        }
    });
    for (std::size_t i = 0; i < submitters; ++i) producers.emplace_back([&] {
        auto stub = harbinger_rpc::Broker::NewStub(channel);
        for (;;) {
            std::size_t sequence;
            { std::unique_lock lock{pending_mutex}; pending_cv.wait(lock, [&] { return producer_done || !pending.empty(); });
              if (pending.empty()) return;
              sequence = pending.front(); pending.pop_front(); }
            const auto& job = jobs[sequence];
            harbinger_rpc::SubmitRequest req; harbinger_rpc::SubmitResponse resp;
            req.set_producer_id(producer_resp.producer_id()); req.set_payload(std::string(16, '\0'));
            (*req.mutable_headers())["job"] = job.job;
            (*req.mutable_headers())["__benchmark_sequence"] = std::to_string(sequence);
            Submission submitted;
            submitted.start = benchmark::now_ns();
            grpc::ClientContext ctx; deadline(ctx);
            const auto status = stub->Submit(&ctx, req, &resp);
            submitted.end = benchmark::now_ns(); submitted.status = status.error_code(); submitted.id = resp.message_id();
            { std::lock_guard lock{submission_mutex}; submissions[sequence] = std::move(submitted); }
        }
    });
    for (const auto& job : jobs) {
        std::this_thread::sleep_until(origin + std::chrono::microseconds{job.arrival_us});
        { std::lock_guard lock{pending_mutex};
          if (pending.size() >= submitters) { submissions[job.sequence].status = -2; continue; }
          pending.push_back(job.sequence); }
        pending_cv.notify_one();
    }
    { std::lock_guard lock{pending_mutex}; producer_done = true; }
    pending_cv.notify_all();
    const auto cutoff = origin + std::chrono::microseconds{jobs.back().arrival_us} +
                        std::chrono::milliseconds{number("drain_ms")};
    std::this_thread::sleep_until(cutoff);
    stop = true;
    { std::lock_guard lock{pull_mutex};
      for (auto* ctx : active_pulls) if (ctx) ctx->TryCancel(); }
    const auto cutoff_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(cutoff.time_since_epoch()).count();
    const auto queued = service.queue_size(), in_flight = service.in_flight_count();
    consumers.clear(); producers.clear();
    server->Shutdown(); server->Wait(); service.benchmark_close_feedback();
    auto records = observations->snapshot();
    std::ofstream obs(output / "observations.tsv"), submit(output / "submissions.tsv"), runtime(output / "runtimes.tsv"), summary(output / "summary.tsv");
    if (!obs || !submit || !runtime || !summary) throw std::runtime_error("cannot open results");
    obs << "message_id\tsequence\tattempt\tarrival_ns\ttime_ns\tpublished_ns\tretries\tkind\n";
    for (const auto& record : records) if (record.time_ns <= cutoff_ns)
        obs << record.message_id.data() << '\t' << record.sequence << '\t' << record.attempt << '\t'
            << record.arrival_ns - origin_ns << '\t' << record.time_ns - origin_ns << '\t'
            << record.published_ns - origin_ns << '\t'
            << record.retries << '\t' << benchmark::name(record.kind) << '\n';
    submit << "sequence\tmessage_id\tstatus\tstart_ns\tend_ns\n";
    for (std::size_t i = 0; i < submissions.size(); ++i) {
        const auto& row = submissions[i];
        submit << i << '\t' << row.id << '\t' << row.status << '\t'
               << (row.start ? row.start - origin_ns : 0) << '\t' << (row.end ? row.end - origin_ns : 0) << '\n';
    }
    runtime << "sequence\thandler_invocation\tstart_ns\tend_ns\tstatus\toperation\n";
    for (const auto& row : runtimes) if (row.end <= cutoff_ns)
        runtime << row.sequence << '\t' << row.attempt << '\t' << row.start - origin_ns << '\t'
                << row.end - origin_ns << '\t' << row.status << '\t' << row.operation << '\n';
    const auto stats = service.feedback_stats();
    summary << "record_version\t1\ncutoff_ns\t" << cutoff_ns - origin_ns << "\nqueued_at_cutoff\t" << queued
        << "\nin_flight_at_cutoff\t" << in_flight << "\nobservation_drops\t" << observations->dropped()
        << "\npull_errors\t" << pull_errors << "\nfeedback_instance\t" << service.benchmark_instance()
        << "\nfeedback_drops\t" << std::accumulate(stats.dropped.begin(), stats.dropped.end(), uint64_t{0})
        << "\nfeedback_uncertain\t" << stats.uncertain_records << "\nfeedback_retention_records\t" << stats.retention_records
        << "\nfeedback_written\t" << stats.written << "\nfeedback_synced\t" << stats.synced
        << "\nfeedback_writer_failures\t" << stats.writer_failures << "\nfeedback_sync_failures\t" << stats.sync_failures
        << "\nfeedback_recovery_failures\t" << stats.recovery_failures << "\nfeedback_pending_records\t" << stats.pending_records
        << "\nfeedback_pending_bytes\t" << stats.pending_bytes
        << "\ncompiler\t" << __VERSION__ << "\ngrpc\t" << grpc::Version()
        << "\nprotobuf\t" << GOOGLE_PROTOBUF_VERSION << '\n';
    obs.flush(); submit.flush(); runtime.flush(); summary.flush();
    if (!obs || !submit || !runtime || !summary) throw std::runtime_error("result write failed");
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
