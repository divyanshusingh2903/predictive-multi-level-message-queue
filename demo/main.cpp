#include "consumer/consumer.hpp"
#include "pmlmq_service.hpp"
#include "producer/producer.hpp"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <iostream>
#include <thread>

using namespace pmlmq;
using namespace std::chrono_literals;

int main() {
    std::cout << "=== PMLMQ Phase 1 Demo ===\n\n";

    // ── Start the PMLMQ broker ────────────────────────────────────────────────
    // In production this runs as a separate process (./pmlmq_server).
    // Here we embed it in the demo binary for convenience.

    PMLMQService service{ PMLMQConfig{
        .num_levels          = 3,
        .aging               = AgingConfig{
            .threshold = 4000ms,
            .interval  = 500ms,
        },
        .default_max_retries = 3,
        .default_ttl         = 0ms,     // no expiry
        .default_priority    = 1,       // MEDIUM (Phase 2 will use ML)
        .max_pull_wait       = 1000ms,
    }};

    const std::string addr = "127.0.0.1:50099";
    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    std::cout << "Broker listening on " << addr << "\n\n";

    // ── Producers connect ─────────────────────────────────────────────────────
    // Producers have no knowledge of queues, priorities, or routing.
    // They just connect and send raw payloads with optional metadata.

    auto producer_a = Producer::connect(addr);
    auto producer_b = Producer::connect(addr);
    std::cout << "Producer A registered: " << producer_a->id() << "\n";
    std::cout << "Producer B registered: " << producer_b->id() << "\n\n";

    // ── Consumers connect ─────────────────────────────────────────────────────
    // Consumers are tier-blind. PMLMQ decides which message each pull receives.
    // Actual processing time is reported back on every Ack/Nack.

    int fail_budget = 3; // first 3 messages fail to demonstrate retry

    auto consumer_1 = Consumer::connect(addr,
        [&](const ReceivedMessage& msg) -> AckResult {
            std::cout << "[C1] processing id=" << msg.id;
            if (msg.headers.count("job_type")) {
                std::cout << " job=" << msg.headers.at("job_type");
            }
            std::this_thread::sleep_for(10ms); // simulate work
            if (fail_budget-- > 0) {
                std::cout << " → NACK\n";
                return AckResult::FAILURE;
            }
            std::cout << " → ACK\n";
            return AckResult::SUCCESS;
        }, 800ms);

    auto consumer_2 = Consumer::connect(addr,
        [](const ReceivedMessage& msg) -> AckResult {
            std::cout << "[C2] processing id=" << msg.id;
            std::this_thread::sleep_for(10ms);
            std::cout << " → ACK\n";
            return AckResult::SUCCESS;
        }, 800ms);

    std::cout << "Consumer 1 registered: " << consumer_1->id() << "\n";
    std::cout << "Consumer 2 registered: " << consumer_2->id() << "\n\n";

    consumer_1->start();
    consumer_2->start();

    // ── Send messages ─────────────────────────────────────────────────────────
    std::cout << "Sending messages...\n";

    for (int i = 0; i < 6; ++i) {
        const std::string id_a = producer_a->send(
            {static_cast<uint8_t>(i)},
            {{"job_type", "resize"}, {"source", "service-a"}});
        std::cout << "  A sent: " << id_a << "\n";
    }
    for (int i = 0; i < 4; ++i) {
        const std::string id_b = producer_b->send(
            {static_cast<uint8_t>(0x80 + i)},
            {{"job_type", "transcode"}, {"source", "service-b"}},
            // Per-message TTL example: generous enough to never fire here;
            // the broker DLQs as TTL_EXPIRED only when the deadline passes.
            60s);
        std::cout << "  B sent: " << id_b << "\n";
    }
    std::cout << "\n";

    // ── Let the system process for 5 seconds ──────────────────────────────────
    std::this_thread::sleep_for(5s);

    consumer_1->stop();
    consumer_2->stop();

    // ── Stats ─────────────────────────────────────────────────────────────────
    std::cout << "\n=== Stats ===\n";
    std::cout << "Producer A sent : " << producer_a->messages_sent() << "\n";
    std::cout << "Producer B sent : " << producer_b->messages_sent() << "\n";
    std::cout << "\nConsumer 1:\n";
    std::cout << "  processed : " << consumer_1->messages_processed() << "\n";
    std::cout << "  acked     : " << consumer_1->messages_acked()     << "\n";
    std::cout << "  nacked    : " << consumer_1->messages_nacked()    << "\n";
    std::cout << "\nConsumer 2:\n";
    std::cout << "  processed : " << consumer_2->messages_processed() << "\n";
    std::cout << "  acked     : " << consumer_2->messages_acked()     << "\n";
    std::cout << "  nacked    : " << consumer_2->messages_nacked()    << "\n";
    std::cout << "\nBroker queue remaining : " << service.queue_size() << "\n";
    std::cout << "Broker DLQ size        : " << service.dlq_size()    << "\n";
    std::cout << "Broker in-flight       : " << service.in_flight_count() << "\n";

    server->Shutdown();
    std::cout << "\nBroker stopped.\n";
    return 0;
}
