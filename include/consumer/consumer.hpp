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

/// Handler result: SUCCESS deletes the message; FAILURE retries or DLQs it.
enum class AckResult : uint8_t {
    SUCCESS,
    FAILURE,
};

/// Server-delivered message, decoupled from protobuf and queue internals.
struct ReceivedMessage {
    std::string                              id;
    std::vector<uint8_t>                     payload;
    std::unordered_map<std::string, std::string> headers;
};

/// Tier-blind gRPC consumer client.
/// The broker decides which message each Pull receives; the consumer never
/// sees queue levels. Processing time is measured and reported on every
/// Ack/Nack for the ML feedback loop.
class Consumer {
public:
    using Handler = std::function<AckResult(const ReceivedMessage&)>;

    /// Register with the broker and obtain a consumer ID.
    /// @param server_addr gRPC target, e.g. "127.0.0.1:50051".
    /// @param handler Callback invoked for each delivered message.
    /// @param pull_timeout Time each Pull waits server-side before returning
    ///   empty; shorter shuts down faster, longer saves round-trips under load.
    /// @return Connected consumer holding its server-assigned ID.
    /// @side_effects Opens a channel and performs a RegisterConsumer RPC.
    /// @throws std::runtime_error if registration fails.
    [[nodiscard]] static std::shared_ptr<Consumer> connect(
        const std::string& server_addr,
        Handler handler,
        std::chrono::milliseconds pull_timeout = std::chrono::milliseconds{1000});

    /// Start the background polling loop.
    /// @side_effects Sets running flag and spawns the worker thread.
    /// @throws std::runtime_error if already running.
    void start();

    /// Stop the polling loop and wait for it to exit.
    /// @side_effects Clears the running flag and joins the worker thread.
    void stop();

    /// Check whether the polling thread is active.
    /// @return True between start and stop.
    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    /// Server-assigned consumer ID from registration.
    /// @return Opaque ID string (e.g. "consumer-0").
    [[nodiscard]] const std::string& id() const noexcept { return id_; }

    /// Total messages handed to the handler.
    /// @return Value of the processed counter.
    [[nodiscard]] uint64_t messages_processed() const noexcept {
        return processed_.load(std::memory_order_relaxed);
    }
    /// Total messages acknowledged with SUCCESS.
    /// @return Value of the acked counter.
    [[nodiscard]] uint64_t messages_acked() const noexcept {
        return acked_.load(std::memory_order_relaxed);
    }
    /// Total messages reported with FAILURE.
    /// @return Value of the nacked counter.
    [[nodiscard]] uint64_t messages_nacked() const noexcept {
        return nacked_.load(std::memory_order_relaxed);
    }
    /// Total Ack/Nack RPCs that failed after local processing.
    [[nodiscard]] uint64_t rpc_failures() const noexcept {
        return rpc_failures_.load(std::memory_order_relaxed);
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
    std::atomic<uint64_t> rpc_failures_{0};
};

} // namespace pmlmq
