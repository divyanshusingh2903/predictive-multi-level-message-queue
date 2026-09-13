#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pmlmq {

/// Aging policy for starvation prevention.
/// Messages waiting longer than threshold are promoted one level per scan.
struct AgingConfig {
    /// Wait time before a message is promoted one level toward priority 0.
    std::chrono::milliseconds threshold{5000};
    /// Interval between background aging scans.
    std::chrono::milliseconds interval{500};
};

/// Single queue message.
/// Priority 0 is highest. Proxy sets id/arrival_time; queue sets enqueue_time.
struct Message {
    std::string id;
    std::vector<uint8_t> payload;
    uint8_t priority{1};
    uint8_t original_priority{1};
    std::chrono::steady_clock::time_point arrival_time{};
    std::chrono::steady_clock::time_point enqueue_time{};
    std::chrono::milliseconds ttl{0};
    uint32_t retry_count{0};
    uint32_t max_retries{3};
    std::unordered_map<std::string, std::string> headers;

    /// Check TTL expiry relative to arrival_time.
    /// @return True if ttl > 0 and now - arrival_time >= ttl, else false.
    /// @side_effects None; reads steady_clock.
    [[nodiscard]] bool is_expired() const noexcept {
        if (ttl.count() == 0) return false;
        return (std::chrono::steady_clock::now() - arrival_time) >= ttl;
    }
};

/// Reason a message was sent to the Dead Letter Queue.
enum class DLQReason : uint8_t {
    MAX_RETRIES_EXCEEDED,
    TTL_EXPIRED,
    PROCESSING_ERROR,
};

/// Failed message plus DLQ metadata.
struct DLQEntry {
    Message message;
    DLQReason reason;
    std::chrono::steady_clock::time_point dlq_time{};
    std::string details;
};

} // namespace pmlmq
