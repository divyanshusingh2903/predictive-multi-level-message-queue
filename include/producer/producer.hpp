#pragma once

#include <pmlmq.grpc.pb.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pmlmq {

/// gRPC client for producers.
///
/// A producer connects to a running PMLMQ server, receives a unique ID at
/// registration, and then sends raw payloads. It has no knowledge of queue
/// tiers, priorities, TTL, or retries — all routing context is assigned by
/// the server.
///
///     auto producer = Producer::connect("127.0.0.1:50051");
///     producer->send({0x01, 0x02}, {{"job_type", "resize"}});
class Producer {
public:
    /// Connect to a PMLMQ server, register, and obtain a producer ID.
    /// @param server_addr  gRPC target address, e.g. "127.0.0.1:50051".
    /// @throws std::runtime_error if the registration RPC fails.
    [[nodiscard]] static std::shared_ptr<Producer> connect(
        const std::string& server_addr);

    /// Send a message into the system.
    /// @param payload  Opaque byte payload.
    /// @param headers  Optional key-value metadata (e.g. job type, source service).
    ///                 Forwarded to consumers and used for ML feature extraction in Phase 2.
    /// @returns The message ID assigned by the server (useful for tracing).
    /// @throws std::runtime_error if the Submit RPC fails.
    std::string send(std::vector<uint8_t> payload,
                     std::unordered_map<std::string, std::string> headers = {});

    [[nodiscard]] const std::string& id() const noexcept { return id_; }

    [[nodiscard]] uint64_t messages_sent() const noexcept {
        return sent_.load(std::memory_order_relaxed);
    }

private:
    Producer(std::string id,
             std::unique_ptr<pmlmq_rpc::Broker::Stub> stub);

    std::string                              id_;
    std::unique_ptr<pmlmq_rpc::Broker::Stub> stub_;
    std::atomic<uint64_t>                    sent_{0};
};

} // namespace pmlmq
