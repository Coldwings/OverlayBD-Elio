// Shared test support: coroutine runner, in-memory BlobSource, temp dirs,
// tar header builder.
#pragma once

#include "source/blob_source.hpp"

#include <elio/coro/task.hpp>
#include <elio/runtime/async_main.hpp>

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace obd::test {

/// Runs a coroutine to completion on a fresh scheduler and returns its
/// value.
template <typename F>
auto run_coro(F&& f) {
    return elio::run(std::forward<F>(f));
}

/// In-memory BlobSource with read counting and failure injection.
class VectorSource final : public source::BlobSource {
public:
    explicit VectorSource(std::vector<uint8_t> data, std::string label = "mem")
        : data_(std::move(data)), label_(std::move(label)) {}

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override {
        reads_.fetch_add(1, std::memory_order_relaxed);
        if (fail_with_ != 0) co_return -fail_with_;
        if (offset >= data_.size()) co_return 0;
        const size_t n =
            std::min(count, static_cast<size_t>(data_.size() - offset));
        std::memcpy(buf, data_.data() + offset, n);
        co_return static_cast<ssize_t>(n);
    }

    uint64_t size() const noexcept override { return data_.size(); }
    std::string_view label() const noexcept override { return label_; }

    uint64_t reads() const { return reads_.load(); }
    void fail_with(int err) { fail_with_ = err; }

    std::vector<uint8_t>& data() { return data_; }

private:
    std::vector<uint8_t> data_;
    std::string label_;
    std::atomic<uint64_t> reads_{0};
    int fail_with_ = 0;
};

/// RAII temporary directory.
class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("obd-test-" + std::to_string(::getpid()) + "-" +
                 std::to_string(counter_++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }

    const std::filesystem::path& path() const { return path_; }
    std::string str() const { return path_.string(); }
    std::string operator/(const std::string& name) const {
        return (path_ / name).string();
    }

private:
    inline static std::atomic<int> counter_{0};
    std::filesystem::path path_;
};

/// Writes `data` to a new file under `dir` and returns its path.
inline std::string write_file(const std::string& path,
                              const std::vector<uint8_t>& data) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) throw std::system_error(errno, std::system_category(),
                                        "cannot create " + path);
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t w = ::write(fd, data.data() + done, data.size() - done);
        if (w <= 0) {
            ::close(fd);
            throw std::system_error(errno, std::system_category(),
                                    "write failed");
        }
        done += static_cast<size_t>(w);
    }
    ::close(fd);
    return path;
}

/// Deterministic pseudo-random content (reproducible fixtures).
inline std::vector<uint8_t> pattern_bytes(size_t n, uint32_t seed = 42) {
    std::vector<uint8_t> out(n);
    uint32_t x = seed;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        out[i] = static_cast<uint8_t>(x >> 24);
    }
    return out;
}

/// Builds a minimal valid 512B ustar header (magic/version/chksum correct)
/// carrying `payload_size` in the octal size field. typeflag '0'.
inline std::vector<uint8_t> make_tar_header(uint64_t payload_size,
                                            char typeflag = '0') {
    std::vector<uint8_t> h(512, 0);
    std::memcpy(h.data(), "blob", 4);                 // name
    std::snprintf(reinterpret_cast<char*>(h.data() + 100), 8, "%07o", 0644);
    std::snprintf(reinterpret_cast<char*>(h.data() + 124), 12, "%011lo",
                  static_cast<unsigned long>(payload_size));
    h[156] = typeflag;
    std::memcpy(h.data() + 257, "ustar", 5);          // magic
    std::memcpy(h.data() + 263, "00", 2);             // version
    std::memset(h.data() + 148, ' ', 8);              // chksum field = spaces
    uint32_t sum = 0;
    for (size_t i = 0; i < 512; ++i) sum += h[i];
    std::snprintf(reinterpret_cast<char*>(h.data() + 148), 8, "%06o", sum);
    h[154] = '\0';
    h[155] = ' ';
    return h;
}

}  // namespace obd::test
