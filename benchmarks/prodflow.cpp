// Production-like job-queue benchmark: an e-commerce backend whose services submit background jobs through the
// real Producer client and whose workers run real CPU work (zlib, image downscaling, sorting) plus simulated
// network calls, through the real Consumer client, against an embedded broker. Described in docs/v2-preregistration.md
// (workload) and docs/v3-sizebins-preregistration.md (size_hint header, predictive_sized arm).
#include "harbinger_service.hpp"
#include "benchmark/hooks.hpp"
#include "consumer/consumer.hpp"
#include "producer/producer.hpp"
#include "config_file.hpp"

#include <grpcpp/grpcpp.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
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

// ── Real work kernels ───────────────────────────────────────────────────────

uint64_t mix(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return x;
}

/// Render an HTML e-mail body of about `kb` KiB and checksum it.
uint64_t render_template(uint64_t seed, int kb) {
    std::string body;
    body.reserve(static_cast<std::size_t>(kb) * 1024 + 256);
    char line[160];
    for (int i = 0; body.size() < static_cast<std::size_t>(kb) * 1024; ++i) {
        const int n = std::snprintf(line, sizeof line, "<tr><td>item-%llu</td><td>%d</td><td>$%.2f</td></tr>\n",
                                    static_cast<unsigned long long>(mix(seed + i) % 100000), i % 7 + 1,
                                    static_cast<double>(mix(seed ^ i) % 100000) / 100.0);
        body.append(line, static_cast<std::size_t>(n));
    }
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : body) { hash ^= c; hash *= 1099511628211ULL; }
    return hash;
}

/// Build an invoice document with `items` line items and deflate it at the highest level (PDF streams are deflated).
uint64_t build_invoice(uint64_t seed, int items) {
    std::string doc;
    char line[256];
    for (int i = 0; i < items; ++i) {
        for (int k = 0; k < 12; ++k) {
            const int n = std::snprintf(line, sizeof line,
                "BT /F1 9 Tf 72 %d Td (SKU-%06llu  qty %d  unit %.2f  tax %.2f  line %.2f) Tj ET\n",
                700 - (i * 12 + k) % 600, static_cast<unsigned long long>(mix(seed + i * 31 + k) % 1000000),
                k + 1, static_cast<double>(mix(seed + i) % 9999) / 100.0, 0.0825 * k, 12.5 * k);
            doc.append(line, static_cast<std::size_t>(n));
        }
    }
    uLongf size = compressBound(static_cast<uLong>(doc.size()));
    std::vector<Bytef> out(size);
    compress2(out.data(), &size, reinterpret_cast<const Bytef*>(doc.data()), static_cast<uLong>(doc.size()), 9);
    return size;
}

/// Decode-free image downscale: fill an RGB buffer of `pixels` pixels and box-filter it to 1/4 size with gamma LUT.
uint64_t resize_image(uint64_t seed, int64_t pixels) {
    const int64_t width = std::max<int64_t>(64, static_cast<int64_t>(std::sqrt(static_cast<double>(pixels) * 4 / 3)));
    const int64_t height = std::max<int64_t>(48, pixels / width);
    std::vector<uint8_t> image(static_cast<std::size_t>(width * height * 3));
    uint64_t state = seed | 1;
    for (auto& p : image) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; p = static_cast<uint8_t>(state); }
    static const auto lut = [] {
        std::array<float, 256> table{};
        for (int i = 0; i < 256; ++i) table[i] = std::pow(i / 255.0f, 2.2f);
        return table;
    }();
    const int64_t ow = width / 4, oh = height / 4;
    std::vector<uint8_t> out(static_cast<std::size_t>(ow * oh * 3));
    for (int64_t y = 0; y < oh; ++y)
        for (int64_t x = 0; x < ow; ++x)
            for (int c = 0; c < 3; ++c) {
                float sum = 0;
                for (int dy = 0; dy < 4; ++dy)
                    for (int dx = 0; dx < 4; ++dx)
                        sum += lut[image[static_cast<std::size_t>(((y * 4 + dy) * width + x * 4 + dx) * 3 + c)]];
                out[static_cast<std::size_t>((y * ow + x) * 3 + c)] = static_cast<uint8_t>(std::pow(sum / 16, 1 / 2.2f) * 255);
            }
    return out[out.size() / 2];
}

/// Tokenize a product description and update an inverted index.
uint64_t reindex(uint64_t seed, int words) {
    std::map<uint64_t, int> index;
    for (int i = 0; i < words; ++i) index[mix(seed + i) % 4096] += 1;
    return index.size();
}

/// Aggregate an order-history export: generate rows, sort by customer, group and sum. `chunks` x 250k rows.
uint64_t export_report(uint64_t seed, int chunks) {
    struct Row { uint32_t customer; uint32_t day; double amount; };
    uint64_t total = 0;
    std::vector<Row> rows(250000);
    for (int c = 0; c < chunks; ++c) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto r = mix(seed + c * 1000003ULL + i);
            rows[i] = {static_cast<uint32_t>(r % 50000), static_cast<uint32_t>((r >> 20) % 365), static_cast<double>(r % 10000) / 100};
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return a.customer != b.customer ? a.customer < b.customer : a.day < b.day; });
        double sum = 0;
        uint32_t groups = 0, last = UINT32_MAX;
        for (const auto& row : rows) { sum += row.amount; if (row.customer != last) { ++groups; last = row.customer; } }
        total += groups + static_cast<uint64_t>(sum);
    }
    return total;
}

void sleep_ms(double ms) { std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(ms * 1000))); }

// ── Workload ────────────────────────────────────────────────────────────────

struct Job {
    uint64_t seq{0};
    int64_t at_us{0};
    std::string service, type;
    int phase{0};
    /// Work parameters: units of the job's CPU kernel and simulated network latency per attempt.
    int64_t units{0};
    double io_ms{0};
    /// Attempts that time out (webhooks) before one succeeds.
    int failures{0};
    double planned_ms{0};
    std::string payload;
};

struct Calibration { double render_kb, invoice_item, resize_mp, reindex_word, export_chunk; };

/// Milliseconds per kernel unit on this machine; median of a few runs on an idle worker.
Calibration calibrate() {
    const auto time = [](auto&& work) {
        std::vector<double> runs;
        for (int i = 0; i < 5; ++i) {
            const auto start = Clock::now();
            volatile uint64_t sink = work(static_cast<uint64_t>(i + 1));
            (void)sink;
            runs.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        }
        std::sort(runs.begin(), runs.end());
        return runs[2];
    };
    Calibration c{};
    c.render_kb = time([](uint64_t s) { return render_template(s, 64); }) / 64;
    c.invoice_item = time([](uint64_t s) { return build_invoice(s, 50); }) / 50;
    c.resize_mp = time([](uint64_t s) { return resize_image(s, 2'000'000); }) / 2;
    c.reindex_word = time([](uint64_t s) { return reindex(s, 20000); }) / 20000;
    c.export_chunk = time([](uint64_t s) { return export_report(s, 2); }) / 2;
    return c;
}

/// Phase of the 5-minute timeline; rates and mixes are multiplied per phase. Durations scale with `seconds`.
int phase_at(double t, double seconds) {
    const double f = t / seconds;
    if (f < 0.20) return 0;   // steady
    if (f < 0.30) return 1;   // ramp
    if (f < 0.50) return 2;   // flash sale (overload)
    if (f < 0.70) return 3;   // recovery
    return 4;                 // shifted regime: high-res uploads, degraded partner latency
}

/// Deterministic open-loop trace for one seed: Poisson arrivals per job type with phase-dependent rates.
std::vector<Job> generate(uint64_t seed, double seconds, double rate_scale, const Calibration& cal) {
    struct Type { const char* service; const char* type; double rate; std::array<double, 5> phase; };
    // Base rates (jobs/s) and per-phase multipliers. The flash sale triples checkout traffic.
    const std::vector<Type> types{
        {"checkout", "send_order_email", 6.0, {1, 1.8, 3.0, 1.4, 1.0}},
        {"checkout", "generate_invoice", 6.0, {1, 1.8, 3.0, 1.4, 1.0}},
        {"checkout", "reserve_inventory", 8.0, {1, 1.8, 3.0, 1.4, 1.0}},
        {"catalog", "resize_image", 4.0, {1, 1, 1.2, 1, 1}},
        {"catalog", "reindex_product", 6.0, {1, 1, 1.0, 1, 1}},
        {"notifications", "push_notification", 10.0, {1, 1.5, 2.5, 1.3, 1.0}},
        {"notifications", "send_sms", 2.0, {1, 1.5, 2.5, 1.3, 1.0}},
        {"partners", "deliver_webhook", 5.0, {1, 1.5, 2.0, 1.2, 1.0}},
        {"analytics", "export_report", 0.1, {1, 1, 1, 1, 1}},
    };
    std::mt19937_64 rng(seed);
    std::vector<Job> jobs;
    for (const auto& t : types) {
        double clock = 0;
        std::uniform_real_distribution<double> unit(0, 1);
        while (true) {
            // Thinning: draw at the maximum rate, accept with the current phase's share.
            const double max_rate = t.rate * rate_scale * *std::max_element(t.phase.begin(), t.phase.end());
            clock += -std::log(1 - unit(rng)) / max_rate;
            if (clock >= seconds) break;
            const int phase = phase_at(clock, seconds);
            if (unit(rng) * max_rate > t.rate * rate_scale * t.phase[phase]) continue;
            Job job;
            job.at_us = static_cast<int64_t>(clock * 1e6);
            job.service = t.service;
            job.type = t.type;
            job.phase = phase;
            const auto lognormal = [&](double median, double sigma) {
                return median * std::exp(sigma * std::normal_distribution<double>(0, 1)(rng));
            };
            const std::string type = t.type;
            std::ostringstream body;
            if (type == "send_order_email") {
                job.units = 24;  // KiB of rendered HTML
                job.io_ms = lognormal(35, 0.5);  // SMTP relay
                body << "{\"order\":" << mix(seed + jobs.size()) % 1000000 << ",\"template\":\"order_confirmation\"}";
            } else if (type == "generate_invoice") {
                job.units = std::clamp<int64_t>(static_cast<int64_t>(lognormal(12, 0.9)), 1, 400);  // line items
                job.io_ms = lognormal(4, 0.3);  // upload to object storage
                body << "{\"order\":" << mix(seed + jobs.size()) % 1000000 << ",\"line_items\":" << job.units << "}";
            } else if (type == "reserve_inventory") {
                job.io_ms = lognormal(6, 0.4);  // database round trips
                body << "{\"sku_count\":" << 1 + mix(seed + jobs.size()) % 5 << "}";
            } else if (type == "resize_image") {
                // Mostly phone photos resized for listings; some uploads are high resolution. After the shift the
                // catalog team starts uploading high-resolution studio photos.
                const bool hires = unit(rng) < (phase == 4 ? 0.55 : 0.08);
                job.units = static_cast<int64_t>(hires ? lognormal(9e6, 0.3) : lognormal(0.8e6, 0.4));  // pixels
                job.io_ms = lognormal(8, 0.4);  // fetch original + store thumbnail
                body << "{\"width\":" << static_cast<int64_t>(std::sqrt(job.units * 4.0 / 3)) << ",\"pixels\":" << job.units << "}";
            } else if (type == "reindex_product") {
                job.units = 3000 + static_cast<int64_t>(mix(seed + jobs.size()) % 3000);  // description words
                job.io_ms = lognormal(2, 0.3);
                body << "{\"product\":" << mix(seed ^ jobs.size()) % 100000 << "}";
            } else if (type == "push_notification") {
                job.io_ms = lognormal(9, 0.35);
                body << "{\"device\":\"" << std::hex << mix(seed + jobs.size()) << "\"}";
            } else if (type == "send_sms") {
                job.io_ms = lognormal(80, 0.4);
                body << "{\"to\":\"+1555" << mix(seed + jobs.size()) % 10000000 << "\"}";
            } else if (type == "deliver_webhook") {
                // Partner endpoints: mostly fast, a slow minority, rare timeouts that fail and are retried. After the
                // shift one partner degrades.
                const double slow_share = phase == 4 ? 0.35 : 0.12;
                const double r = unit(rng);
                job.io_ms = r < 0.02 ? 1500 : r < 0.02 + slow_share ? lognormal(300, 0.3) : lognormal(40, 0.4);
                job.failures = r < 0.02 ? 1 : 0;  // the timed-out attempt fails; the retry succeeds
                body << "{\"partner\":" << mix(seed + jobs.size()) % 40 << ",\"event\":\"order.updated\"}";
            } else if (type == "export_report") {
                job.units = 40 + static_cast<int64_t>(mix(seed + jobs.size()) % 80);  // 250k-row chunks
                body << "{\"report\":\"orders_by_customer\",\"days\":365}";
            }
            job.payload = body.str();
            const double cpu = type == "send_order_email" ? job.units * cal.render_kb
                : type == "generate_invoice" ? job.units * cal.invoice_item
                : type == "resize_image" ? static_cast<double>(job.units) / 1e6 * cal.resize_mp
                : type == "reindex_product" ? static_cast<double>(job.units) * cal.reindex_word
                : type == "export_report" ? static_cast<double>(job.units) * cal.export_chunk : 0.0;
            // Planned cost of the successful attempt: what an oracle would know in advance.
            job.planned_ms = cpu + (job.failures ? lognormal(40, 0.4) : job.io_ms);
            jobs.push_back(std::move(job));
        }
    }
    std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.at_us < b.at_us; });
    for (std::size_t i = 0; i < jobs.size(); ++i) jobs[i].seq = i;
    return jobs;
}

/// Execute one attempt of a job; returns false for an attempt that times out.
bool execute(const Job& job, int attempt) {
    const auto& t = job.type;
    if (t == "deliver_webhook" && attempt <= job.failures) { sleep_ms(job.io_ms); return false; }
    const double io = t == "deliver_webhook" && job.failures ? job.planned_ms : job.io_ms;
    volatile uint64_t sink = 0;
    if (t == "send_order_email") sink = render_template(job.seq, static_cast<int>(job.units));
    else if (t == "generate_invoice") sink = build_invoice(job.seq, static_cast<int>(job.units));
    else if (t == "resize_image") sink = resize_image(job.seq, job.units);
    else if (t == "reindex_product") sink = reindex(job.seq, static_cast<int>(job.units));
    else if (t == "export_report") sink = export_report(job.seq, static_cast<int>(job.units));
    (void)sink;
    sleep_ms(io);
    return true;
}

// ── Arms ────────────────────────────────────────────────────────────────────

/// Size a producer knows at submission for job types whose cost follows their input; every arm sends it, only
/// predictive_sized routes on it.
constexpr const char* kSizeHintHeader = "size_hint";
bool has_size_hint(const std::string& type) {
    return type == "resize_image" || type == "generate_invoice" || type == "export_report";
}

const std::map<std::string, uint8_t> kTunedTiers{
    // An operator's informed static map: fast I/O jobs first, CPU-heavy and batch work last.
    {"reserve_inventory", 0}, {"push_notification", 0}, {"reindex_product", 0},
    {"send_order_email", 1}, {"generate_invoice", 1}, {"send_sms", 1}, {"deliver_webhook", 1},
    {"resize_image", 2}, {"export_report", 2}};
const std::map<std::string, uint8_t> kMisconfiguredTiers{
    // Priority by business importance rather than cost: checkout, partners and the executive report first.
    {"send_order_email", 0}, {"generate_invoice", 0}, {"reserve_inventory", 0}, {"deliver_webhook", 0},
    {"export_report", 0}, {"push_notification", 1}, {"send_sms", 1}, {"resize_image", 1}, {"reindex_product", 2}};

struct Record {
    std::atomic<int64_t> submit_ns{0}, first_start_ns{0}, start_ns{0}, end_ns{0};
    std::atomic<int> attempts{0};
    std::atomic<double> handler_ms{0};
    std::atomic<bool> done{false};
};

} // namespace

int main(int argc, char** argv) try {
    std::map<std::string, std::string> args;
    for (int i = 1; i + 1 < argc; i += 2) args[argv[i]] = argv[i + 1];
    if (argc % 2 == 0 || !args.contains("--arm") || !args.contains("--seed") || !args.contains("--out"))
        throw std::invalid_argument("usage: harbinger_prodflow --arm fifo|static_tuned|static_misconfigured|shadow|"
            "predictive|predictive_p75|predictive_producer|predictive_sized|oracle --seed N --out NEW_DIR [--seconds 300] [--workers 4] [--rate-scale 1] [--drain-s 180]");
    const std::string arm = args["--arm"];
    const uint64_t seed = std::stoull(args["--seed"]);
    const double seconds = args.contains("--seconds") ? std::stod(args["--seconds"]) : 300;
    const int workers = args.contains("--workers") ? std::stoi(args["--workers"]) : 4;
    const double rate_scale = args.contains("--rate-scale") ? std::stod(args["--rate-scale"]) : 1.0;
    const double drain_s = args.contains("--drain-s") ? std::stod(args["--drain-s"]) : 180;
    const std::filesystem::path out = args["--out"];
    if (std::filesystem::exists(out)) throw std::invalid_argument("refusing to overwrite " + out.string());
    std::filesystem::create_directories(out);

    const auto cal = calibrate();
    auto jobs = generate(seed, seconds, rate_scale, cal);
    // Oracle tiers: terciles of the planned cost over the whole trace (same quantile rule as the predictor).
    std::vector<double> planned;
    for (const auto& j : jobs) planned.push_back(j.planned_ms);
    std::sort(planned.begin(), planned.end());
    const std::vector<double> oracle_bounds{planned[planned.size() / 3], planned[planned.size() * 2 / 3]};

    HarbingerConfig config;
    config.aging = AgingConfig{std::chrono::milliseconds{5000}, std::chrono::milliseconds{500}};  // server default
    config.delivery_lease = std::chrono::milliseconds{30000};
    benchmark::Options options;
    options.observations = std::make_shared<benchmark::Observations>(1);
    options.static_header = "job_type";
    if (arm == "fifo") options.policy = benchmark::Policy::Fifo;
    else if (arm == "static_tuned") { options.policy = benchmark::Policy::Static; options.static_tiers = kTunedTiers; }
    else if (arm == "static_misconfigured") { options.policy = benchmark::Policy::Static; options.static_tiers = kMisconfiguredTiers; }
    else if (arm == "oracle") options.policy = benchmark::Policy::Oracle;
    else if (arm == "shadow" || arm == "predictive" || arm == "predictive_p75" || arm == "predictive_producer" ||
             arm == "predictive_sized") {
        ml::PredictiveRoutingConfig routing;
        routing.mode = arm == "shadow" ? ml::RoutingMode::Shadow : ml::RoutingMode::Predictive;
        if (arm == "predictive_p75") routing.predictor.summary = ml::DurationSummary::P75;
        // Zero-configuration control: producers do not label jobs, so each service is one key.
        if (arm == "predictive_producer") routing.key.job_header.clear();
        // Size-binned keys (#35, #39): the producer's size hint (pixels, line items, chunks) adds a log2 bin.
        if (arm == "predictive_sized") {
            routing.key.size_source = ml::PredictorKeyPolicy::SizeSource::Header;
            routing.key.size_header = kSizeHintHeader;
        }
        config.predictive_routing = routing;  // defaults: producer-scoped job_type key
    } else throw std::invalid_argument("unknown arm " + arm);
    config.benchmark_options = options;

    HarbingerService service{config};
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    if (!server || !port) throw std::runtime_error("cannot start broker");
    const std::string address = "127.0.0.1:" + std::to_string(port);

    std::vector<Record> records(jobs.size());
    std::atomic<std::size_t> completed{0};
    std::vector<std::shared_ptr<Consumer>> consumers;
    for (int w = 0; w < workers; ++w) {
        consumers.push_back(Consumer::connect(address, [&](const ReceivedMessage& message) {
            const auto found = message.headers.find("bench_seq");
            const auto seq = std::stoull(found->second);
            auto& record = records[seq];
            const int attempt = record.attempts.fetch_add(1) + 1;
            const auto start = now_ns();
            if (attempt == 1) record.first_start_ns = start;
            record.start_ns = start;
            const bool ok = execute(jobs[seq], attempt);
            const auto end = now_ns();
            record.handler_ms = static_cast<double>(end - start) / 1e6;
            if (ok) {
                record.end_ns = end;
                if (!record.done.exchange(true)) completed.fetch_add(1);
                return AckResult::SUCCESS;
            }
            return AckResult::FAILURE;
        }, std::chrono::milliseconds{200}));
    }
    for (auto& c : consumers) c->start();

    // One producer connection per service, as separate deployments would have.
    std::map<std::string, std::vector<std::size_t>> by_service;
    for (const auto& j : jobs) by_service[j.service].push_back(j.seq);
    const auto origin = Clock::now() + std::chrono::milliseconds{500};
    std::vector<std::thread> producers;
    std::atomic<uint64_t> submit_errors{0};
    for (const auto& [name, sequence] : by_service) {
        producers.emplace_back([&, name, sequence] {
            auto producer = Producer::connect(address);
            for (const auto seq : sequence) {
                const auto& job = jobs[seq];
                std::this_thread::sleep_until(origin + std::chrono::microseconds(job.at_us));
                std::unordered_map<std::string, std::string> headers{
                    {"job_type", job.type}, {"bench_seq", std::to_string(seq)}};
                if (has_size_hint(job.type)) headers[kSizeHintHeader] = std::to_string(job.units);
                if (arm == "oracle")
                    headers[benchmark::kOracleTierHeader] = std::to_string(
                        std::upper_bound(oracle_bounds.begin(), oracle_bounds.end(), job.planned_ms) - oracle_bounds.begin());
                records[seq].submit_ns = now_ns();
                try {
                    producer->send(std::vector<uint8_t>(job.payload.begin(), job.payload.end()), std::move(headers));
                } catch (...) {
                    submit_errors.fetch_add(1);
                }
            }
        });
    }
    for (auto& p : producers) p.join();
    const auto arrivals_done = Clock::now();
    while (completed.load() + service.dlq_size() < jobs.size() &&
           Clock::now() - arrivals_done < std::chrono::duration<double>(drain_s))
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    const auto stats = stats_json(service);
    for (auto& c : consumers) c->stop();
    server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds{2});

    std::ofstream tsv(out / "messages.tsv");
    tsv << "seq\tservice\tjob_type\tphase\tplanned_ms\tsubmit_ns\tfirst_start_ns\tstart_ns\tend_ns\tattempts\thandler_ms\tdone\n";
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const auto& j = jobs[i];
        const auto& r = records[i];
        tsv << i << '\t' << j.service << '\t' << j.type << '\t' << j.phase << '\t' << j.planned_ms << '\t'
            << r.submit_ns.load() << '\t' << r.first_start_ns.load() << '\t' << r.start_ns.load() << '\t'
            << r.end_ns.load() << '\t' << r.attempts.load() << '\t' << r.handler_ms.load() << '\t'
            << (r.done.load() ? 1 : 0) << '\n';
    }
    uint64_t lost = 0;
    for (auto& c : consumers) lost += c->leases_lost();
    std::ofstream run(out / "run.json");
    run << "{\"benchmark\":\"prodflow-v1\",\"arm\":\"" << arm << "\",\"seed\":" << seed << ",\"seconds\":" << seconds
        << ",\"workers\":" << workers << ",\"rate_scale\":" << rate_scale
        << ",\"size_hint_header\":\"" << kSizeHintHeader << "\"" << ",\"messages\":" << jobs.size()
        << ",\"completed\":" << completed.load() << ",\"dlq\":" << service.dlq_size()
        << ",\"submit_errors\":" << submit_errors.load() << ",\"leases_lost\":" << lost
        << ",\"oracle_boundaries_ms\":[" << oracle_bounds[0] << "," << oracle_bounds[1] << "]"
        << ",\"calibration_ms_per_unit\":{\"render_kb\":" << cal.render_kb << ",\"invoice_item\":" << cal.invoice_item
        << ",\"resize_mp\":" << cal.resize_mp << ",\"reindex_word\":" << cal.reindex_word
        << ",\"export_chunk\":" << cal.export_chunk << "},\"broker\":" << stats << "}\n";
    std::cout << "[prodflow] arm=" << arm << " seed=" << seed << " messages=" << jobs.size()
              << " completed=" << completed.load() << " dlq=" << service.dlq_size() << '\n';
    return completed.load() == jobs.size() ? 0 : 3;
} catch (const std::exception& error) {
    std::cerr << "[prodflow] " << error.what() << '\n';
    return 2;
}
