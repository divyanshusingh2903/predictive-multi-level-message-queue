#include "ml/feedback_writer.hpp"
#include "feedback_storage.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <random>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace harbinger::ml::detail {
int FeedbackStorage::open(const std::filesystem::path& p, int f, mode_t m) { return ::open(p.c_str(), f, m); }
ssize_t FeedbackStorage::write(int f, const void* p, std::size_t n) { return ::write(f, p, n); }
ssize_t FeedbackStorage::read(int f, void* p, std::size_t n) { return ::read(f, p, n); }
int FeedbackStorage::sync(int f) { return ::fsync(f); }
int FeedbackStorage::close(int f) { return ::close(f); }
int FeedbackStorage::rename(const std::filesystem::path& a, const std::filesystem::path& b) { return ::rename(a.c_str(), b.c_str()); }
int FeedbackStorage::remove(const std::filesystem::path& p) { return ::unlink(p.c_str()); }
std::chrono::system_clock::time_point FeedbackStorage::wall_now() { return std::chrono::system_clock::now(); }
} // namespace harbinger::ml::detail

namespace harbinger::ml {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto kRecoveryBackoff = std::chrono::milliseconds{200};
constexpr std::size_t index(FeedbackDrop reason) { return static_cast<std::size_t>(reason); }
void fail(const char* reason) { throw std::runtime_error(reason); }
FeedbackConfig validate(FeedbackConfig config, int64_t lease) {
    const auto max_duration = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max() / 2);
    if (lease <= 0 || config.path.empty() || !config.buffer_records || !config.max_segments ||
        !config.max_event_bytes || config.max_event_bytes > kMaxEventBytes ||
        config.buffer_bytes <= config.max_event_bytes || config.segment_bytes <= config.max_event_bytes ||
        config.retention_bytes < config.segment_bytes || config.retention_age.count() <= 0 ||
        config.sync_interval.count() <= 0 || config.shutdown_drain.count() <= 0 ||
        config.retention_age > max_duration || config.sync_interval > max_duration ||
        config.shutdown_drain > max_duration)
        throw std::invalid_argument("invalid feedback capacities/durations/path");
    if (!std::filesystem::is_directory(config.path) || std::filesystem::is_symlink(config.path))
        throw std::invalid_argument("feedback requires an existing dedicated non-symlink directory");
    config.path = std::filesystem::canonical(config.path);
    return config;
}
std::string instance_id() {
    std::random_device random;
    constexpr char hex[] = "0123456789abcdef";
    std::string id;
    id.reserve(32);
    for (int i = 0; i < 16; ++i) {
        const auto byte = random() & 255;
        id += hex[byte >> 4]; id += hex[byte & 15];
    }
    return id;
}
bool recognized(std::string_view name) {
    if (!name.starts_with("feedback-") || name.size() < 9 + 32 + 1 + 20 + 6) return false;
    for (std::size_t i = 9; i < 41; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return false;
    if (name[41] != '-') return false;
    for (std::size_t i = 42; i < 62; ++i) if (name[i] < '0' || name[i] > '9') return false;
    const auto suffix = name.substr(62);
    return suffix == ".active" || suffix == ".jsonl" || suffix == ".suspect";
}
} // namespace

struct FeedbackWriter::Impl {
    FeedbackConfig config;
    int64_t lease;
    std::shared_ptr<detail::FeedbackStorage> storage;
    const std::string instance{ml::instance_id()};
    std::atomic<uint64_t> sequence{0}, captured{0}, admitted{0}, written{0}, synced{0};
    std::array<std::atomic<uint64_t>, index(FeedbackDrop::Count)> drops{};
    std::atomic<uint64_t> failures{0}, sync_failures{0}, recovery_failures{0}, uncertain{0};
    std::atomic<uint64_t> removed_segments{0}, removed_records{0}, removed_bytes{0};
    std::atomic<std::size_t> pending_records{0}, pending_bytes{0};
    std::atomic<int64_t> last_sync_ns{0};
    std::atomic<bool> healthy{false};
    std::mutex mutex, close_mutex;
    std::condition_variable cv;
    std::vector<std::string> slots;
    std::size_t head{0}, tail{0}, queued{0};
    bool closing{false};
    Clock::time_point drain_deadline{};
    std::thread worker;
    int lock_fd{-1}, directory_fd{-1}, fd{-1};
    std::filesystem::path active;
    std::size_t active_bytes{0};
    uint64_t generation{0}, unsynced{0};

    Impl(FeedbackConfig c, int64_t l, std::shared_ptr<detail::FeedbackStorage> s)
        : config(validate(std::move(c), l)), lease(l), storage(std::move(s)), slots(config.buffer_records) {
        try {
            directory_fd = storage->open(config.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0);
            if (directory_fd < 0) fail("cannot open feedback directory");
            lock_fd = storage->open(config.path / ".feedback.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (lock_fd < 0 || ::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) fail("feedback directory already owned or unavailable");
            recover_startup();
            open_segment();
            healthy = true;
            worker = std::thread([this] { run(); });
        } catch (...) { release_files(); throw; }
    }
    ~Impl() { close(); release_files(); }
    void release_files() noexcept {
        if (fd >= 0) { storage->close(fd); fd = -1; }
        if (lock_fd >= 0) { ::flock(lock_fd, LOCK_UN); storage->close(lock_fd); lock_fd = -1; }
        if (directory_fd >= 0) { storage->close(directory_fd); directory_fd = -1; }
    }
    void drop(FeedbackDrop reason) noexcept {
        if (index(reason) < drops.size()) ++drops[index(reason)];
    }

    struct Catalog {
        std::size_t bytes{0}, count{0};
        std::filesystem::path oldest;
        std::chrono::system_clock::time_point oldest_time{std::chrono::system_clock::time_point::max()};
        std::size_t oldest_bytes{0};
    };
    Catalog catalog() {
        Catalog result;
        for (const auto& entry : std::filesystem::directory_iterator(config.path)) {
            if (!recognized(entry.path().filename().string())) continue;
            struct stat info{};
            if (::lstat(entry.path().c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
                fail("invalid feedback segment");
            const auto bytes = static_cast<std::size_t>(info.st_size);
            if (bytes > std::numeric_limits<std::size_t>::max() - result.bytes) fail("feedback storage size overflow");
            result.bytes += bytes; ++result.count;
            if (entry.path() == active) continue;
            const auto time = std::chrono::system_clock::from_time_t(info.st_mtime);
            if (time < result.oldest_time || (time == result.oldest_time && entry.path() < result.oldest)) {
                result.oldest_time = time; result.oldest = entry.path(); result.oldest_bytes = bytes;
            }
        }
        return result;
    }
    uint64_t line_count(const std::filesystem::path& path) {
        const int input = storage->open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0);
        if (input < 0) fail("cannot inspect feedback segment");
        uint64_t count = 0;
        char buffer[4096];
        try {
            while (true) {
                const auto n = storage->read(input, buffer, sizeof buffer);
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) fail("cannot read feedback segment");
                if (!n) break;
                count += static_cast<uint64_t>(std::count(buffer, buffer + n, '\n'));
            }
        } catch (...) { storage->close(input); throw; }
        storage->close(input);
        return count;
    }
    void retention(std::size_t extra_bytes = 0, std::size_t extra_segments = 0) {
        while (true) {
            const auto files = catalog();
            const bool expired = !files.oldest.empty() && storage->wall_now() >= files.oldest_time &&
                storage->wall_now() - files.oldest_time >= config.retention_age;
            const bool over = files.bytes > config.retention_bytes - extra_bytes ||
                files.count > config.max_segments - extra_segments;
            if (!expired && !over) return;
            if (files.oldest.empty()) fail("active feedback segment prevents retention");
            const auto records = line_count(files.oldest);
            if (storage->remove(files.oldest) != 0) fail("feedback retention deletion failed");
            ++removed_segments; removed_records += records; removed_bytes += files.oldest_bytes;
            if (storage->sync(directory_fd) != 0) fail("feedback retention directory sync failed");
        }
    }
    void recover_startup() {
        for (const auto& entry : std::filesystem::directory_iterator(config.path)) {
            if (!recognized(entry.path().filename().string())) continue;
            if (entry.is_symlink() || !entry.is_regular_file()) fail("unsafe feedback segment");
            if (entry.path().extension() == ".active") {
                auto suspect = entry.path(); suspect.replace_extension(".suspect");
                if (std::filesystem::exists(suspect) || storage->rename(entry.path(), suspect) != 0)
                    fail("cannot quarantine stale feedback segment");
            }
        }
        if (storage->sync(directory_fd) != 0) fail("feedback recovery directory sync failed");
        retention();
    }
    void open_segment() {
        retention(0, 1);
        if (generation == std::numeric_limits<uint64_t>::max()) fail("feedback generation exhausted");
        const auto digits = std::to_string(++generation);
        active = config.path / ("feedback-" + instance + "-" + std::string(20 - digits.size(), '0') + digits + ".active");
        fd = storage->open(active, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) { active.clear(); fail("cannot create feedback segment"); }
        active_bytes = 0; unsynced = 0;
        if (storage->sync(directory_fd) != 0) fail("feedback create directory sync failed");
    }
    void sync_active() {
        if (fd < 0) return;
        int result;
        do { result = storage->sync(fd); } while (result != 0 && errno == EINTR);
        if (result != 0) { ++sync_failures; fail("feedback sync failed"); }
        synced += unsynced; unsynced = 0;
        last_sync_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    }
    void seal() {
        sync_active();
        const int closing_fd = fd; fd = -1;
        if (storage->close(closing_fd) != 0) fail("feedback close failed");
        auto sealed = active; sealed.replace_extension(".jsonl");
        if (storage->rename(active, sealed) != 0) fail("feedback seal failed");
        active.clear(); active_bytes = 0;
        if (storage->sync(directory_fd) != 0) fail("feedback seal directory sync failed");
    }
    void abandon() noexcept {
        healthy = false;
        uncertain += unsynced; unsynced = 0;
        if (fd >= 0) { storage->close(fd); fd = -1; }
        // If renaming fails, retain the active name for recovery rather than creating unbounded tails.
        try {
            if (!active.empty()) {
                auto suspect = active; suspect.replace_extension(".suspect");
                if (storage->rename(active, suspect) == 0) active.clear();
            }
        } catch (...) {}
    }
    void recover() {
        if (!active.empty()) {
            auto suspect = active; suspect.replace_extension(".suspect");
            if (storage->rename(active, suspect) != 0) fail("feedback quarantine failed");
            active.clear();
        }
        open_segment(); healthy = true;
    }
    void append(const std::string& record) {
        if (record.size() > config.segment_bytes - active_bytes) { seal(); open_segment(); }
        retention(record.size());
        std::size_t offset = 0;
        while (offset < record.size()) {
            const auto n = storage->write(fd, record.data() + offset, record.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ++uncertain; fail("feedback append failed"); }
            offset += static_cast<std::size_t>(n); active_bytes += static_cast<std::size_t>(n);
        }
        ++written; ++unsynced;
    }
    void finish_pending(std::size_t bytes) noexcept { --pending_records; pending_bytes -= bytes; }
    void discard_locked() noexcept {
        while (queued) {
            finish_pending(slots[head].size()); slots[head].clear();
            head = (head + 1) % slots.size(); --queued; drop(FeedbackDrop::Shutdown);
        }
    }
    void run() noexcept {
        auto next_sync = Clock::now() + config.sync_interval;
        auto next_recovery = Clock::now();
        while (true) {
            std::string record;
            {
                std::unique_lock lock(mutex);
                const auto wake = healthy ? next_sync : next_recovery;
                cv.wait_until(lock, wake, [this] { return closing || queued != 0; });
                if (closing && Clock::now() >= drain_deadline) discard_locked();
                if (closing && !queued) break;
                if (queued) {
                    record = std::move(slots[head]); head = (head + 1) % slots.size(); --queued;
                }
            }
            if (!healthy && Clock::now() >= next_recovery) {
                try { recover(); next_sync = Clock::now() + config.sync_interval; }
                catch (...) { ++recovery_failures; abandon(); next_recovery = Clock::now() + kRecoveryBackoff; }
            }
            if (!record.empty()) {
                if (!healthy) drop(FeedbackDrop::Storage);
                else {
                    try { append(record); }
                    catch (...) { ++failures; drop(FeedbackDrop::Storage); abandon(); next_recovery = Clock::now() + kRecoveryBackoff; }
                }
                finish_pending(record.size());
            }
            if (healthy && Clock::now() >= next_sync) {
                try { sync_active(); retention(); }
                catch (...) { ++failures; abandon(); next_recovery = Clock::now() + kRecoveryBackoff; }
                next_sync = Clock::now() + config.sync_interval;
            }
        }
        if (healthy) {
            try { seal(); retention(); }
            catch (...) { ++failures; abandon(); }
        }
    }
    void close() noexcept {
        std::lock_guard join(close_mutex);
        {
            std::lock_guard lock(mutex);
            if (!closing) { closing = true; drain_deadline = Clock::now() + config.shutdown_drain; }
        }
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }
};

FeedbackWriter::FeedbackWriter(FeedbackConfig config, int64_t lease)
    : FeedbackWriter(std::move(config), lease, std::make_shared<detail::FeedbackStorage>()) {}
FeedbackWriter::FeedbackWriter(FeedbackConfig config, int64_t lease, std::shared_ptr<detail::FeedbackStorage> storage)
    : impl_(std::make_unique<Impl>(std::move(config), lease, std::move(storage))) {}
FeedbackWriter::~FeedbackWriter() = default;
void FeedbackWriter::record_drop(FeedbackDrop reason) noexcept { impl_->drop(reason); }
void FeedbackWriter::publish(const FeedbackEvent& event) noexcept {
    auto& p = *impl_;
    ++p.captured;
    try {
        auto seq = p.sequence.load();
        do {
            if (seq == std::numeric_limits<uint64_t>::max()) { p.drop(FeedbackDrop::IdentityExhausted); return; }
        } while (!p.sequence.compare_exchange_weak(seq, seq + 1));
        auto record = feedback_json(event, p.instance, seq + 1, p.lease, p.config.max_event_bytes);
        record += '\n';
        std::unique_lock lock(p.mutex, std::try_to_lock);
        if (!lock.owns_lock()) { p.drop(FeedbackDrop::Contention); return; }
        if (p.closing) { p.drop(FeedbackDrop::Closed); return; }
        if (!p.healthy) { p.drop(FeedbackDrop::Storage); return; }
        if (p.pending_records >= p.config.buffer_records ||
            record.size() > p.config.buffer_bytes - p.pending_bytes.load()) { p.drop(FeedbackDrop::BufferFull); return; }
        p.pending_bytes += record.size(); ++p.pending_records;
        p.slots[p.tail] = std::move(record); p.tail = (p.tail + 1) % p.slots.size(); ++p.queued;
        ++p.admitted;
        lock.unlock(); p.cv.notify_one();
    } catch (const std::length_error&) { p.drop(FeedbackDrop::EventLimit); }
      catch (...) { p.drop(FeedbackDrop::Serialization); }
}
FeedbackStats FeedbackWriter::stats() const noexcept {
    const auto& p = *impl_;
    FeedbackStats s;
    s.enabled = true; s.storage_healthy = p.healthy;
    s.captured = p.captured; s.admitted = p.admitted; s.written = p.written; s.synced = p.synced;
    for (std::size_t i = 0; i < s.dropped.size(); ++i) s.dropped[i] = p.drops[i];
    s.writer_failures = p.failures; s.sync_failures = p.sync_failures; s.recovery_failures = p.recovery_failures;
    s.uncertain_records = p.uncertain; s.retention_segments = p.removed_segments;
    s.retention_records = p.removed_records; s.retention_bytes = p.removed_bytes;
    s.pending_records = p.pending_records; s.pending_bytes = p.pending_bytes;
    const auto last = p.last_sync_ns.load();
    if (last) s.sync_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch() - std::chrono::nanoseconds(last)).count();
    return s;
}
void FeedbackWriter::close() noexcept { impl_->close(); }
const std::string& FeedbackWriter::instance_id() const noexcept { return impl_->instance; }
std::unique_lock<std::mutex> FeedbackWriterTestAccess::gate_admission(FeedbackWriter& writer) {
    return std::unique_lock(writer.impl_->mutex);
}
void FeedbackWriterTestAccess::exhaust_sequence(FeedbackWriter& writer) {
    writer.impl_->sequence = std::numeric_limits<uint64_t>::max();
}
} // namespace harbinger::ml
