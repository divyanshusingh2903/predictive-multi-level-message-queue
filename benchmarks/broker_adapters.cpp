#include "broker_adapters.hpp"

#include <hiredis/hiredis.h>
#if __has_include(<rabbitmq-c/amqp.h>)  // rabbitmq-c 0.12 and later
#include <rabbitmq-c/amqp.h>
#include <rabbitmq-c/tcp_socket.h>
#else  // rabbitmq-c 0.11 (Ubuntu 24.04)
#include <amqp.h>
#include <amqp_tcp_socket.h>
#endif

#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <sys/time.h>
#include <thread>
#include <utility>
#include <vector>

namespace harbinger::bench {
namespace {

constexpr const char* kQueue = "prodflow";
constexpr const char* kRedisQueue = "prodflow:queue";
constexpr int kMaxPriority = 3;
/// How long a worker waits for a delivery before checking whether it should stop.
constexpr int kPollMs = 200;

uint64_t parse_seq(std::string_view text) {
    uint64_t seq = 0;
    if (text.empty()) throw std::runtime_error("delivery without a sequence number");
    for (const char c : text) {
        if (c < '0' || c > '9') throw std::runtime_error("bad sequence number in delivery");
        seq = seq * 10 + static_cast<uint64_t>(c - '0');
    }
    return seq;
}

// ── RabbitMQ (rabbitmq-c) ───────────────────────────────────────────────────

void check_rpc(amqp_rpc_reply_t reply, const char* what) {
    if (reply.reply_type == AMQP_RESPONSE_NORMAL) return;
    std::string detail = "unknown";
    if (reply.reply_type == AMQP_RESPONSE_LIBRARY_EXCEPTION) detail = amqp_error_string2(reply.library_error);
    else if (reply.reply_type == AMQP_RESPONSE_SERVER_EXCEPTION) detail = "server exception";
    throw std::runtime_error(std::string("RabbitMQ ") + what + ": " + detail);
}

void check_status(int status, const char* what) {
    if (status < 0) throw std::runtime_error(std::string("RabbitMQ ") + what + ": " + amqp_error_string2(status));
}

/// One connection with channel 1 open.
class AmqpConnection {
public:
    explicit AmqpConnection(const Endpoint& endpoint) : conn_(amqp_new_connection()) {
        amqp_socket_t* socket = amqp_tcp_socket_new(conn_);
        if (!socket) throw std::runtime_error("RabbitMQ: cannot create socket");
        check_status(amqp_socket_open(socket, endpoint.host.c_str(), endpoint.port), "connect");
        check_rpc(amqp_login(conn_, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN, "guest", "guest"), "login");
        amqp_channel_open(conn_, 1);
        check_rpc(amqp_get_rpc_reply(conn_), "channel.open");
    }
    AmqpConnection(const AmqpConnection&) = delete;
    AmqpConnection& operator=(const AmqpConnection&) = delete;
    ~AmqpConnection() {
        amqp_channel_close(conn_, 1, AMQP_REPLY_SUCCESS);
        amqp_connection_close(conn_, AMQP_REPLY_SUCCESS);
        amqp_destroy_connection(conn_);
    }
    amqp_connection_state_t get() const noexcept { return conn_; }

    std::string server_version() const {
        const amqp_table_t* properties = amqp_get_server_properties(conn_);
        for (int i = 0; properties && i < properties->num_entries; ++i) {
            const auto& entry = properties->entries[i];
            const std::string_view key(static_cast<const char*>(entry.key.bytes), entry.key.len);
            if (key == "version" && entry.value.kind == AMQP_FIELD_KIND_UTF8)
                return {static_cast<const char*>(entry.value.value.bytes.bytes), entry.value.value.bytes.len};
        }
        return "unknown";
    }

private:
    amqp_connection_state_t conn_;
};

class AmqpPublisher final : public Publisher {
public:
    explicit AmqpPublisher(const Endpoint& endpoint) : connection_(endpoint) {}
    void publish(uint64_t seq, const std::string& payload, std::optional<uint8_t> tier) override {
        const std::string id = std::to_string(seq);
        amqp_basic_properties_t props{};
        props._flags = AMQP_BASIC_DELIVERY_MODE_FLAG | AMQP_BASIC_MESSAGE_ID_FLAG;
        props.delivery_mode = 1;  // transient: #37 compares in-memory brokers
        props.message_id = amqp_cstring_bytes(id.c_str());
        if (tier) {
            props._flags |= AMQP_BASIC_PRIORITY_FLAG;
            props.priority = static_cast<uint8_t>(*tier >= 2 ? 0 : 2 - *tier);
        }
        amqp_bytes_t body{payload.size(), const_cast<char*>(payload.data())};
        check_status(amqp_basic_publish(connection_.get(), 1, amqp_empty_bytes, amqp_cstring_bytes(kQueue), 0, 0,
                                        &props, body), "publish");
    }

private:
    AmqpConnection connection_;
};

// ── Redis (hiredis) ─────────────────────────────────────────────────────────

struct RedisReply {
    redisReply* reply;
    explicit RedisReply(void* r) : reply(static_cast<redisReply*>(r)) {}
    RedisReply(RedisReply&& other) noexcept : reply(std::exchange(other.reply, nullptr)) {}
    RedisReply(const RedisReply&) = delete;
    RedisReply& operator=(const RedisReply&) = delete;
    ~RedisReply() { if (reply) freeReplyObject(reply); }
};

class RedisConnection {
public:
    explicit RedisConnection(const Endpoint& endpoint) {
        const timeval timeout{5, 0};
        ctx_ = redisConnectWithTimeout(endpoint.host.c_str(), endpoint.port, timeout);
        if (!ctx_ || ctx_->err)
            throw std::runtime_error(std::string("Redis connect: ") + (ctx_ ? ctx_->errstr : "out of memory"));
    }
    RedisConnection(const RedisConnection&) = delete;
    RedisConnection& operator=(const RedisConnection&) = delete;
    ~RedisConnection() { redisFree(ctx_); }

    /// Run a command; throws on a connection error or an error reply.
    template <class... Args>
    RedisReply command(const char* format, Args... args) {
        RedisReply r{redisCommand(ctx_, format, args...)};
        if (!r.reply) throw std::runtime_error(std::string("Redis: ") + ctx_->errstr);
        if (r.reply->type == REDIS_REPLY_ERROR) throw std::runtime_error(std::string("Redis: ") + r.reply->str);
        return r;
    }

private:
    redisContext* ctx_{nullptr};
};

class RedisPublisher final : public Publisher {
public:
    explicit RedisPublisher(const Endpoint& endpoint) : connection_(endpoint) {}
    void publish(uint64_t seq, const std::string& payload, std::optional<uint8_t>) override {
        const std::string message = std::to_string(seq) + ":" + payload;
        connection_.command("LPUSH %s %b", kRedisQueue, message.data(), message.size());
    }

private:
    RedisConnection connection_;
};

// ── Workers ─────────────────────────────────────────────────────────────────

class ThreadWorkers final : public Workers {
public:
    ThreadWorkers(int count, std::function<void(int, const std::atomic<bool>&)> loop) {
        for (int i = 0; i < count; ++i)
            threads_.emplace_back([this, i, loop] {
                try {
                    loop(i, stopping_);
                } catch (const std::exception& e) {
                    // A dead worker leaves work undone, so the run cannot complete; record why.
                    std::lock_guard lock{mutex_};
                    if (error_.empty()) error_ = e.what();
                    std::cerr << "[prodflow] worker " << i << " stopped: " << e.what() << '\n';
                }
            });
    }
    ~ThreadWorkers() override { stop(); }
    void stop() override {
        stopping_ = true;
        for (auto& t : threads_) if (t.joinable()) t.join();
    }
    std::string error() const override {
        std::lock_guard lock{mutex_};
        return error_;
    }

private:
    std::atomic<bool> stopping_{false};
    mutable std::mutex mutex_;
    std::string error_;
    std::vector<std::thread> threads_;
};

void amqp_worker(const Endpoint& endpoint, const Handle& handle, const std::atomic<bool>& stopping) {
    AmqpConnection connection{endpoint};
    const auto conn = connection.get();
    amqp_basic_qos(conn, 1, 0, 1, 0);  // prefetch 1: priorities can reorder only undelivered messages
    check_rpc(amqp_get_rpc_reply(conn), "basic.qos");
    amqp_basic_consume(conn, 1, amqp_cstring_bytes(kQueue), amqp_empty_bytes, 0, 0, 0, amqp_empty_table);
    check_rpc(amqp_get_rpc_reply(conn), "basic.consume");
    while (!stopping) {
        amqp_maybe_release_buffers(conn);
        amqp_envelope_t envelope;
        timeval timeout{0, kPollMs * 1000};
        const auto reply = amqp_consume_message(conn, &envelope, &timeout, 0);
        if (reply.reply_type == AMQP_RESPONSE_LIBRARY_EXCEPTION && reply.library_error == AMQP_STATUS_TIMEOUT) continue;
        check_rpc(reply, "consume");
        const auto& id = envelope.message.properties.message_id;
        const bool ok = handle(parse_seq({static_cast<const char*>(id.bytes), id.len}));
        const int status = ok ? amqp_basic_ack(conn, 1, envelope.delivery_tag, 0)
                              : amqp_basic_nack(conn, 1, envelope.delivery_tag, 0, 1);
        amqp_destroy_envelope(&envelope);
        check_status(status, ok ? "ack" : "nack");
    }
}

void redis_worker(const Endpoint& endpoint, int index, const Handle& handle, const std::atomic<bool>& stopping) {
    RedisConnection connection{endpoint};
    const std::string processing = std::string("prodflow:processing:") + std::to_string(index);
    const std::string timeout = std::to_string(kPollMs / 1000.0);
    while (!stopping) {
        auto r = connection.command("BLMOVE %s %s RIGHT LEFT %s", kRedisQueue, processing.c_str(), timeout.c_str());
        if (r.reply->type == REDIS_REPLY_NIL) continue;
        if (r.reply->type != REDIS_REPLY_STRING) throw std::runtime_error("Redis BLMOVE: unexpected reply");
        const std::string_view message(r.reply->str, r.reply->len);
        const auto colon = message.find(':');
        if (colon == std::string_view::npos) throw std::runtime_error("Redis: message without a sequence number");
        if (handle(parse_seq(message.substr(0, colon))))
            connection.command("LREM %s 1 %b", processing.c_str(), message.data(), message.size());
        else  // retried: back to the tail of the queue, which workers reach last
            connection.command("LMOVE %s %s LEFT LEFT", processing.c_str(), kRedisQueue);
    }
}

} // namespace

Endpoint parse_endpoint(const std::string& text) {
    const auto colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == text.size())
        throw std::invalid_argument("expected HOST:PORT, got " + text);
    return {text.substr(0, colon), std::stoi(text.substr(colon + 1))};
}

std::string prepare_queue(BrokerKind kind, const Endpoint& endpoint, bool priorities) {
    if (kind == BrokerKind::Redis) {
        RedisConnection connection{endpoint};
        connection.command("FLUSHALL");  // the broker serves only this benchmark run
        auto info = connection.command("INFO server");
        const std::string text(info.reply->str, info.reply->len);
        const auto at = text.find("redis_version:");
        return at == std::string::npos ? "unknown" : text.substr(at + 14, text.find('\r', at) - at - 14);
    }
    AmqpConnection connection{endpoint};
    const auto conn = connection.get();
    amqp_queue_delete(conn, 1, amqp_cstring_bytes(kQueue), 0, 0);
    check_rpc(amqp_get_rpc_reply(conn), "queue.delete");
    amqp_table_entry_t entry{};
    entry.key = amqp_cstring_bytes("x-max-priority");
    entry.value.kind = AMQP_FIELD_KIND_I32;
    entry.value.value.i32 = kMaxPriority;
    const amqp_table_t arguments{priorities ? 1 : 0, priorities ? &entry : nullptr};
    // Classic, non-durable, not exclusive, not auto-delete.
    amqp_queue_declare(conn, 1, amqp_cstring_bytes(kQueue), 0, 0, 0, 0, arguments);
    check_rpc(amqp_get_rpc_reply(conn), "queue.declare");
    return connection.server_version();
}

std::unique_ptr<Publisher> make_publisher(BrokerKind kind, const Endpoint& endpoint) {
    if (kind == BrokerKind::Redis) return std::make_unique<RedisPublisher>(endpoint);
    return std::make_unique<AmqpPublisher>(endpoint);
}

std::unique_ptr<Workers> start_workers(BrokerKind kind, const Endpoint& endpoint, int count, Handle handle) {
    return std::make_unique<ThreadWorkers>(count, [kind, endpoint, handle](int index, const std::atomic<bool>& stopping) {
        if (kind == BrokerKind::Redis) redis_worker(endpoint, index, handle, stopping);
        else amqp_worker(endpoint, handle, stopping);
    });
}

std::string client_library(BrokerKind kind) {
    if (kind == BrokerKind::Redis)
        return "hiredis " + std::to_string(HIREDIS_MAJOR) + "." + std::to_string(HIREDIS_MINOR) + "." +
               std::to_string(HIREDIS_PATCH);
    return std::string("rabbitmq-c ") + amqp_version();
}

} // namespace harbinger::bench
