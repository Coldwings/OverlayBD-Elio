// LayerStore — sparse-file layer persistence with a sidecar extent map
// (ADR-0011). Wired into image assembly as the read chain for
// dir-configured remote layers; an optional background fill (the download
// config contract) warms the whole layer without readers.
//
// Every remote byte a layer serves is persisted into a sparse local staging
// file, so the layer's dependence on the remote source shrinks monotonically
// and survives restarts. The pieces:
//
//   * one staging file "<dir>/.download.<nonce>" (ftruncate'ed sparse to the
//     blob size) plus one sidecar "<dir>/.bitmap.<nonce>" per fill attempt;
//     the nonce in the file names pairs them, so a stale sidecar can never
//     be attached to a fresh staging file;
//   * a uniform extent granularity (64 KiB default) for remote fetches,
//     persistence accounting, and sidecar records;
//   * per-extent CRC32 verification on local reads: a mismatch demotes the
//     extent to a hole and re-fetches (torn writes become deterministic
//     cache misses, never silent corruption);
//   * a bounded, droppable write-behind queue drained by a dedicated plain
//     std::thread (blocking disk work never occupies an Elio worker);
//   * an optional background fill coroutine: delayed, throttled bulk
//     warming of every missing extent (the ADR-0011 bulk path — contiguous
//     misses are coalesced into larger range reads and split back into
//     extents for accounting);
//   * a bypass state on ENOSPC/EIO: writes stop, fill switches off, reads
//     continue purely remote — a normal degraded mode, not an error path;
//   * completion: when every extent is present, the staging file is
//     sha256-verified against the image-config digest and atomically
//     renamed to "<dir>/overlaybd.commit" (the committed-layer contract
//     the local probe binds).
//
// Lifetime: a LayerStore must not be destroyed while pread/populate
// coroutines or a background fill are in flight on it — a suspended fetch,
// joiner, or fill step touches members on resume (unlike LocalFileSource,
// whose destructor orders the fd close against the io_uring backend). Park
// an active fill first: stop_fill() + fill_status() reaching kDone/kStopped.
#pragma once

#include "source/blob_source.hpp"

#include <elio/coro/task.hpp>
#include <elio/sync/event.hpp>
#include <elio/sync/mutex.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace obd::source {

class LayerStore final : public BlobSource {
public:
    struct Config {
        uint32_t extent_size = 64 * 1024;            // uniform extent unit
        uint64_t queue_max_bytes = 4ULL * 1024 * 1024;  // write-behind bound
        uint32_t try_count = 5;  // completion-verify attempts before giving up

        /// Background fill — the `download` config contract (docs/config.md):
        /// a scavenger-class bulk walk that warms every missing extent
        /// without readers. Fill traffic runs at concurrency 1 (one walk);
        /// the ADR-0012 admission funnel governs it once merged.
        struct Fill {
            bool enable = false;
            uint32_t delay_sec = 300;        // start delay after open
            uint32_t delay_extra_sec = 30;   // plus uniform random 0..extra
            uint32_t max_mbps = 100;         // throughput throttle, MiB/s
            uint32_t block_size = 256 * 1024;  // range-read coalescing cap
        } fill;
    };

    enum class State : int {
        Filling = 0,   // serving remote + persisting into the staging pair
        Complete = 1,  // bound to <dir>/overlaybd.commit, plain local reads
        Bypass = 2,    // persistence broken/exhausted; reads purely remote
    };

    /// Background-fill lifecycle (fill_status()).
    enum class FillStatus : int {
        kDisabled = 0,  // fill not enabled
        kWaiting = 1,   // in the start delay
        kFilling = 2,   // walking and persisting
        kDone = 3,      // walk finished: no missing extent remained
        kStopped = 4,   // left early: bypass, completion, or stop_fill()
    };

    /// Opens (or creates) the persistence state for one layer in `dir`.
    /// `expected_sha256_hex` is the hex digest from the image config (an
    /// optional "sha256:" prefix is stripped; empty means no completion
    /// verification, logged). Throws obd::error on unrecoverable setup
    /// problems (null remote, bad config, missing/unwritable dir, unloadable
    /// commit file, staging-pair creation failure). Requires a running Elio
    /// scheduler. The three-argument overload uses the default Config.
    static elio::coro::task<std::unique_ptr<LayerStore>> open(
        BlobSourcePtr remote, std::string dir, std::string expected_sha256_hex);
    static elio::coro::task<std::unique_ptr<LayerStore>> open(
        BlobSourcePtr remote, std::string dir, std::string expected_sha256_hex,
        Config cfg);

    ~LayerStore() override;

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;

    /// Warms local persistence for [offset, offset+len) without delivering
    /// data: joins-or-starts the fetch of every missing extent in the range
    /// and enqueues the bytes for write-behind. Returns 0, or a negative
    /// -errno from the remote. No-op (0) in Complete and Bypass states.
    elio::coro::task<ssize_t> populate(uint64_t offset, size_t len) override;

    uint64_t size() const noexcept override { return size_; }
    std::string_view label() const noexcept override { return label_; }

    State state() const noexcept {
        return static_cast<State>(state_.load(std::memory_order_acquire));
    }
    uint64_t extents_present() const noexcept {
        return present_.load(std::memory_order_relaxed);
    }
    uint64_t extents_total() const noexcept { return extent_count_; }
    uint64_t dropped_writes() const noexcept {
        return dropped_writes_.load(std::memory_order_relaxed);
    }
    uint64_t crc_failures() const noexcept {
        return crc_failures_.load(std::memory_order_relaxed);
    }
    uint64_t remote_fetches() const noexcept {
        return remote_fetches_.load(std::memory_order_relaxed);
    }
    uint64_t coalesced_joins() const noexcept {
        return coalesced_joins_.load(std::memory_order_relaxed);
    }
    FillStatus fill_status() const noexcept {
        return static_cast<FillStatus>(
            fill_status_.load(std::memory_order_acquire));
    }

    /// Asks the background fill to stop; it exits at the next step (a sleep
    /// or queue wait, within ~1s once past the start delay). Idempotent.
    /// Tests and teardown should poll fill_status() for kDone/kStopped
    /// before destroying the store (see the lifetime contract above).
    void stop_fill() noexcept {
        fill_stop_.store(true, std::memory_order_release);
    }

    /// Test-only hook: invoked by the writer thread before persisting each
    /// queued entry; a non-zero return is treated as a pwrite failure with
    /// that errno (ENOSPC/EIO trigger bypass), and the hook may block to
    /// simulate a slow disk. Not part of the module API.
    void set_test_write_hook(std::function<int(uint64_t extent_id)> hook);

private:
    LayerStore() = default;

    // In-memory mirror of one sidecar record: (crc32 << 32) | flags.
    static constexpr uint64_t kFlagPresent = 1;

    struct InFlight {
        elio::sync::event done;
        std::shared_ptr<const std::vector<uint8_t>> data;  // set on success
        int error = 0;  // positive errno on failure
    };
    struct FetchResult {
        std::shared_ptr<const std::vector<uint8_t>> data;
        int error = 0;  // positive errno
    };

    struct WriteJob {
        uint64_t extent_id = 0;
        std::shared_ptr<const std::vector<uint8_t>> data;  // null for clear
        size_t data_offset = 0;  // extent's bytes begin here inside *data
        size_t len = 0;  // extent payload length (queue accounting unit)
        uint32_t crc = 0;
        bool clear = false;  // demote the sidecar record (CRC-mismatch path)
    };

    // Setup/recovery (cold paths, may throw obd::error).
    // A newly created staging pair, built into locals first and published
    // atomically by the caller, so readers never observe a -1 fd window
    // (see restart_fresh).
    struct FreshPair {
        uint64_t nonce = 0;
        int staging_fd = -1;
        int sidecar_fd = -1;
    };
    int create_fresh_pair(FreshPair& out);  // 0 or errno; touches no members
    uint64_t find_valid_pair();  // nonce of a resumed pair, 0 = start fresh
    bool try_load_pair(uint64_t nonce, const std::string& staging,
                       const std::string& sidecar);

    // Hot-path helpers (coroutines, -errno results, never throw).
    elio::coro::task<ssize_t> read_fd_loop(int fd, void* buf, size_t count,
                                           uint64_t offset);
    elio::coro::task<FetchResult> join_or_fetch(uint64_t extent_id);
    void enqueue_write(uint64_t extent_id,
                       std::shared_ptr<const std::vector<uint8_t>> data,
                       size_t data_offset = 0);
    void enqueue_clear(uint64_t extent_id);

    // Background fill (one coroutine, spawned by open when fill.enable).
    elio::coro::task<void> run_fill();
    // Waits until the write-behind queue has room for `len` more bytes;
    // false when the fill must stop (bypass, teardown, stop_fill).
    elio::coro::task<bool> wait_queue_room(size_t len);

    // Writer thread (the only place blocking disk syscalls run).
    void writer_main();
    void process_job(const WriteJob& job,
                     const std::function<int(uint64_t)>& hook);
    void on_write_error(int err);
    void enter_bypass(int err, const char* what);
    void complete_layer();
    void restart_fresh();

    static std::string hex_nonce(uint64_t nonce);
    static std::string commit_path(const std::string& dir) {
        return dir + "/overlaybd.commit";
    }
    std::string staging_path() const {
        return dir_ + "/.download." + hex_nonce(nonce_);
    }
    std::string sidecar_path() const {
        return dir_ + "/.bitmap." + hex_nonce(nonce_);
    }

    BlobSourcePtr remote_;
    std::string dir_;
    std::string label_;
    std::string expected_hex_;               // empty = skip verification
    std::array<uint8_t, 32> expected_raw_{};  // zero-filled when empty
    Config cfg_;
    uint64_t size_ = 0;
    uint64_t extent_count_ = 0;
    uint64_t nonce_ = 0;

    // O_RDWR; promoted to the commit fd at completion. Atomic: the writer
    // thread exchanges it on a fresh restart while reader coroutines load
    // it — readers always observe a valid fd (the retired-but-open old one
    // or the new sparse file), never -1.
    std::atomic<int> staging_fd_{-1};
    int sidecar_fd_ = -1;  // writer thread only
    std::atomic<int> commit_fd_{-1};  // valid once state == Complete
    std::vector<int> retired_fds_;    // superseded staging fds (writer only)

    std::vector<std::atomic<uint64_t>> records_;  // packed crc|flags mirror

    std::atomic<int> state_{static_cast<int>(State::Filling)};
    std::atomic<uint64_t> present_{0};
    std::atomic<uint64_t> dropped_writes_{0};
    std::atomic<uint64_t> crc_failures_{0};
    std::atomic<uint64_t> remote_fetches_{0};
    std::atomic<uint64_t> coalesced_joins_{0};

    // Extent fetch coalescing (coroutine-side only).
    elio::sync::mutex inflight_mu_;
    std::unordered_map<uint64_t, std::shared_ptr<InFlight>> inflight_;

    // Write-behind queue (plain std::thread + std::mutex/condvar).
    std::thread writer_;
    std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<WriteJob> queue_;
    uint64_t queued_bytes_ = 0;
    bool stopping_ = false;
    std::function<int(uint64_t)> write_hook_;  // test-only, under qmu_
    uint32_t attempts_ = 0;        // completion-verify attempts (writer only)
    bool kick_completion_check_ = false;  // set before the writer starts

    // Background fill (one elio::go coroutine; see the lifetime contract).
    std::atomic<int> fill_status_{static_cast<int>(FillStatus::kDisabled)};
    std::atomic<bool> fill_stop_{false};
};

}  // namespace obd::source
