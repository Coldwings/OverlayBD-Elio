// Device-side trace recording control (ADR-0013, record path): the
// obd-device half of the supervisor's trace_start/trace_stop commands.
//
// The supervisor forwards trace commands as JSON-lines on the same
// control socketpair the device reports lifecycle status on (protocol
// v3). This loop reads them, drives the image's TraceRecorder, and
// writes reply lines back with the "reply" discriminator:
//
//   {"reply":"trace_start","ok":true,"path":...,"duration_sec":N,"seq":N}
//   {"reply":"trace_stop","ok":true,"path":...,"sha256":...,"size":N,
//    "records":N,"dropped":N,"seq":N}
//   {"reply":"trace_event","event":"expired", ...same stats...}
//
// Replies echo the command's `seq` when present (the supervisor's
// correlation token); the unsolicited expiry event carries none.
//
// The duration bound is enforced HERE, in the device process (a timer
// armed by TraceRecorder::start) — a dead or disconnected CLI can never
// leak a recording device. Shared by obd-device and the test fake
// device so the wire behavior exists in exactly one place.
#pragma once

#include "image/trace_record.hpp"

#include <elio/coro/task.hpp>

#include <nlohmann/json.hpp>

#include <fcntl.h>

#include <functional>
#include <memory>
#include <mutex>

namespace obd::supervisor {

/// Serializes EVERY writer of the device control channel (fd 3). The
/// lifecycle status report() path, the trace control loop's replies,
/// and the expiry event run in different coroutines — potentially on
/// different scheduler workers — and AF_UNIX SOCK_STREAM offers NO
/// PIPE_BUF atomicity: two concurrent small writes can interleave into
/// one corrupted line. Routing all of them through one shared writer
/// makes every line a single serialized ::write.
class ControlChannelWriter {
public:
    explicit ControlChannelWriter(int fd) : fd_(fd) {
        // Non-blocking: write_line runs on a scheduler worker and must
        // never block it if the supervisor stalls (full socket buffer).
        // A full buffer surfaces as EAGAIN, handled as a dropped line.
        const int fl = ::fcntl(fd, F_GETFL, 0);
        if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    int fd() const { return fd_; }
    /// Best-effort serialized write of one JSON line, LOOPED until all
    /// bytes are out (a single ::write on SOCK_STREAM may write short,
    /// which would truncate/fuse protocol lines). Returns false on a
    /// hard error (logged, dropped — never throws; the supervisor
    /// tolerates loss as a command timeout / EOF).
    bool write_line(const nlohmann::json& j);
    /// Same for a pre-serialized line (the lifecycle status codec
    /// produces a string, not a json object).
    bool write_line(const std::string& line);

private:
    int fd_;
    std::mutex mu_;
};

using ControlChannelWriterPtr = std::shared_ptr<ControlChannelWriter>;

struct TraceControlHooks {
    /// Invoked (coroutine context, best-effort) right after a recording
    /// successfully starts. The test fake uses it to run a scripted
    /// workload inside the recording window; the real device passes
    /// nothing (the workload is guest IO).
    std::function<void()> on_start;
};

/// Serves trace commands from `channel` until EOF (the supervisor
/// closed or died — the device keeps serving its block device; any
/// active recording is still finalized by its duration timer or by the
/// device shutdown path). Never throws: structurally malformed lines
/// are logged and skipped, while parsed commands with missing or
/// wrong-typed fields get a clean error REPLY (the supervisor awaits a
/// reply — a skip would cost it a 30 s timeout). Spawn with elio::go
/// after the image is open.
elio::coro::task<void> run_trace_control(
    ControlChannelWriterPtr channel,
    std::shared_ptr<image::TraceRecorder> recorder,
    TraceControlHooks hooks = {});

}  // namespace obd::supervisor
