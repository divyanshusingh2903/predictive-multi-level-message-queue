#pragma once

#include "ml/feedback_writer.hpp"
#include "../../src/ml/feedback_storage.hpp"

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace feedback_test {
using namespace std::chrono_literals;
using namespace harbinger::ml;

class Directory {
public:
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "harbinger-feedback-XXXXXX").string();
        auto* name = ::mkdtemp(pattern.data());
        if (!name) throw std::runtime_error("test temporary directory");
        path = name;
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
    std::filesystem::path path;
    FeedbackConfig config() const { FeedbackConfig c; c.path = path; return c; }
};

inline bool wait_for(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!condition() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    return condition();
}
inline google::protobuf::Struct parse(const std::string& json) {
    google::protobuf::Struct result;
    EXPECT_TRUE(google::protobuf::util::JsonStringToMessage(json, &result).ok()) << json;
    return result;
}
inline std::vector<google::protobuf::Struct> records(const Directory& directory) {
    std::vector<google::protobuf::Struct> result;
    for (const auto& entry : std::filesystem::directory_iterator(directory.path)) {
        if (entry.path().extension() != ".jsonl") continue;
        std::ifstream input(entry.path());
        std::string line;
        while (std::getline(input, line)) result.push_back(parse(line));
    }
    return result;
}
inline FeedbackEvent event() {
    auto context = std::make_shared<RoutingContext>();
    context->feature_schema_version = "features-v1";
    context->routing_policy_version = "static-v1";
    context->features = FeatureSnapshot{.payload_size_bytes = 4096, .headers = {}, .missing_reasons = {}};
    harbinger::Message message;
    message.id = "123456-0";
    message.routing_context = context;
    message.arrival_time = std::chrono::steady_clock::now();
    auto e = capture_feedback(message, FeedbackTrigger::Settlement);
    e.attempt_id = 1; e.operation = FeedbackOperation::Ack; e.outcome = FeedbackOutcome::Ack;
    e.processing_time_ms = 0;
    return e;
}
inline uint64_t dropped(const FeedbackStats& stats, FeedbackDrop reason) {
    return stats.dropped[static_cast<std::size_t>(reason)];
}
inline bool admit(FeedbackWriter& writer, const FeedbackEvent& e) {
    const auto before = writer.stats().admitted;
    // Direct writer tests offer synthetic records until one is admitted. Contention drops are valid.
    return wait_for([&] {
        if (writer.stats().admitted != before) return true;
        writer.publish(e);
        return writer.stats().admitted != before;
    });
}

class Storage : public detail::FeedbackStorage {
public:
    enum class Fault { None, Open, Write, Sync, DirectorySync, Rename, Delete, Close };
    std::atomic<Fault> fault{Fault::None};
    std::atomic<bool> interrupt_write{false}, short_write{false};
    std::atomic<int64_t> bytes_before_failure{-1};
    std::atomic<int64_t> wall_offset_ms{0};
    std::mutex mutex;
    std::condition_variable cv;
    bool block{false}, entered{false};
    int open(const std::filesystem::path& path, int flags, mode_t mode) override {
        if (fault == Fault::Open && path.extension() == ".active") { errno = ENOSPC; return -1; }
        return FeedbackStorage::open(path, flags, mode);
    }
    ssize_t write(int fd, const void* data, std::size_t size) override {
        {
            std::unique_lock lock(mutex);
            if (block) { entered = true; cv.notify_all(); cv.wait(lock, [this] { return !block; }); }
        }
        if (interrupt_write.exchange(false)) { errno = EINTR; return -1; }
        if (fault == Fault::Write) { errno = ENOSPC; return -1; }
        const auto budget = bytes_before_failure.load();
        if (budget == 0) { errno = ENOSPC; return -1; }
        if (budget > 0) size = std::min(size, static_cast<std::size_t>(budget));
        const auto result = FeedbackStorage::write(fd, data, short_write ? std::min(size, std::size_t{7}) : size);
        if (budget > 0 && result > 0) bytes_before_failure -= result;
        return result;
    }
    int sync(int fd) override {
        struct stat info{}; ::fstat(fd, &info);
        if ((fault == Fault::Sync && S_ISREG(info.st_mode)) ||
            (fault == Fault::DirectorySync && S_ISDIR(info.st_mode))) { errno = EIO; return -1; }
        return FeedbackStorage::sync(fd);
    }
    int close(int fd) override {
        const auto result = FeedbackStorage::close(fd);
        if (fault == Fault::Close) { errno = EIO; return -1; }
        return result;
    }
    int rename(const std::filesystem::path& from, const std::filesystem::path& to) override {
        if (fault == Fault::Rename) { errno = EIO; return -1; }
        return FeedbackStorage::rename(from, to);
    }
    int remove(const std::filesystem::path& path) override {
        if (fault == Fault::Delete) { errno = EACCES; return -1; }
        return FeedbackStorage::remove(path);
    }
    std::chrono::system_clock::time_point wall_now() override {
        return FeedbackStorage::wall_now() + std::chrono::milliseconds(wall_offset_ms.load());
    }
    void gate() { std::lock_guard lock(mutex); block = true; entered = false; }
    bool await_entry() {
        std::unique_lock lock(mutex); return cv.wait_for(lock, 3s, [this] { return entered; });
    }
    void release() { std::lock_guard lock(mutex); block = false; cv.notify_all(); }
};
struct ReleaseStorage {
    std::shared_ptr<Storage> storage;
    ~ReleaseStorage() { storage->release(); }
};
} // namespace feedback_test
