// Shared test support: coroutine runner, in-memory BlobSource, temp dirs,
// tar header builder, ephemeral-port helper.
#pragma once

#include "source/blob_source.hpp"

#include <elio/coro/task.hpp>
#include <elio/runtime/async_main.hpp>
#include <elio/time/timer.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace obd::test {

/// Runs a coroutine to completion on a fresh scheduler and returns its
/// value.
template <typename F>
auto run_coro(F&& f) {
    return elio::run(std::forward<F>(f));
}

/// RAII reservation for an OS-assigned loopback TCP port. Each reservation
/// binds port 0 and reads the assigned port with getsockname(); mock servers
/// keep the reservation until immediately before their real HTTP listener
/// binds the same port. If an unrelated process wins that close-to-bind
/// window, the fixture reserves a fresh port and test code waits for
/// `is_running()` before publishing URLs/configuration. This removes the
/// repository's fixed-port collisions across sibling worktrees (issue #15).
class ReservedTcpPort {
public:
    ReservedTcpPort() { reset(); }
    ~ReservedTcpPort() { release(); }

    ReservedTcpPort(const ReservedTcpPort&) = delete;
    ReservedTcpPort& operator=(const ReservedTcpPort&) = delete;

    uint16_t port() const noexcept { return port_; }

    void reset() {
        release();
        port_ = 0;

        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            throw std::system_error(errno, std::system_category(), "socket");
        }
        sockaddr_in sa {};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = 0;
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) !=
            0) {
            const int e = errno;
            ::close(fd);
            throw std::system_error(e, std::system_category(), "bind");
        }
        socklen_t len = sizeof(sa);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len) != 0) {
            const int e = errno;
            ::close(fd);
            throw std::system_error(e, std::system_category(), "getsockname");
        }
        const auto port = ntohs(sa.sin_port);
        if (port == 0) {
            ::close(fd);
            throw std::runtime_error("ReservedTcpPort: no port assigned");
        }
        fd_ = fd;
        port_ = port;
    }

    void release() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
    uint16_t port_ = 0;
};

/// Binds and listens on a specific loopback port to force a deterministic
/// EADDRINUSE collision in mock-server fixture tests.
class TcpPortBlocker {
public:
    explicit TcpPortBlocker(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            throw std::system_error(errno, std::system_category(), "socket");
        }
        sockaddr_in sa {};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(port);
        if (::bind(fd_, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) !=
            0) {
            const int e = errno;
            release();
            throw std::system_error(e, std::system_category(), "bind");
        }
        if (::listen(fd_, 1) != 0) {
            const int e = errno;
            release();
            throw std::system_error(e, std::system_category(), "listen");
        }
    }
    ~TcpPortBlocker() { release(); }

    TcpPortBlocker(const TcpPortBlocker&) = delete;
    TcpPortBlocker& operator=(const TcpPortBlocker&) = delete;

private:
    void release() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd_ = -1;
};

template <typename Server>
elio::coro::task<bool> wait_server_running(
    const Server& server,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    constexpr auto kPoll = std::chrono::milliseconds(5);
    for (auto waited = std::chrono::milliseconds(0); waited < timeout;
         waited += kPoll) {
        if (server.is_running()) co_return true;
        co_await elio::time::sleep_for(kPoll);
    }
    co_return server.is_running();
}

/// Elio's pinned http::server::listen() logs and returns on bind/listen
/// failure, preserving errno from tcp_listener::bind(). The mock fixtures
/// retry only the close-to-bind race they are designed to handle.
inline void require_retryable_http_listen_return(const char* what,
                                                 int listen_errno) {
    if (listen_errno == EADDRINUSE) return;
    if (listen_errno != 0) {
        throw std::system_error(listen_errno, std::system_category(), what);
    }
    throw std::runtime_error(std::string(what) +
                             ": listen returned before stop without errno");
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
