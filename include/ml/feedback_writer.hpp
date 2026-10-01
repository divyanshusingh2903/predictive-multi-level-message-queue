#pragma once

#include "ml/feedback_event.hpp"

#include <array>
#include <filesystem>
#include <memory>

namespace harbinger::ml {

/// Opt-in bounded JSONL storage in an existing dedicated directory.
struct FeedbackConfig {
    std::filesystem::path path;
    std::size_t buffer_records{4096};
    std::size_t buffer_bytes{32 * 1024 * 1024};
    std::size_t max_event_bytes{kMaxEventBytes};
    std::size_t segment_bytes{64 * 1024 * 1024};
    std::size_t retention_bytes{1024 * 1024 * 1024};
    std::chrono::milliseconds retention_age{std::chrono::hours{24 * 7}};
    std::chrono::milliseconds sync_interval{1000};
    std::chrono::milliseconds shutdown_drain{1000};
    std::size_t max_segments{256};
};

enum class FeedbackDrop { Capture, Serialization, EventLimit, Contention, BufferFull,
                          Closed, Storage, Shutdown, IdentityExhausted, Count };

/// Bounded telemetry counters; written does not imply successfully synced.
struct FeedbackStats {
    bool enabled{false};
    bool storage_healthy{false};
    uint64_t captured{0}, admitted{0}, written{0}, synced{0};
    std::array<uint64_t, static_cast<std::size_t>(FeedbackDrop::Count)> dropped{};
    uint64_t writer_failures{0}, sync_failures{0}, recovery_failures{0};
    uint64_t uncertain_records{0}, retention_segments{0}, retention_records{0}, retention_bytes{0};
    std::size_t pending_records{0}, pending_bytes{0};
    std::optional<int64_t> sync_age_ms;
};

namespace detail { class FeedbackStorage; }
struct FeedbackWriterTestAccess;

/// Single-writer persistence; admission never waits for storage or broker locks.
class FeedbackWriter {
public:
    explicit FeedbackWriter(FeedbackConfig config, int64_t delivery_lease_ms);
    ~FeedbackWriter();
    FeedbackWriter(const FeedbackWriter&) = delete;
    FeedbackWriter& operator=(const FeedbackWriter&) = delete;
    /// Serialize and attempt nonblocking admission; failures are counted and contained.
    void publish(const FeedbackEvent& event) noexcept;
    /// Record a telemetry-only failure encountered before publication.
    void record_drop(FeedbackDrop reason) noexcept;
    /// Read approximate concurrent stats without disk access.
    [[nodiscard]] FeedbackStats stats() const noexcept;
    /// Stop admission, drain within the healthy target, and join safely; idempotent.
    void close() noexcept;
    /// Independent telemetry namespace, never settlement-token material.
    [[nodiscard]] const std::string& instance_id() const noexcept;
private:
    friend struct FeedbackWriterTestAccess;
    FeedbackWriter(FeedbackConfig config, int64_t lease, std::shared_ptr<detail::FeedbackStorage> storage);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace harbinger::ml
