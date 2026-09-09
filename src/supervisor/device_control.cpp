// Device-side trace recording control. See device_control.hpp.
#include "supervisor/device_control.hpp"

#include "supervisor/protocol.hpp"

#include <elio/io/io_awaitables.hpp>
#include <elio/log/macros.hpp>

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <cerrno>
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

}  // namespace

bool ControlChannelWriter::write_line(const nlohmann::json& j) {
    return write_line(j.dump() + "\n");
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
        const ssize_t w =
            ::write(fd_, line.data() + done, line.size() - done);
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

elio::coro::task<void> run_trace_control(
    ControlChannelWriterPtr channel,
    std::shared_ptr<image::TraceRecorder> recorder,
    TraceControlHooks hooks) {
    // Both are required by every path below; dereferencing a null here
    // would crash the detached coroutine. Refuse loudly instead.
    if (!channel || !recorder) {
        ELIO_LOG_ERROR("run_trace_control requires a channel and a "
                       "recorder; refusing to run");
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
            if (started && hooks.on_start) hooks.on_start();
        } else if (cmd == "trace_stop") {
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
