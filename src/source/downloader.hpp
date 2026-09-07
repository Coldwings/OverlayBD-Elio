// Background downloader — pulls a remote blob into the per-layer directory
// so subsequent reads (after the atomic switch, see switch_source.hpp) are
// served locally. Mirrors overlaybd's download contract
// (docs/source.md §Download):
//
//   * target file "<dir>/.download", ftruncate'ed sparse to the blob size;
//   * resume via SEEK_HOLE (previous partial downloads are continued);
//   * simple throughput throttle (maxMBps), download.blockSize chunks;
//   * sha256 verified against the config digest on completion — mismatch
//     discards the file and restarts (within tryCnt attempts);
//   * on success, renamed atomically to "<dir>/overlaybd.commit".
#pragma once

#include "source/blob_source.hpp"

#include <elio/coro/task.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace obd::source {

struct DownloadConfig {
    bool enable = false;
    uint32_t delay_sec = 300;        // start delay after device open
    uint32_t delay_extra_sec = 30;   // + random(0, extra)
    uint32_t max_mbps = 100;         // throttle, MiB/s
    uint32_t try_count = 5;
    uint32_t block_size = 256 * 1024;
};

class Downloader {
public:
    enum class Status : int {
        kIdle = 0,
        kWaiting = 1,
        kDownloading = 2,
        kVerifying = 3,
        kDone = 4,
        kFailed = 5,
    };

    /// `remote` is the raw (un-cached) blob source; `dir` the per-layer
    /// directory; `expected_sha256` the hex digest from the image config
    /// (empty = no verification, logged). The completion callback runs on
    /// the scheduler after the file is renamed into place.
    Downloader(BlobSourcePtr remote, std::string dir,
               std::string expected_sha256, DownloadConfig cfg);

    /// Starts the download coroutine. Requires a running Elio scheduler on
    /// the calling thread (elio::go semantics). Returns immediately.
    void start();

    Status status() const noexcept {
        return static_cast<Status>(status_.load());
    }
    int last_error() const noexcept { return last_error_.load(); }
    uint64_t bytes_done() const noexcept { return bytes_done_.load(); }
    uint64_t bytes_total() const noexcept { return bytes_total_.load(); }

    static std::string staging_path(const std::string& dir) {
        return dir + "/.download";
    }
    static std::string target_path(const std::string& dir) {
        return dir + "/overlaybd.commit";
    }

    /// Completion callback: invoked with the target path after rename.
    std::function<elio::coro::task<void>(const std::string&)> on_complete;

private:
    elio::coro::task<void> run();
    elio::coro::task<int> download_once(uint64_t& resume_pos);
    elio::coro::task<int> verify_and_install();

    BlobSourcePtr remote_;
    std::string dir_;
    std::string expected_sha256_;
    DownloadConfig cfg_;

    std::atomic<int> status_{0};
    std::atomic<int> last_error_{0};
    std::atomic<uint64_t> bytes_done_{0};
    std::atomic<uint64_t> bytes_total_{0};
};

}  // namespace obd::source
