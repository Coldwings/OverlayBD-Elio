// Trace recording (ADR-0013, record path): capture the layer-blob access
// pattern of a running device as an upstream-compatible prefetch trace
// blob (docs/trace-format.md), controlled over the supervisor protocol.
//
// Two pieces:
//
//   * TraceRecordSource — the tap. Image assembly wraps every REMOTE
//     lower's RegistrySource with it (before LayerStore/AdmissionSource
//     consume it). Every pread on a lower's RegistrySource IS a remote
//     read by construction: the LayerStore touches its remote source only
//     on a local miss (local hits produce NO record, per the ADR), and
//     the remote-only chains have no local cache at all. The tap is the
//     narrowest common point of both chains, and it records in the
//     layer-blob offset space the replay side addresses (payload space:
//     the tar base offset learned from TarOffsetSource is subtracted).
//
//   * TraceRecorder — the sink. One per opened image, shared by every
//     tap. record() is the hot path: one atomic load when idle; when
//     recording, a short std::mutex section appends to a bounded
//     in-memory queue (kMaxPendingRecords) with adjacent-record
//     coalescing — no IO, no allocation beyond the bounded deque, so
//     reads never block on recording. Overflow is DROPPED and counted:
//     a dropped-record trace is still a valid, replayable blob, just
//     less complete (trace-format.md §8 tolerates missing ranges — the
//     affected bytes simply become on-demand reads at replay).
//
// Coalescing window (documented contract): only ADJACENT records of the
// same layer merge (prev.offset + prev.count == next.offset), and only
// while the merged count stays <= the conforming-writer cap of 1 MiB
// (trace-format.md §10 rule 4). Merging adjacents preserves replay
// order by construction. Reads larger than 1 MiB are split into
// 1 MiB record chunks at record() time, as the writer contract demands.
//
// Finalize = the conforming-writer contract end to end: the queue
// drains into format::trace::TraceWriter (24xN framing, raw-chaining
// CRC-32C, zero padding), whose finalize() performs the header checksum
// rewrite; the blob is then written to the output file and sha256'd.
#pragma once

#include "format/trace.hpp"
#include "source/blob_source.hpp"

#include <elio/coro/cancel_token.hpp>
#include <elio/coro/task.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace obd::image {

class TraceRecorder {
public:
    /// Bounded pending-record queue (~1.5 MiB of records; matches the
    /// replay record cap in trace_replay.hpp). Overflow drops.
    static constexpr size_t kMaxPendingRecords = 65536;

    /// Duration bound accepted by start(): 1s..1h. Recording targets
    /// container starts (upstream numbers use ~60s windows); the hour
    /// ceiling bounds a stale/mistaken request.
    static constexpr uint32_t kMinDurationSec = 1;
    static constexpr uint32_t kMaxDurationSec = 3600;

    struct FinalizeResult {
        bool ok = false;
        std::string error;     // when !ok
        std::string path;      // output file
        std::string sha256;    // 64 lowercase hex chars
        uint64_t size = 0;     // blob bytes written
        uint64_t records = 0;  // records written (post-coalescing)
        uint64_t dropped = 0;  // records dropped (buffer overflow)
        std::string reason;    // "stopped" | "expired" | "shutdown"
    };

    /// `max_pending` overrides the queue bound (tests).
    explicit TraceRecorder(size_t max_pending = kMaxPendingRecords);

    // --- hot path (tap) -------------------------------------------------

    /// True while a recording window is open. One atomic acquire load —
    /// the entire disabled cost on the read path.
    bool recording() const noexcept {
        return active_.load(std::memory_order_acquire);
    }

    /// Appends one fully-satisfied remote read. noexcept, never blocks on
    /// IO: a short mutex section coalesces into or appends to the bounded
    /// queue; overflow increments the drop counter. count is split into
    /// <= 1 MiB record chunks (conforming-writer MUST, trace-format.md
    /// §10 rule 4). A range the int64 wire offset cannot represent
    /// (offset/count past INT64_MAX) is dropped + counted rather than
    /// silently corrupting into a negative blob offset.
    void record(uint32_t layer_index, uint64_t offset,
                uint64_t count) noexcept;

    // --- control path (device process, coroutine context) ---------------

    /// Opens the output file (fail-fast on path errors), arms the taps,
    /// and starts the device-side duration timer: after duration_sec the
    /// recording finalizes EXACTLY like an explicit stop (the CLI's fate
    /// is irrelevant — the timer lives in the device process) and
    /// `on_expire` receives the result. `on_expire` runs on the timer
    /// coroutine; keep it short (a best-effort control-channel write).
    /// Returns false with `error` set when a recording is in progress
    /// OR still finalizing (a start must never slip into an in-flight
    /// finalize: it would wipe the draining queue, corrupt the
    /// finalize's stats, and strand the new recording's state), when
    /// the duration is out of bounds, or the path is not writable.
    /// Requires a running Elio scheduler. LIFETIME: stop() is the
    /// completion barrier for the duration timer; it cancels a sleeping
    /// timer and waits until the timer coroutine has destroyed its frame
    /// before returning to the caller.
    elio::coro::task<bool> start(
        std::string path, uint32_t duration_sec,
        std::function<void(const FinalizeResult&)> on_expire,
        std::string& error);

    /// Finalizes the current recording (header checksum rewrite via the
    /// codec writer, file write + fsync) and drains the duration timer
    /// coroutine before returning. Idempotent: a stop after the recording
    /// already finalized (explicit stop, expiry, shutdown) returns the
    /// cached result of that finalize. A stop with no recording ever
    /// started returns {!ok, "no trace recording in progress"}.
    elio::coro::task<FinalizeResult> stop(std::string reason);

    /// Stats of the last (or current) finalize; nullopt before any.
    std::optional<FinalizeResult> last_result() const {
        std::lock_guard<std::mutex> lk(mu_);
        return last_;
    }

    /// Test-only: when set, finalize co_awaits this hook after the
    /// queue drain and before the file write, letting a test hold a
    /// finalize open to exercise start/stop races deterministically
    /// (same role as LayerStore's fetch_done_hook_). Coroutine-side;
    /// never set in production.
    void set_finalize_hook_for_test(
        std::function<elio::coro::task<void>()> hook) {
        std::lock_guard<std::mutex> lk(mu_);
        finalize_hook_ = std::move(hook);
    }

    /// Test-only: when set, start() co_awaits this hook between the
    /// output open and the locked state re-check, letting a test make
    /// the start-vs-start race deterministic. Coroutine-side; never
    /// set in production.
    void set_start_hook_for_test(
        std::function<elio::coro::task<void>()> hook) {
        std::lock_guard<std::mutex> lk(mu_);
        start_hook_ = std::move(hook);
    }

    /// Test-only: when set, stop() co_awaits this hook after observing
    /// Recording and before selecting the finalizer owner. This makes the
    /// stop-vs-stop ownership race deterministic without weakening the
    /// production state transition.
    void set_stop_claim_hook_for_test(
        std::function<elio::coro::task<void>()> hook) {
        std::lock_guard<std::mutex> lk(mu_);
        stop_claim_hook_ = std::move(hook);
    }

    /// Test-only: when set, the duration timer co_awaits this hook after
    /// its sleep completes and before it reads recorder state.
    void set_timer_awake_hook_for_test(
        std::function<elio::coro::task<void>()> hook) {
        std::lock_guard<std::mutex> lk(mu_);
        timer_awake_hook_ = std::move(hook);
    }

    /// Test-only: when set, the first caller draining a duration timer
    /// co_awaits this hook after claiming that timer's handle.
    void set_timer_drain_claim_hook_for_test(
        std::function<elio::coro::task<void>()> hook) {
        std::lock_guard<std::mutex> lk(mu_);
        timer_drain_claim_hook_ = std::move(hook);
    }

    /// Test-only: when set, callers waiting for another drain owner
    /// co_await this hook after observing that owner.
    void set_timer_drain_wait_hook_for_test(
        std::function<elio::coro::task<void>()> hook) {
        std::lock_guard<std::mutex> lk(mu_);
        timer_drain_wait_hook_ = std::move(hook);
    }

    /// Test-only: when set, the duration timer co_awaits this hook after
    /// the expiry callback and before its coroutine returns.
    void set_timer_exit_hook_for_test(
        std::function<elio::coro::task<void>()> hook) {
        std::lock_guard<std::mutex> lk(mu_);
        timer_exit_hook_ = std::move(hook);
    }

private:
    enum class State : int { Idle = 0, Recording = 1, Finalizing = 2 };

    struct FinalizeCompletion {
        uint64_t generation = 0;
        std::optional<FinalizeResult> result;
    };

    struct TimerDrain {
        bool draining = false;
        bool drained = false;
    };

    /// The duration timer body: ONE cancellable sleep — a stop/shutdown
    /// cancels it for an immediate exit, so the timer never parks a
    /// multi-minute sleep into the scheduler's teardown drain.
    elio::coro::task<void> run_timer(
        uint64_t generation, uint32_t duration_sec,
        std::shared_ptr<elio::coro::cancel_source> cancel);

    elio::coro::task<FinalizeResult> stop_impl(std::string reason,
                                               bool from_timer);
    elio::coro::task<void> drain_timer_task(
        std::shared_ptr<TimerDrain> drain);

    /// Shared finalize: drain + write + fsync + digest. Caller holds
    /// state transition; returns the filled result (reason set by caller).
    elio::coro::task<FinalizeResult> finalize_locked_state(
        int fd, std::string path, std::string reason);

    const size_t max_pending_;

    std::atomic<bool> active_{false};  // fast path gate (taps)

    mutable std::mutex mu_;  // queue + control state (short sections)
    State state_ = State::Idle;
    uint64_t generation_ = 0;  // invalidated timers compare against this
    std::deque<format::trace::TraceRecord> pending_;
    std::atomic<uint64_t> dropped_{0};
    int fd_ = -1;
    std::string path_;
    std::function<void(const FinalizeResult&)> on_expire_;
    std::optional<FinalizeResult> last_;
    std::shared_ptr<FinalizeCompletion> finalizing_;
    std::shared_ptr<elio::coro::cancel_source> timer_cancel_;
    std::optional<elio::coro::join_handle<void>> timer_task_;
    std::shared_ptr<TimerDrain> timer_drain_;
    std::function<elio::coro::task<void>()> finalize_hook_;      // test-only
    std::function<elio::coro::task<void>()> start_hook_;         // test-only
    std::function<elio::coro::task<void>()> stop_claim_hook_;    // test-only
    std::function<elio::coro::task<void>()> timer_awake_hook_;   // test-only
    std::function<elio::coro::task<void>()> timer_drain_claim_hook_;
    std::function<elio::coro::task<void>()> timer_drain_wait_hook_;
    std::function<elio::coro::task<void>()> timer_exit_hook_;    // test-only
};

using TraceRecorderPtr = std::shared_ptr<TraceRecorder>;

/// The record tap: wraps a lower's remote source; every fully-satisfied
/// pread appends a record (layer-blob payload offset space) to the
/// shared recorder when it is recording. Disabled cost: one atomic load
/// per remote pread. Pass-through for populate/size/label.
class TraceRecordSource final : public source::BlobSource {
public:
    TraceRecordSource(source::BlobSourcePtr inner, TraceRecorderPtr recorder,
                      uint32_t layer_index);

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;
    elio::coro::task<ssize_t> populate(uint64_t offset,
                                       size_t len) override {
        co_return co_await inner_->populate(offset, len);
    }
    uint64_t size() const noexcept override { return inner_->size(); }
    std::string_view label() const noexcept override {
        return inner_->label();
    }

    /// The tar wrapper's base offset (TarOffsetSource::base_offset), set
    /// by image assembly once, right after the tar probe and BEFORE any
    /// read traffic exists — no synchronization needed. Recorded offsets
    /// are (remote offset - base), the payload space replay addresses.
    void set_base(uint64_t base) noexcept { base_ = base; }

private:
    source::BlobSourcePtr inner_;
    TraceRecorderPtr recorder_;
    uint32_t layer_index_;
    uint64_t base_ = 0;
};

}  // namespace obd::image
