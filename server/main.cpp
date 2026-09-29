#include "harbinger_service.hpp"

#include <grpcpp/grpcpp.h>

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <pthread.h>

// ── Graceful shutdown ─────────────────────────────────────────────────────────

// ── Entry point ───────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::cerr << "Cannot block termination signals\n";
        return EXIT_FAILURE;
    }
    const std::string addr =
        (argc > 1) ? argv[1] : "0.0.0.0:50051";

    harbinger::HarbingerService service{ harbinger::HarbingerConfig{
        .num_levels          = 3,
        .aging               = harbinger::AgingConfig{
            .threshold = std::chrono::milliseconds{5000},
            .interval  = std::chrono::milliseconds{500},
        },
        .default_max_retries = 3,
        .default_ttl         = std::chrono::milliseconds{0},
        .default_priority    = 1, // MEDIUM — Phase 2 will override with ML
        .max_pull_wait       = std::chrono::milliseconds{5000},
    }};

    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);

    auto server = builder.BuildAndStart();
    if (!server || port == 0) {
        std::cerr << "Cannot listen on " << addr << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "[harbinger] Ready port=" << port << std::endl;
    std::cout << "[harbinger] Send SIGINT or SIGTERM to shut down gracefully.\n";

    int signal = 0;
    const int result = sigwait(&signals, &signal);
    server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds{5});
    server->Wait();

    std::cout << "[harbinger] Server stopped.\n";
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
