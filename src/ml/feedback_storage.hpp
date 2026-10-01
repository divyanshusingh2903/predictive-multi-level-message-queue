#pragma once

#include <chrono>
#include <filesystem>
#include <sys/types.h>
#include "ml/feedback_writer.hpp"
#include <mutex>

namespace harbinger::ml::detail {

// Private syscall boundary for deterministic storage faults in tests.
class FeedbackStorage {
public:
    virtual ~FeedbackStorage() = default;
    virtual int open(const std::filesystem::path& path, int flags, mode_t mode);
    virtual ssize_t write(int fd, const void* data, std::size_t size);
    virtual ssize_t read(int fd, void* data, std::size_t size);
    virtual int sync(int fd);
    virtual int close(int fd);
    virtual int rename(const std::filesystem::path& from, const std::filesystem::path& to);
    virtual int remove(const std::filesystem::path& path);
    virtual std::chrono::system_clock::time_point wall_now();
};

} // namespace harbinger::ml::detail

namespace harbinger::ml {
// Private test access; production callers cannot configure fault injection.
struct FeedbackWriterTestAccess {
    static std::unique_ptr<FeedbackWriter> create(FeedbackConfig config, int64_t lease,
                                                std::shared_ptr<detail::FeedbackStorage> storage) {
        return std::unique_ptr<FeedbackWriter>(new FeedbackWriter(std::move(config), lease, std::move(storage)));
    }
    static std::unique_lock<std::mutex> gate_admission(FeedbackWriter& writer);
    static void exhaust_sequence(FeedbackWriter& writer);
};
}
