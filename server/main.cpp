#include "harbinger_service.hpp"

#include <grpcpp/grpcpp.h>

#include <csignal>
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
              << "  --help                          Show this help and exit\n";
}
}

int main(int argc, char* argv[]) try {
    std::string addr = "0.0.0.0:50051";
    bool address_set = false;
    harbinger::HarbingerConfig config;
    config.aging = harbinger::AgingConfig{
        .threshold = std::chrono::milliseconds{5000},
        .interval = std::chrono::milliseconds{500},
    };
    for (int i = 1; i < argc; ++i) {
        const std::string_view option{argv[i]};
        if (option == "--help") {
            print_help();
            return EXIT_SUCCESS;
        }
        if (!option.starts_with("-")) {
            if (address_set) throw std::invalid_argument("Only one listen address is allowed");
            addr = option;
            address_set = true;
            continue;
        }
        std::chrono::milliseconds* duration = nullptr;
        std::size_t* size = nullptr;
        if (option == "--delivery-lease-ms") duration = &config.delivery_lease;
        else if (option == "--lease-sweep-interval-ms") duration = &config.lease_sweep_interval;
        else if (option == "--completion-retention-ms") duration = &config.completion_retention;
        else if (option == "--ttl-sweep-interval-ms") duration = &config.ttl_sweep_interval;
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
    std::cout << "[harbinger] Ready port=" << port << std::endl;
    std::cout << "[harbinger] Send SIGINT or SIGTERM to shut down gracefully.\n";

    int signal = 0;
    const int result = sigwait(&signals, &signal);
    server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds{5});
    server->Wait();

    std::cout << "[harbinger] Server stopped.\n";
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
} catch (const std::exception& error) {
    std::cerr << "[harbinger] " << error.what() << "\nUse --help for server options.\n";
    return EXIT_FAILURE;
}
