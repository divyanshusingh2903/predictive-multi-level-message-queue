#include "benchmark/hooks.hpp"
#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace harbinger::benchmark {
Observations::Observations(std::size_t capacity) : capacity_(capacity) {
    if (!capacity) throw std::invalid_argument("observation capacity must be positive");
    records_.resize(capacity);
}
void Observations::publish(Observation record) noexcept {
    record.published_ns = now_ns();
    const auto slot = next_.fetch_add(1, std::memory_order_relaxed);
    if (slot >= capacity_) { ++dropped_; return; }
    records_[slot] = record;
}
std::vector<Observation> Observations::snapshot() const {
    return {records_.begin(), records_.begin() + std::min(capacity_, next_.load())};
}
int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::optional<Observation> capture(const Message& message, Kind kind) noexcept {
    const auto found = message.headers.find("__benchmark_sequence");
    if (found == message.headers.end() || message.id.size() > 128) return std::nullopt;
    Observation record;
    const auto& value = found->second;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), record.sequence);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return std::nullopt;
    std::copy(message.id.begin(), message.id.end(), record.message_id.begin());
    record.attempt = message.delivery_count;
    record.arrival_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        message.arrival_time.time_since_epoch()).count();
    record.time_ns = now_ns();
    record.retries = message.retry_count;
    record.kind = kind;
    return record;
}
const char* name(Kind kind) noexcept {
    switch (kind) {
        case Kind::Ingress: return "ingress";
        case Kind::Dispatch: return "dispatch";
        case Kind::Ack: return "ack";
        case Kind::Retry: return "retry";
        case Kind::Ttl: return "ttl";
        case Kind::MaxRetries: return "max_retries";
    }
    return "invalid";
}
} // namespace harbinger::benchmark
