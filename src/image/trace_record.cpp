// Trace recording. See trace_record.hpp for the design contract.
#include "image/trace_record.hpp"

#include "common/sha256.hpp"

#include <elio/io/io_awaitables.hpp>
#include <elio/log/macros.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/time/timer.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cstring>

namespace obd::image {

TraceRecorder::TraceRecorder(size_t max_pending) : max_pending_(max_pending) {}

void TraceRecorder::record(uint32_t layer_index, uint64_t offset,
                           uint64_t count) noexcept {
    if (!recording()) return;  // fast path: disabled = one atomic load
    if (count == 0) return;
    try {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != State::Recording) return;  // finalized concurrently
        while (count > 0) {
            const uint64_t chunk =
                std::min<uint64_t>(count, format::trace::kMaxRecordCount);
            // Coalescing window: adjacent same-layer tail, merged count
            // stays <= the 1 MiB conforming-writer cap (order preserved
            // — only the tail ever merges).
            if (!pending_.empty()) {
                format::trace::TraceRecord& tail = pending_.back();
                if (tail.layer_index == layer_index &&
                    static_cast<uint64_t>(tail.offset) + tail.count ==
                        offset &&
                    tail.count + chunk <= format::trace::kMaxRecordCount) {
                    tail.count += chunk;
                    offset += chunk;
                    count -= chunk;
                    continue;
                }
            }
            if (pending_.size() >= max_pending_) {
                // one drop per unqueued record chunk
                dropped_.fetch_add(1, std::memory_order_relaxed);
            } else {
                pending_.push_back(
                    format::trace::TraceRecord{format::trace::kOpRead,
                                               layer_index, chunk,
                                               static_cast<int64_t>(offset)});
            }
            offset += chunk;
            count -= chunk;
        }
    } catch (...) {
        // The hot path must never throw into a read coroutine; an
        // allocation failure degrades to a dropped record.
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

elio::coro::task<bool> TraceRecorder::start(
    std::string path, uint32_t duration_sec,
    std::function<void(const FinalizeResult&)> on_expire,
    std::string& error) {
    if (duration_sec < kMinDurationSec || duration_sec > kMaxDurationSec) {
        error = "duration_sec out of bounds [" +
                std::to_string(kMinDurationSec) + ", " +
                std::to_string(kMaxDurationSec) + "]";
        co_return false;
    }
    if (path.empty() || path.front() != '/') {
        error = "trace output path must be absolute";
        co_return false;
    }
    // Gate BEFORE touching the filesystem: a rejected start must not
    // O_TRUNC anything — least of all a previous recording's valid,
    // already-finalized blob whose path the operator reused.
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != State::Idle) {
            error = "trace recording already in progress";
            co_return false;
        }
    }
    // Fail fast on the output path before arming any tap.
    const int fd =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        error = "cannot open trace output " + path + ": " +
                std::strerror(errno);
        co_return false;
    }
    uint64_t generation;
    std::shared_ptr<elio::coro::cancel_source> cancel;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != State::Idle) {
            // Re-check after the open (start-vs-start race): reject
            // Finalizing too — a start slipping into an in-flight
            // finalize would wipe the draining queue (pending_.clear()),
            // corrupt the finalize's stats (dropped_.store(0)), and the
            // old stop's tail would stomp the NEW recording's state
            // back to Idle with its fd/timer live.
            ::close(fd);
            error = "trace recording already in progress";
            co_return false;
        }
        state_ = State::Recording;
        ++generation_;
        generation = generation_;
        pending_.clear();
        dropped_.store(0, std::memory_order_relaxed);
        fd_ = fd;
        path_ = std::move(path);
        on_expire_ = std::move(on_expire);
        last_.reset();
        cancel = std::make_shared<elio::coro::cancel_source>();
        timer_cancel_ = cancel;
    }
    active_.store(true, std::memory_order_release);
    ELIO_LOG_INFO("trace recording to {} for {}s", path_, duration_sec);
    elio::go([this, generation, duration_sec,
              cancel = std::move(cancel)]() -> elio::coro::task<void> {
        co_await run_timer(generation, duration_sec, std::move(cancel));
    });
    co_return true;
}

elio::coro::task<void> TraceRecorder::run_timer(
    uint64_t generation, uint32_t duration_sec,
    std::shared_ptr<elio::coro::cancel_source> cancel) {
    // One cancellable sleep: a stop() cancels it (immediate exit), so
    // the timer never lingers into the scheduler's teardown drain.
    const auto slept = co_await elio::time::sleep_for(
        std::chrono::seconds(duration_sec), cancel->get_token());
    if (slept != elio::coro::cancel_result::completed) {
        co_return;  // stopped or superseded
    }
    std::function<void(const FinalizeResult&)> cb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != State::Recording || generation != generation_) {
            co_return;  // stopped concurrently
        }
        cb = on_expire_;
    }
    // Duration expired: finalize exactly like an explicit stop; the
    // CLI's fate is irrelevant (ADR-0013 server-side bound).
    FinalizeResult res = co_await stop("expired");
    if (cb) cb(res);
    co_return;
}

elio::coro::task<TraceRecorder::FinalizeResult> TraceRecorder::stop(
    std::string reason) {
    // A concurrent stop while another caller is finalizing (an explicit
    // stop racing the duration expiry) WAITS for that finalize and then
    // reports its cached result below — never a spurious "no recording"
    // error. The finalize is a bounded in-memory drain plus one file
    // write, so the poll is short.
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (state_ != State::Finalizing) break;
        }
        co_await elio::time::sleep_for(std::chrono::milliseconds(1));
    }
    int fd;
    std::string path;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != State::Recording) {
            // Idempotent stop: report the cached finalize when there was
            // one (a stop racing the expiry gets the expiry's stats).
            if (last_.has_value()) co_return *last_;
            FinalizeResult res;
            res.error = "no trace recording in progress";
            co_return res;
        }
        state_ = State::Finalizing;
        ++generation_;  // invalidate the duration timer
        fd = fd_;
        fd_ = -1;
        path = path_;
        if (timer_cancel_) timer_cancel_->cancel();  // immediate exit
    }
    active_.store(false, std::memory_order_release);
    FinalizeResult res =
        co_await finalize_locked_state(fd, std::move(path), reason);
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_ = res;
        state_ = State::Idle;
        on_expire_ = nullptr;
    }
    co_return res;
}

elio::coro::task<TraceRecorder::FinalizeResult>
TraceRecorder::finalize_locked_state(int fd, std::string path,
                                     std::string reason) {
    FinalizeResult res;
    res.path = path;
    res.reason = std::move(reason);
    // Drain into the codec's conforming writer: 24xN framing, raw-
    // chaining CRC-32C, zero padding; finalize() rewrites the header
    // checksum (trace-format.md §10).
    format::trace::TraceWriter writer;
    {
        std::lock_guard<std::mutex> lk(mu_);
        res.dropped = dropped_.load(std::memory_order_relaxed);
        for (const auto& rec : pending_) {
            // record() enforces the writer contract at append time, so
            // writer rejects are impossible here.
            if (!writer.append(rec)) {
                ELIO_LOG_WARNING("trace writer rejected a queued record; "
                                 "counted as dropped");
                ++res.dropped;
            }
        }
        pending_.clear();
    }
    // Test-only hook: lets a test hold the finalize open here (state
    // stays Finalizing) to exercise start/stop races.
    std::function<elio::coro::task<void>()> hook;
    {
        std::lock_guard<std::mutex> lk(mu_);
        hook = finalize_hook_;
    }
    if (hook) co_await hook();
    const std::span<const uint8_t> blob = writer.finalize();
    res.records = writer.record_count();
    res.size = blob.size();

    const uint8_t* p = blob.data();
    size_t done = 0;
    while (done < blob.size()) {
        const auto r = co_await elio::io::async_write(
            fd, p + done, blob.size() - done, static_cast<off_t>(done));
        if (r.result < 0) {
            res.error = "trace write failed: " +
                        std::string(std::strerror(
                            static_cast<int>(-r.result)));
            ::close(fd);
            co_return res;
        }
        if (r.result == 0) {
            res.error = "trace write failed: short write";
            ::close(fd);
            co_return res;
        }
        done += static_cast<size_t>(r.result);
    }
    if (::fsync(fd) != 0) {
        res.error = std::string("trace fsync failed: ") +
                    std::strerror(errno);
        ::close(fd);
        co_return res;
    }
    ::close(fd);
    res.sha256 = common::Sha256::hex(blob.data(), blob.size());
    res.ok = true;
    ELIO_LOG_INFO("trace recording finalized ({}): {} records, {} dropped, "
                  "{} bytes, sha256 {}",
                  res.reason, res.records, res.dropped, res.size,
                  res.sha256);
    co_return res;
}

TraceRecordSource::TraceRecordSource(source::BlobSourcePtr inner,
                                     TraceRecorderPtr recorder,
                                     uint32_t layer_index)
    : inner_(std::move(inner)),
      recorder_(std::move(recorder)),
      layer_index_(layer_index) {}

elio::coro::task<ssize_t> TraceRecordSource::pread(void* buf, size_t count,
                                                   uint64_t offset) {
    const ssize_t r = co_await inner_->pread(buf, count, offset);
    // Only FULLY-satisfied remote reads record (ADR-0013): a partial
    // EOF read or an error carries no usable range.
    if (r == static_cast<ssize_t>(count) && count > 0 &&
        recorder_->recording()) {
        // The read range is raw-blob space; the tar header occupies
        // [0, base_). Extent-granular fetches can span the header, so
        // clamp to the payload overlap and record in payload space.
        const uint64_t end = offset + static_cast<uint64_t>(r);
        if (end > base_) {
            const uint64_t payload_off =
                offset > base_ ? offset - base_ : 0;
            recorder_->record(layer_index_, payload_off,
                              end - std::max(offset, base_));
        }
    }
    co_return r;
}

}  // namespace obd::image
