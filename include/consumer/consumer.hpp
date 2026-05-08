#pragma once

#include <pmlmq.grpc.pb.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pmlmq {

/// Result returned by the consumer's message handler.
enum class AckResult : uint8_t {
    SUCCESS, ///< Processing succeeded — server deletes the message.
    FAILURE, ///< Processing failed — server retries or DLQs the message.
};

/// Message received from the server by a consumer.
/// Intentionally decoupled from protobuf types and internal queue fields.
struct ReceivedMessage {
    std::string                              id;
    std::vector<uint8_t>                     payload;
    std::unordered_map<std::string, std::string> headers;
};

/// gRPC client for consumers.
///
/// A consumer connects to a running PMLMQ server, receives a unique ID at
/// registration, and then polls for messages via Pull(). The server decides
/// which message to dispatch — consumers are completely tier-blind.
///
/// Actual processing time is measured and reported back on every Ack/Nack,
/// providing the data stream for the Phase 2 ML feedback loop.
///
///     auto consumer = Consumer::connect("127.0.0.1:50051", [](const ReceivedMessage& msg) {
///         // process msg.payload...
///         return AckResult::SUCCESS;
///     });
///     consumer->start();
///     // ...
///     consumer->stop();
class Consumer {
public:
    using Handler = std::function<AckResult(const ReceivedMessage&)>;

    /// Connect to a PMLMQ server, register, and obtain a consumer ID.
    /// @param server_addr   gRPC target address, e.g. "127.0.0.1:50051".
    /// @param handler       Callback invoked for each delivered message.
    /// @param pull_timeout  How long each Pull() waits server-side before
    ///                      returning an empty response. Shorter = faster
    ///                      shutdown; longer = fewer round-trips under load.
    /// @throws std::runtime_error if the registration RPC fails.
    [[nodiscard]] static std::shared_ptr<Consumer> connect(
        const std::string& server_addr,
        Handler handler,
        std::chrono::milliseconds pull_timeout = std::chrono::milliseconds{1000});

    /// Start the background polling thread.
    /// @throws std::runtime_error if already running.
    void start();

    /// Signal the polling thread to stop and block until it exits.
    void stop();

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] const std::string& id() const noexcept { return id_; }

    [[nodiscard]] uint64_t messages_processed() const noexcept {
        return processed_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t messages_acked() const noexcept {
        return acked_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t messages_nacked() const noexcept {
        return nacked_.load(std::memory_order_relaxed);
    }

private:
    Consumer(std::string id,
             std::unique_ptr<pmlmq_rpc::Broker::Stub> stub,
             Handler handler,
             std::chrono::milliseconds pull_timeout);

    void run();

    std::string                              id_;
    std::unique_ptr<pmlmq_rpc::Broker::Stub> stub_;
    Handler                                  handler_;
    std::chrono::milliseconds                pull_timeout_;

    std::atomic<bool>     running_{false};
    std::thread           worker_;

    std::atomic<uint64_t> processed_{0};
    std::atomic<uint64_t> acked_{0};
    std::atomic<uint64_t> nacked_{0};
};

} // namespace pmlmq
