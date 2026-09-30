#pragma once

#include <harbinger.grpc.pb.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <vector>

namespace harbinger {

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
    /// Stop and join; must not be destroyed from its own handler.
    ~Consumer();
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

    /// Cancel polling and join; handler-thread calls only request stop.
    void stop();

    /// Check whether the polling thread is active.
    /// False after stop or a terminal RPC error.
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
    /// Total deliveries whose settlement was rejected as expired, stale, or conflicting.
    [[nodiscard]] uint64_t leases_lost() const noexcept {
        return leases_lost_.load(std::memory_order_relaxed);
    }
    /// Last terminal RPC error, or OK since the latest start.
    [[nodiscard]] grpc::Status last_rpc_status() const;

private:
    Consumer(std::string id,
             std::unique_ptr<harbinger_rpc::Broker::Stub> stub,
             Handler handler,
             std::chrono::milliseconds pull_timeout);

    void run();
    void fail(grpc::Status status);
    void settlement_failed(grpc::Status status);

    std::string                              id_;
    std::unique_ptr<harbinger_rpc::Broker::Stub> stub_;
    Handler                                  handler_;
    std::chrono::milliseconds                pull_timeout_;

    std::atomic<bool>     running_{false};
    std::thread           worker_;
    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_cv_;
    bool joining_{false};
    std::thread::id worker_id_;
    grpc::ClientContext* active_pull_{nullptr};
    grpc::Status last_status_;

    std::atomic<uint64_t> processed_{0};
    std::atomic<uint64_t> acked_{0};
    std::atomic<uint64_t> nacked_{0};
    std::atomic<uint64_t> rpc_failures_{0};
    std::atomic<uint64_t> leases_lost_{0};
};

} // namespace harbinger
