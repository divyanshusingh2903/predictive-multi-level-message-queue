#pragma once

#include <pmlmq.grpc.pb.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pmlmq {

/// gRPC producer client.
/// Connects to a broker, registers once, then sends payloads with optional
/// headers. Routing context (priority, TTL, retries) is assigned server-side.
class Producer {
public:
    /// Register with the broker and obtain a producer ID.
    /// @param server_addr gRPC target, e.g. "127.0.0.1:50051".
    /// @return Connected producer holding its server-assigned ID.
    /// @side_effects Opens a channel and performs a RegisterProducer RPC.
    /// @throws std::runtime_error if registration fails.
    [[nodiscard]] static std::shared_ptr<Producer> connect(
        const std::string& server_addr);

    /// Submit one message to the broker.
    /// @param payload Opaque bytes sent as the message body.
    /// @param headers Optional metadata forwarded to consumers and used
    ///   for future ML feature extraction.
    /// @return Message ID assigned by the server, useful for tracing.
    /// @side_effects Performs a Submit RPC and increments the sent counter.
    /// @throws std::runtime_error if the Submit RPC fails.
    std::string send(std::vector<uint8_t> payload,
                     std::unordered_map<std::string, std::string> headers = {});

    /// Server-assigned producer ID from registration.
    /// @return Opaque ID string (e.g. "producer-0").
    [[nodiscard]] const std::string& id() const noexcept { return id_; }

    /// Total successful sends through this client.
    /// @return Value of the sent counter.
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
