// Background downloader. See downloader.hpp.
#include "source/downloader.hpp"

#include "common/errors.hpp"
#include "common/sha256.hpp"

#include <elio/io/io_awaitables.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/time/timer.hpp>

#include <elio/log/macros.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace obd::source {

Downloader::Downloader(BlobSourcePtr remote, std::string dir,
                       std::string expected_sha256, DownloadConfig cfg)
    : remote_(std::move(remote)),
      dir_(std::move(dir)),
      expected_sha256_(std::move(expected_sha256)),
      cfg_(cfg) {
    bytes_total_.store(remote_->size());
}

void Downloader::start() {
    status_.store(static_cast<int>(Status::kWaiting));
    elio::go([this]() -> elio::coro::task<void> { return run(); }());
}

elio::coro::task<void> Downloader::run() {
    // Start delay: delay_sec + random(0, delay_extra).
    uint32_t delay = cfg_.delay_sec;
    if (cfg_.delay_extra_sec > 0) {
        std::random_device rd;
        delay += rd() % (cfg_.delay_extra_sec + 1);
    }
    ELIO_LOG_INFO("download of {} scheduled in {}s", dir_, delay);
    co_await elio::time::sleep_for(std::chrono::seconds(delay));

    uint64_t resume_pos = 0;
    for (uint32_t attempt = 0; attempt < cfg_.try_count; ++attempt) {
        status_.store(static_cast<int>(Status::kDownloading));
        int rc = co_await download_once(resume_pos);
        if (rc == 0) {
            status_.store(static_cast<int>(Status::kVerifying));
            rc = co_await verify_and_install();
            if (rc == 0) {
                status_.store(static_cast<int>(Status::kDone));
                ELIO_LOG_INFO("download complete: {}", dir_);
                if (on_complete) {
                    co_await on_complete(target_path(dir_));
                }
                co_return;
            }
            // Verification failed: discard and restart from scratch.
            ::unlink(staging_path(dir_).c_str());
            resume_pos = 0;
        }
        ELIO_LOG_WARNING("download attempt {} for {} failed: {}",
                      attempt + 1, dir_, strerror(rc));
        last_error_.store(rc);
        bytes_done_.store(0);
        resume_pos = 0;
        co_await elio::time::sleep_for(std::chrono::seconds(1));
    }
    status_.store(static_cast<int>(Status::kFailed));
    ELIO_LOG_ERROR("download of {} failed after {} attempts", dir_,
                   cfg_.try_count);
}

elio::coro::task<int> Downloader::download_once(uint64_t& resume_pos) {
    const std::string staging = staging_path(dir_);
    const uint64_t total = remote_->size();

    int fd = ::open(staging.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) co_return errno;
    struct FdGuard {
        int fd;
        ~FdGuard() { ::close(fd); }
    } guard{fd};

    if (::ftruncate(fd, static_cast<off_t>(total)) != 0) {
        co_return errno;
    }

    // Resume: find the first hole (we always write sequentially, so data is
    // a single extent at the front).
    uint64_t pos = 0;
    const off_t hole = ::lseek(fd, 0, SEEK_HOLE);
    if (hole >= 0) {
        pos = static_cast<uint64_t>(hole);
    }
    // (EINVAL/other → filesystem without SEEK_HOLE: restart from 0.)
    if (pos > total) pos = total;
    resume_pos = pos;
    bytes_done_.store(pos);
    if (pos == total) co_return 0;  // previously finished, only verify

    std::vector<uint8_t> buf(cfg_.block_size);
    // Throttle: max_mbps MiB/s via a per-second token window.
    const uint64_t quota =
        static_cast<uint64_t>(cfg_.max_mbps) * 1024 * 1024;
    uint64_t window_bytes = 0;
    auto window_start = std::chrono::steady_clock::now();

    while (pos < total) {
        const size_t chunk =
            static_cast<size_t>(std::min<uint64_t>(cfg_.block_size,
                                                   total - pos));
        const ssize_t r = co_await remote_->pread(buf.data(), chunk, pos);
        if (r < 0) co_return static_cast<int>(-r);
        if (static_cast<size_t>(r) != chunk) co_return EIO;
        size_t written = 0;
        while (written < chunk) {
            const auto w = co_await elio::io::async_write(
                fd, buf.data() + written, chunk - written,
                static_cast<int64_t>(pos + written));
            if (w.result < 0) co_return static_cast<int>(-w.result);
            written += static_cast<size_t>(w.result);
        }
        pos += chunk;
        bytes_done_.store(pos);

        window_bytes += chunk;
        if (window_bytes >= quota) {
            const auto elapsed = std::chrono::steady_clock::now() - window_start;
            if (elapsed < std::chrono::seconds(1)) {
                co_await elio::time::sleep_for(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::seconds(1) - elapsed));
            }
            window_start = std::chrono::steady_clock::now();
            window_bytes = 0;
        }
    }
    co_return 0;
}

elio::coro::task<int> Downloader::verify_and_install() {
    const std::string staging = staging_path(dir_);
    if (!expected_sha256_.empty()) {
        const int fd = ::open(staging.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) co_return errno;
        struct FdGuard {
            int fd;
            ~FdGuard() { ::close(fd); }
        } guard{fd};
        common::Sha256 hash;
        std::vector<uint8_t> buf(1 << 20);
        uint64_t off = 0;
        const uint64_t total = remote_->size();
        while (off < total) {
            const size_t chunk = static_cast<size_t>(
                std::min<uint64_t>(buf.size(), total - off));
            const auto r = co_await elio::io::async_read(
                fd, buf.data(), chunk, static_cast<int64_t>(off));
            if (r.result < 0) co_return static_cast<int>(-r.result);
            if (r.result == 0) co_return EIO;
            hash.update(buf.data(), static_cast<size_t>(r.result));
            off += static_cast<uint64_t>(r.result);
        }
        const std::string actual = hash.final_hex();
        if (actual != expected_sha256_) {
            ELIO_LOG_ERROR("sha256 mismatch for {}: expected {}, got {}",
                           dir_, expected_sha256_, actual);
            co_return EIO;
        }
    } else {
        ELIO_LOG_WARNING("no sha256 digest for {}; skipping verification",
                      dir_);
    }
    if (::rename(staging.c_str(), target_path(dir_).c_str()) != 0) {
        co_return errno;
    }
    co_return 0;
}

}  // namespace obd::source
