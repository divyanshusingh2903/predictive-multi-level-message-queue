#include "proxy/proxy.hpp"

#include <atomic>
#include <chrono>
#include <stdexcept>

namespace pmlmq {

Proxy::Proxy(MessageSink sink) : sink_(std::move(sink)) {
    if (!sink_) {
        throw std::invalid_argument("Proxy: sink must not be null");
    }
}

std::string Proxy::generate_id() {
    static std::atomic<uint64_t> counter{0};
    const auto ts =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    return std::to_string(ts) + "-" +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

std::string Proxy::accept(std::vector<uint8_t> payload,
                          std::unordered_map<std::string, std::string> headers,
                          const std::string& producer_id) {
    Message msg;
    msg.id                       = generate_id();
    msg.arrival_time             = std::chrono::steady_clock::now();
    msg.payload                  = std::move(payload);
    msg.headers                  = std::move(headers);
    msg.headers["__producer_id"] = producer_id;

    const std::string id = msg.id; // copy before move
    accepted_.fetch_add(1, std::memory_order_relaxed);
    sink_(std::move(msg));
    return id;
}

} // namespace pmlmq
