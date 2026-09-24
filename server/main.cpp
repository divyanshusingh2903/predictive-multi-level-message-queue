#include "harbinger_service.hpp"

#include <grpcpp/grpcpp.h>

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

// ── Graceful shutdown ─────────────────────────────────────────────────────────

static grpc::Server* g_server = nullptr;

static void signal_handler(int /*sig*/) {
    if (g_server) g_server->Shutdown();
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
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
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    auto server = builder.BuildAndStart();
    g_server = server.get();

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "[harbinger] Broker listening on " << addr << "\n";
    std::cout << "[harbinger] Send SIGINT or SIGTERM to shut down gracefully.\n";

    server->Wait();

    std::cout << "[harbinger] Server stopped.\n";
    return EXIT_SUCCESS;
}
