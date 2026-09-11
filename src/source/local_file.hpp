// LocalFileSource — a BlobSource over a local read-only file, using the
// Elio io_uring/epoll backend for positional reads.
#pragma once

#include "source/blob_source.hpp"

#include <elio/coro/task.hpp>

#include <atomic>
#include <memory>
#include <string>

namespace obd::source {

class LocalFileSource final : public BlobSource {
public:
    /// Opens `path` read-only (O_RDONLY) and stat()s its size. Synchronous
    /// open on the calling coroutine (cold path); reads are async.
    /// Throws obd::error on failure.
    static elio::coro::task<std::unique_ptr<LocalFileSource>> open(
        std::string path);

    ~LocalFileSource() override;

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;

    uint64_t size() const noexcept override {
        return size_.load(std::memory_order_acquire);
    }
    std::string_view label() const noexcept override { return label_; }

    int fd() const noexcept { return fd_; }

private:
    LocalFileSource() = default;

    int fd_ = -1;
    std::atomic<uint64_t> size_{0};
    std::string label_;
};

}  // namespace obd::source
