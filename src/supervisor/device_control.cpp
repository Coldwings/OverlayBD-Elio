// Device-side trace recording control. See device_control.hpp.
#include "supervisor/device_control.hpp"

#include "supervisor/protocol.hpp"

#include <elio/io/io_awaitables.hpp>
#include <elio/log/macros.hpp>

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <optional>
#include <string>

namespace obd::supervisor {

namespace {

/// Buffered JSON-lines reader over the control fd (Elio IO backend);
/// mirrors the supervisor's LineReader.
class LineReader {
public:
    explicit LineReader(int fd) : fd_(fd) {}

    /// Next line without the trailing '\n'; std::nullopt on EOF or error.
    elio::coro::task<std::optional<std::string>> next() {
        for (;;) {
            if (const auto nl = buf_.find('\n'); nl != std::string::npos) {
                std::string line = buf_.substr(0, nl);
                buf_.erase(0, nl + 1);
                co_return line;
            }
            if (buf_.size() > kMaxMessageBytes) co_return std::nullopt;
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

/// Best-effort single short write of one reply line to the control fd
/// (same pattern as the lifecycle report(); the supervisor tolerates
/// loss as a command timeout). Well under PIPE_BUF, so concurrent
/// status/report writes cannot interleave mid-line.
void reply_line(int fd, const nlohmann::json& j) {
    const std::string line = j.dump() + "\n";
    const ssize_t w = ::write(fd, line.data(), line.size());
    (void)w;
}

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
    int control_fd, std::shared_ptr<image::TraceRecorder> recorder,
    TraceControlHooks hooks) {
    LineReader reader(control_fd);
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
            const std::string path = j.value("path", "");
            const uint32_t duration = j.value("duration_sec", 0);
            std::string error;
            // The expiry report rides the same control channel.
            auto on_expire = [fd = control_fd](
                                 const image::TraceRecorder::FinalizeResult&
                                     r) {
                nlohmann::json ev = finalize_fields(r);
                ev["reply"] = "trace_event";
                ev["event"] = "expired";
                ev["ok"] = r.ok;
                if (!r.ok) ev["error"] = r.error;
                reply_line(fd, ev);
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
            reply_line(control_fd, rj);
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
            reply_line(control_fd, rj);
        } else {
            nlohmann::json rj = {{"reply", cmd},
                                 {"ok", false},
                                 {"error", "unknown device command"}};
            echo_seq(j, rj);
            reply_line(control_fd, rj);
        }
    }
}

}  // namespace obd::supervisor
