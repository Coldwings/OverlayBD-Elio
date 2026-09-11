// Device-side command control. See device_control.hpp.
#include "supervisor/device_control.hpp"

#include "common/errors.hpp"
#include "supervisor/protocol.hpp"

#include <elio/io/io_awaitables.hpp>
#include <elio/log/macros.hpp>
#include <elio/runtime/spawn_blocking.hpp>

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <cerrno>
#include <functional>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace obd::supervisor {

namespace {

/// Buffered JSON-lines reader over the control fd (Elio IO backend);
/// mirrors the supervisor's LineReader.
class LineReader {
public:
    explicit LineReader(int fd) : fd_(fd) {}

    /// Next line without the trailing '\n'; std::nullopt on EOF or error.
    /// An OVERSIZED line (no '\n' within kMaxMessageBytes) is discarded
    /// through its newline and reading continues — it must never be
    /// mistaken for channel EOF, which would end the control loop.
    elio::coro::task<std::optional<std::string>> next() {
        for (;;) {
            if (const auto nl = buf_.find('\n'); nl != std::string::npos) {
                std::string line = buf_.substr(0, nl);
                buf_.erase(0, nl + 1);
                co_return line;
            }
            // Oversize check: strictly > (not >=). A line of exactly
            // kMaxMessageBytes bytes is LEGAL — its '\n' would arrive
            // at position kMaxMessageBytes, making the buffered prefix
            // exactly kMaxMessageBytes with no newline yet. Discarding
            // at >= would kill that legal line. With >, the buffer may
            // briefly hold kMaxMessageBytes + one read chunk (each
            // chunk <= 4096) before the discard triggers — a BOUNDED
            // overshoot, which is all the memory-cap contract needs.
            if (buf_.size() > kMaxMessageBytes) {
                // Skip the oversized line WITHOUT growing buf_ any
                // further (the 64 KiB cap exists to bound memory: an
                // endless no-newline stream must not accumulate).
                // Scan incoming chunks for the terminating '\n' and drop
                // each junk chunk in place; only the bytes trailing the
                // newline are kept.
                for (;;) {
                    char tmp[4096];
                    const auto r = co_await elio::io::async_read(
                        fd_, tmp, sizeof(tmp), -1);
                    if (r.result <= 0) co_return std::nullopt;
                    const auto chunk =
                        std::string_view(tmp, static_cast<size_t>(r.result));
                    const auto nl = chunk.find('\n');
                    if (nl != std::string_view::npos) {
                        // Junk ends here; the rest of the chunk may hold
                        // the start of the next (valid) line.
                        buf_.assign(chunk.substr(nl + 1));
                        break;
                    }
                    // Whole chunk is more junk: drop it, buf_ untouched.
                }
                continue;
            }
            char tmp[4096];
            const auto r =
                co_await elio::io::async_read(fd_, tmp, sizeof(tmp), -1);
            if (r.result <= 0) co_return std::nullopt;
            buf_.append(tmp, static_cast<size_t>(r.result));
        }
    }

private:
    int fd_;
    std::string buf_;
};

struct SyncMutexGuard {
    elio::sync::mutex* mu = nullptr;

    SyncMutexGuard() = default;
    explicit SyncMutexGuard(elio::sync::mutex& m) : mu(&m) {}
    ~SyncMutexGuard() {
        if (mu != nullptr) mu->unlock();
    }

    SyncMutexGuard(const SyncMutexGuard&) = delete;
    SyncMutexGuard& operator=(const SyncMutexGuard&) = delete;
};

}  // namespace

bool ControlChannelWriter::write_line(const nlohmann::json& j) {
    // dump() throws on non-finite numbers and other pathological
    // values; the writer's contract is never-throw (it runs on the
    // detached control path). A pathological payload is dropped.
    std::string line;
    try {
        line = j.dump() + "\n";
    } catch (const std::exception& e) {
        ELIO_LOG_ERROR("cannot serialize control line: {}", e.what());
        return false;
    }
    return write_line(line);
}

bool ControlChannelWriter::write_line(const std::string& line) {
    // SOCK_STREAM has no PIPE_BUF rule: serialization across ALL of the
    // channel's writers (status reports, command replies, the expiry
    // event — different coroutines, different workers) is what keeps
    // one line one write. The lock is held across the WHOLE loop so a
    // short write can never interleave another writer's bytes mid-line.
    std::lock_guard<std::mutex> lk(mu_);
    size_t done = 0;
    while (done < line.size()) {
        // Sockets use send(MSG_NOSIGNAL): EPIPE (supervisor gone) must
        // be an error, never SIGPIPE-kill the device process.
        const ssize_t w =
            is_socket_
                ? ::send(fd_, line.data() + done, line.size() - done,
                         MSG_NOSIGNAL)
                : ::write(fd_, line.data() + done, line.size() - done);
        if (w == 0) {
            // A zero-byte write with bytes pending is not supposed to
            // happen on a stream socket; looping would spin forever.
            ELIO_LOG_ERROR("control channel zero-byte write ({} bytes "
                           "pending); dropping line",
                           line.size() - done);
            return false;
        }
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Non-blocking fd, buffer full: the supervisor is not
                // draining. Drop the remainder of this line rather than
                // block a scheduler worker. Framing consequence (bounded,
                // self-healing): if done > 0, the reader holds a partial
                // prefix that FUSES with the next complete line and is
                // dropped by it as one malformed (non-JSON) line — the
                // line after that parses cleanly again. Loss is bounded
                // to this line plus the one fused line, both tolerable
                // on this best-effort channel (command replies have a
                // 30 s timeout; status is re-queryable).
                ELIO_LOG_WARNING("control channel write would block ({} "
                                 "of {} bytes pending); dropping line",
                                 line.size() - done, line.size());
                return false;
            }
            ELIO_LOG_WARNING("control channel write failed after {} of "
                             "{} bytes: {}",
                             done, line.size(), std::strerror(errno));
            return false;  // logged + dropped, never thrown
        }
        done += static_cast<size_t>(w);
    }
    return true;
}

namespace {

/// L2 correlation: the supervisor stamps a per-command `seq` into every
/// forwarded command; replies echo it so the supervisor can drop a LATE
/// reply to a timed-out command instead of completing the wrong one.
/// Additive: absent seq (an older supervisor) means no echo.
void echo_seq(const nlohmann::json& cmd, nlohmann::json& reply) {
    const auto it = cmd.find("seq");
    if (it != cmd.end() && it->is_number_unsigned()) {
        reply["seq"] = it->get<uint64_t>();
    }
}

nlohmann::json finalize_fields(const image::TraceRecorder::FinalizeResult& r) {
    nlohmann::json j;
    j["path"] = r.path;
    j["sha256"] = r.sha256;
    j["size"] = r.size;
    j["records"] = r.records;
    j["dropped"] = r.dropped;
    return j;
}

}  // namespace

std::function<uint64_t(uint64_t)> make_resize_apply(
    std::shared_ptr<std::atomic<bool>> stopping,
    std::shared_ptr<std::mutex> gate,
    std::function<int(uint64_t)> grow_data_plane,
    std::function<uint64_t(uint64_t)> grow_device) {
    // The stopping flag and the kernel grow are required for a real
    // executor; a missing kernel grow would make every apply a no-op, so
    // refuse loudly at construction time rather than silently accepting
    // resizes.
    if (!stopping || !gate || !grow_device) {
        throw std::invalid_argument(
            "make_resize_apply requires a stopping flag, a gate and a "
            "device grow");
    }
    return [stopping, gate, grow_data_plane = std::move(grow_data_plane),
            grow_device = std::move(grow_device)](uint64_t bytes) -> uint64_t {
        // The gate is held across the whole grow: the shutdown path sets
        // `stopping` and then drains the gate, so a grow either finishes
        // completely BEFORE the shutdown checkpoint runs or is rejected
        // here — it can never interleave with the checkpoint's trailer
        // write (see the header).
        std::lock_guard<std::mutex> grow_guard(*gate);
        // Shutdown guard (see the header): once the device began its
        // graceful shutdown, the shutdown checkpoint has (or is about
        // to have) written its trailer at the CURRENT size — growing
        // the layer now would rewrite the on-disk header to a different
        // size and make the checkpoint pair unsealable. Reject without
        // touching either grow.
        if (stopping->load(std::memory_order_acquire)) {
            throw obd::error(ECANCELED,
                             "device is shutting down; resize ignored");
        }
        if (grow_data_plane) {
            const int r = grow_data_plane(bytes);
            if (r != 0) {
                throw obd::error(
                    -r,
                    "resize failed: writable data plane grow returned " +
                        std::to_string(r));
            }
        }
        return grow_device(bytes);
    };
}

elio::coro::task<void> run_device_control(
    ControlChannelWriterPtr channel,
    std::shared_ptr<image::TraceRecorder> recorder,
    DeviceControlHooks hooks) {
    // The channel is required by every path below; dereferencing a null
    // here would crash the detached coroutine. Refuse loudly instead.
    // `recorder` may be null on devices without one (read-only or
    // recorder-less images): trace commands then get a clean error
    // reply, while resize still works (it needs no recorder).
    if (!channel) {
        ELIO_LOG_ERROR("run_device_control requires a channel; refusing "
                       "to run");
        co_return;
    }
    LineReader reader(channel->fd());
    for (;;) {
        auto line = co_await reader.next();
        if (!line) co_return;  // supervisor closed or died
        nlohmann::json j = nlohmann::json::parse(*line, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("cmd") ||
            !j["cmd"].is_string()) {
            ELIO_LOG_WARNING("malformed device control line ignored");
            continue;
        }
        const std::string cmd = j["cmd"].get<std::string>();
        if (cmd == "trace_start") {
            // A recorder-less device (or one mid-teardown) cannot record:
            // answer cleanly instead of dereferencing a null recorder.
            if (!recorder) {
                nlohmann::json rj = {{"reply", "trace_start"},
                                     {"ok", false},
                                     {"error",
                                      "trace recording unavailable on this "
                                      "device"}};
                echo_seq(j, rj);
                channel->write_line(rj);
                continue;
            }
            // Field TYPES are validated, never assumed: value() on a
            // present-but-wrong-typed key throws type_error, and this
            // loop must answer malformed input, not die on it (the
            // never-throws contract — an escaping exception would kill
            // the detached control coroutine).
            const auto pit = j.find("path");
            const auto dit = j.find("duration_sec");
            // Strictly an INTEGER in the recorder's duration bound
            // BEFORE any get<>(): a float would truncate silently
            // (1.5 -> 1) and a negative/huge integer wraps in
            // get<uint32_t> (2^40 + 300 would alias to 300). The bound
            // constants come from the recorder — single source.
            // Signed JSON integers are read SIGNED first so a negative
            // is rejected explicitly instead of depending on unsigned-
            // conversion wrap; the recorder's bound constants are the
            // single source for the accepted range.
            uint32_t duration = 0;
            if (dit != j.end() && dit->is_number()) {
                int64_t v = -1;
                if (dit->is_number_unsigned()) {
                    const uint64_t u = dit->get<uint64_t>();
                    if (u <= uint64_t{image::TraceRecorder::
                                      kMaxDurationSec}) {
                        v = static_cast<int64_t>(u);
                    }
                } else if (dit->is_number_integer()) {
                    v = dit->get<int64_t>();
                }
                if (v >= int64_t{image::TraceRecorder::kMinDurationSec} &&
                    v <= int64_t{image::TraceRecorder::kMaxDurationSec}) {
                    duration = static_cast<uint32_t>(v);
                }
            }
            if (pit == j.end() || !pit->is_string() || duration == 0) {
                // Bounds come from the recorder (single source of truth);
                // do not hard-code a second copy that could drift.
                const std::string bound_msg =
                    "trace_start requires a string path and an integer "
                    "duration_sec in [" +
                    std::to_string(image::TraceRecorder::kMinDurationSec) +
                    ", " +
                    std::to_string(image::TraceRecorder::kMaxDurationSec) +
                    "]";
                nlohmann::json rj = {{"reply", "trace_start"},
                                     {"ok", false},
                                     {"error", bound_msg}};
                echo_seq(j, rj);
                channel->write_line(rj);
                continue;
            }
            std::optional<SyncMutexGuard> trace_start_guard;
            if (hooks.trace_start_gate) {
                co_await hooks.trace_start_gate->lock();
                trace_start_guard.emplace(*hooks.trace_start_gate);
            }
            if (hooks.trace_start_stopping &&
                hooks.trace_start_stopping->load(
                    std::memory_order_acquire)) {
                nlohmann::json rj = {
                    {"reply", "trace_start"},
                    {"ok", false},
                    {"error",
                     "device is shutting down; trace_start ignored"}};
                echo_seq(j, rj);
                channel->write_line(rj);
                continue;
            }
            const std::string path = pit->get<std::string>();
            std::string error;
            // The expiry report rides the same control channel.
            auto on_expire = [channel](
                                 const image::TraceRecorder::FinalizeResult&
                                     r) {
                nlohmann::json ev = finalize_fields(r);
                ev["reply"] = "trace_event";
                ev["event"] = "expired";
                ev["ok"] = r.ok;
                if (!r.ok) ev["error"] = r.error;
                channel->write_line(ev);
            };
            const bool started = co_await recorder->start(
                path, duration, std::move(on_expire), error);
            nlohmann::json rj;
            if (started) {
                rj = {{"reply", "trace_start"},
                      {"ok", true},
                      {"path", path},
                      {"duration_sec", duration}};
            } else {
                rj = {{"reply", "trace_start"},
                      {"ok", false},
                      {"error", error}};
            }
            echo_seq(j, rj);
            channel->write_line(rj);
            if (started && hooks.on_start) {
                // A hook throwing would kill the detached control
                // coroutine and strand every later command until the
                // 30 s supervisor timeout. Guard it.
                try {
                    hooks.on_start();
                } catch (const std::exception& e) {
                    ELIO_LOG_WARNING("trace_start on_start hook threw: {}",
                                     e.what());
                }
            }
        } else if (cmd == "trace_stop") {
            if (!recorder) {
                nlohmann::json rj = {{"reply", "trace_stop"},
                                     {"ok", false},
                                     {"error",
                                      "trace recording unavailable on this "
                                      "device"}};
                echo_seq(j, rj);
                channel->write_line(rj);
                continue;
            }
            auto res = co_await recorder->stop("stopped");
            nlohmann::json rj;
            if (res.ok) {
                rj = finalize_fields(res);
                rj["reply"] = "trace_stop";
                rj["ok"] = true;
            } else {
                rj = {{"reply", "trace_stop"},
                      {"ok", false},
                      {"error", res.error}};
            }
            echo_seq(j, rj);
            channel->write_line(rj);
        } else if (cmd == "resize") {
            // D3 grow-only online resize (ADR-0014 dev_size model). The
            // GROW-ONLY rule is enforced HERE, against the executor's
            // current-size seam, BEFORE any kernel IO: a request <= the
            // current size is a shrink/no-op and is rejected with a
            // clean error reply (never an exception — the never-throws
            // contract, and the supervisor awaits a reply).
            const ResizeExecutor& ex = hooks.resize;
            const auto bad = [&](const std::string& msg) {
                nlohmann::json rj = {{"reply", "resize"},
                                     {"ok", false},
                                     {"error", msg}};
                echo_seq(j, rj);
                channel->write_line(rj);
            };
            if (!ex.apply_resize || !ex.current_size) {
                bad("resize unsupported on this device");
                continue;
            }
            // `size` is a byte count. Field TYPES are validated, never
            // assumed (value() would throw type_error): accept unsigned
            // or non-negative signed integers only — floats, negatives
            // and strings all get a clean error.
            uint64_t requested = 0;
            const auto zit = j.find("size");
            bool size_ok = false;
            if (zit != j.end() && zit->is_number_unsigned()) {
                requested = zit->get<uint64_t>();
                size_ok = true;
            } else if (zit != j.end() && zit->is_number_integer() &&
                       zit->get<int64_t>() >= 0) {
                requested = static_cast<uint64_t>(zit->get<int64_t>());
                size_ok = true;
            }
            if (!size_ok) {
                bad("resize requires a non-negative integer 'size' (bytes)");
                continue;
            }
            if (requested == 0 || requested % 512 != 0) {
                bad("resize size must be a positive multiple of 512 bytes");
                continue;
            }
            uint64_t current = 0;
            try {
                current = ex.current_size();
            } catch (const std::exception& e) {
                bad(std::string("cannot read current device size: ") +
                    e.what());
                continue;
            }
            if (requested <= current) {
                bad("resize rejected: grow-only (requested " +
                    std::to_string(requested) + " <= current " +
                    std::to_string(current) + " bytes)");
                continue;
            }
            // The executor's apply does the DATA-PLANE grow (blocking
            // header/truncate IO) and then issues the ublk UPDATE_SIZE —
            // a BLOCKING control-plane call that may sleep on our own
            // data plane, never on an Elio worker, hence the
            // spawn_blocking below (ublk control-plane rule; the
            // executor may additionally re-check grow-only as defense in
            // depth). Exceptions (a failed data-plane grow, a rejected
            // UPDATE_SIZE, or the executor's own "device is shutting
            // down; resize ignored" rejection, which protects the
            // post-checkpoint header/trailer consistency) become clean
            // error replies.
            uint64_t applied = 0;
            try {
                applied = co_await elio::spawn_blocking(
                    [&]() { return ex.apply_resize(requested); });
            } catch (const std::exception& e) {
                bad(std::string("resize failed: ") + e.what());
                continue;
            }
            nlohmann::json rj = {{"reply", "resize"},
                                 {"ok", true},
                                 {"size", applied}};
            echo_seq(j, rj);
            channel->write_line(rj);
        } else {
            nlohmann::json rj = {{"reply", cmd},
                                 {"ok", false},
                                 {"error", "unknown device command"}};
            echo_seq(j, rj);
            channel->write_line(rj);
        }
    }
}

}  // namespace obd::supervisor
