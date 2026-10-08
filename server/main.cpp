#include "config_file.hpp"
#include "harbinger_service.hpp"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <thread>
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <pthread.h>

namespace {
uint64_t parse_number(std::string_view option, std::string_view value) {
    uint64_t number = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size())
        throw std::invalid_argument(std::string{option} + " requires an unsigned decimal integer");
    return number;
}

void print_help() {
    const harbinger::HarbingerConfig defaults;
    std::cout << "Usage: harbinger_server [address] [options]\n"
              << "Address defaults to 0.0.0.0:50051. Durations are milliseconds.\n"
              << "  --delivery-lease-ms N            Default: " << defaults.delivery_lease.count() << '\n'
              << "  --lease-sweep-interval-ms N      Default: " << defaults.lease_sweep_interval.count() << '\n'
              << "  --completion-retention-ms N      Default: " << defaults.completion_retention.count() << '\n'
              << "  --completion-cache-max-entries N Default: " << defaults.completion_cache_max_entries << '\n'
              << "  --maintenance-batch-size N       Default: " << defaults.maintenance_batch_size << '\n'
              << "  --ttl-sweep-interval-ms N        Default: " << defaults.ttl_sweep_interval.count() << '\n'
              << "All values must be positive; TTL sweep interval alone accepts 0 (off).\n"
              << "  --config PATH                    Version-1 JSON configuration (see docs/configuration.md)\n"
              << "  --routing-mode static|shadow|predictive\n"
              << "                                   Override the file's routing mode; shadow/predictive need no file\n"
              << "  --stats-interval-ms N            Print a JSON stats line every N ms (0 = off, default)\n"
              << "  --level-weights W0,W1,...|off    Worker-time share per level (e.g. 8,3,1); aging then pauses\n"
              << "                                   while the level above is behind (default off: strict priority)\n"
              << "Precedence: built-in defaults, then --config, then the other options (in any order).\n"
              << "  --help                          Show this help and exit\n";
}
}

int main(int argc, char* argv[]) try {
    harbinger::ServerSettings settings;
    bool address_set = false;
    harbinger::HarbingerConfig config;
    config.aging = harbinger::AgingConfig{
        .threshold = std::chrono::milliseconds{5000},
        .interval = std::chrono::milliseconds{500},
    };
    // The config file is applied first wherever it appears, so explicit options always win.
    for (int i = 1; i < argc; ++i) {
        const std::string_view option{argv[i]};
        if (option == "--help") {
            print_help();
            return EXIT_SUCCESS;
        }
        if (option == "--config") {
            if (i + 1 == argc) throw std::invalid_argument("Missing value for --config");
            harbinger::apply_config_file(argv[i + 1], config, settings);
        }
    }
    for (int i = 1; i < argc; ++i) {
        const std::string_view option{argv[i]};
        if (!option.starts_with("-")) {
            if (address_set) throw std::invalid_argument("Only one listen address is allowed");
            settings.listen_address = option;
            address_set = true;
            continue;
        }
        if (option == "--config") { ++i; continue; }
        if (option == "--level-weights") {
            if (++i == argc) throw std::invalid_argument("Missing value for --level-weights");
            const std::string_view text{argv[i]};
            if (text == "off") { config.level_weights.reset(); continue; }
            std::vector<uint32_t> weights;
            std::size_t start = 0;
            while (start <= text.size()) {
                const auto end = std::min(text.find(',', start), text.size());
                const auto weight = parse_number(option, text.substr(start, end - start));
                if (weight < 1 || weight > 1000) throw std::invalid_argument("--level-weights entries must be in [1, 1000]");
                weights.push_back(static_cast<uint32_t>(weight));
                start = end + 1;
            }
            config.level_weights = std::move(weights);
            continue;
        }
        if (option == "--routing-mode") {
            if (++i == argc) throw std::invalid_argument("Missing value for --routing-mode");
            const std::string_view mode{argv[i]};
            if (mode == "static") config.predictive_routing.reset();
            else if (mode == "shadow" || mode == "predictive") {
                if (!config.predictive_routing) config.predictive_routing.emplace();
                config.predictive_routing->mode = mode == "shadow" ? harbinger::ml::RoutingMode::Shadow
                                                                   : harbinger::ml::RoutingMode::Predictive;
            } else throw std::invalid_argument("--routing-mode must be static, shadow or predictive");
            continue;
        }
        std::chrono::milliseconds* duration = nullptr;
        std::size_t* size = nullptr;
        if (option == "--delivery-lease-ms") duration = &config.delivery_lease;
        else if (option == "--lease-sweep-interval-ms") duration = &config.lease_sweep_interval;
        else if (option == "--completion-retention-ms") duration = &config.completion_retention;
        else if (option == "--ttl-sweep-interval-ms") duration = &config.ttl_sweep_interval;
        else if (option == "--stats-interval-ms") duration = &settings.stats_interval;
        else if (option == "--completion-cache-max-entries") size = &config.completion_cache_max_entries;
        else if (option == "--maintenance-batch-size") size = &config.maintenance_batch_size;
        else throw std::invalid_argument("Unknown option: " + std::string{option});
        if (++i == argc) throw std::invalid_argument("Missing value for " + std::string{option});
        const auto number = parse_number(option, argv[i]);
        if (duration) {
            if (number > static_cast<uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max()))
                throw std::invalid_argument("Duration out of range for " + std::string{option});
            *duration = std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(number)};
        } else {
            if (number > std::numeric_limits<std::size_t>::max())
                throw std::invalid_argument("Size out of range for " + std::string{option});
            *size = static_cast<std::size_t>(number);
        }
    }
    const std::string addr = settings.listen_address;
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::cerr << "Cannot block termination signals\n";
        return EXIT_FAILURE;
    }
    harbinger::HarbingerService service{config};

    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);

    auto server = builder.BuildAndStart();
    if (!server || port == 0) {
        std::cerr << "Cannot listen on " << addr << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "[harbinger] Recovery config: delivery_lease_ms=" << config.delivery_lease.count()
              << " lease_sweep_interval_ms=" << config.lease_sweep_interval.count()
              << " completion_retention_ms=" << config.completion_retention.count()
              << " completion_cache_max_entries=" << config.completion_cache_max_entries
              << " maintenance_batch_size=" << config.maintenance_batch_size
              << " ttl_sweep_interval_ms=" << config.ttl_sweep_interval.count() << '\n';
    std::cout << "[harbinger] Config: " << harbinger::describe_config(config, settings) << '\n';
    if (config.predictive_routing && config.predictive_routing->snapshot_path)
        std::cout << "[harbinger] Predictor snapshot: " << service.routing_stats().snapshot_status << '\n';
    std::cout << "[harbinger] Ready port=" << port << std::endl;
    std::mutex stats_mutex;
    std::condition_variable stats_cv;
    bool stopping = false;
    std::thread reporter;
    if (settings.stats_interval.count() > 0) {
        reporter = std::thread([&] {
            std::unique_lock lock{stats_mutex};
            while (!stats_cv.wait_for(lock, settings.stats_interval, [&] { return stopping; }))
                std::cout << "[harbinger] stats " << harbinger::stats_json(service) << std::endl;
        });
    }
    std::cout << "[harbinger] Send SIGINT or SIGTERM to shut down gracefully.\n";

    int signal = 0;
    const int result = sigwait(&signals, &signal);
    {
        std::lock_guard lock{stats_mutex};
        stopping = true;
    }
    stats_cv.notify_all();
    if (reporter.joinable()) reporter.join();
    server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds{5});
    server->Wait();

    std::cout << "[harbinger] Server stopped.\n";
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
} catch (const std::exception& error) {
    std::cerr << "[harbinger] " << error.what() << "\nUse --help for server options.\n";
    return EXIT_FAILURE;
}
