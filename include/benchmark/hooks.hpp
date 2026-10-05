#pragma once

#include "queue/message.hpp"
#include <array>
#include <map>
#include <memory>
#include <atomic>
#include <optional>
#include <string>

namespace harbinger::benchmark {

enum class Policy { Disabled, Fifo, Static, RoundRobin, Oracle };
enum class Kind { Ingress, Dispatch, Ack, Retry, Ttl, MaxRetries };

/// Fixed-size benchmark observation, without payload or settlement token.
struct Observation {
    std::array<char, 129> message_id{};
    uint64_t sequence{0};
    uint64_t attempt{0};
    int64_t arrival_ns{0};
    int64_t time_ns{0};
    int64_t published_ns{0};
    uint32_t retries{0};
    Kind kind{Kind::Ingress};
};

/// Finite nonblocking observation sink; loss invalidates benchmark coverage.
class Observations {
public:
    explicit Observations(std::size_t capacity);
    void publish(Observation observation) noexcept;
    void drop() noexcept { ++dropped_; }
    /// Copy after all publishers have quiesced.
    [[nodiscard]] std::vector<Observation> snapshot() const;
    [[nodiscard]] uint64_t dropped() const noexcept { return dropped_.load(); }
private:
    std::vector<Observation> records_;
    std::size_t capacity_;
    std::atomic<std::size_t> next_{0};
    std::atomic<uint64_t> dropped_{0};
};

/// Immutable benchmark-only ingress assignments and observation configuration.
struct Options {
    Policy policy{Policy::Disabled};
    std::array<uint8_t, 3> job_tiers{0, 1, 2};
    std::shared_ptr<Observations> observations;
    /// When nonempty, Static maps the static_header value through this table instead of job_tiers.
    std::map<std::string, uint8_t> static_tiers{};
    std::string static_header{"job"};
};

[[nodiscard]] int64_t now_ns() noexcept;
[[nodiscard]] std::optional<Observation> capture(const Message& message, Kind kind) noexcept;
[[nodiscard]] const char* name(Kind kind) noexcept;
/// Header carrying the true-cost tier for the Oracle policy; set only by benchmark producers.
inline constexpr const char* kOracleTierHeader = "__benchmark_oracle_tier";

} // namespace harbinger::benchmark
