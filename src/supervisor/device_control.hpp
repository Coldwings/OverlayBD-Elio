// Device-side command control: the obd-device half of the supervisor's
// forwarded device commands (ADR-0013 trace record path + D3 resize).
//
// The supervisor forwards commands as JSON-lines on the same control
// socketpair the device reports lifecycle status on (protocol v4). This
// loop is the device's single reader of that channel; it drives the
// image's TraceRecorder, executes D3 resizes through the caller-supplied
// executor seam, and writes reply lines back with the "reply"
// discriminator:
//
//   {"reply":"trace_start","ok":true,"path":...,"duration_sec":N,"seq":N}
//   {"reply":"trace_stop","ok":true,"path":...,"sha256":...,"size":N,
//    "records":N,"dropped":N,"seq":N}
//   {"reply":"trace_event","event":"expired", ...same stats...}
//   {"reply":"resize","ok":true,"size":<new bytes>,"seq":N}
//   {"reply":"resize","ok":false,"error":"...","seq":N}
//
// Replies echo the command's `seq` when present (the supervisor's
// correlation token); the unsolicited expiry event carries none.
//
// The trace duration bound is enforced HERE, in the device process (a
// timer armed by TraceRecorder::start) — a dead or disconnected CLI can
// never leak a recording device. The D3 resize is GROW-ONLY, enforced
// HERE against the executor's current-size seam before any kernel IO
// (shrink/no-op attempts get a clean error reply; see resize_blocking
// in the ublk Device for the second, defense-in-depth check). Shared by
// obd-device and the test fake device so the wire behavior exists in
// exactly one place.
#pragma once

#include "image/trace_record.hpp"

#include <elio/coro/task.hpp>
#include <elio/sync/mutex.hpp>

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/stat.h>

#include <atomic>
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
        // Sockets write via ::send(MSG_NOSIGNAL): an EPIPE (supervisor
        // gone mid-write) must surface as an error, NOT SIGPIPE-kill the
        // device process mid-finalize. Pipes (test harness) use ::write.
        struct stat st {};
        is_socket_ = ::fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode);
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
    bool is_socket_ = false;
    std::mutex mu_;
};

using ControlChannelWriterPtr = std::shared_ptr<ControlChannelWriter>;

/// D3 resize executor seam: how THIS device (real obd-device or test
/// fake) serves a `resize` command. All members are optional — an
/// executor without `apply_resize` answers resize with a clean
/// "unsupported" error. `current_size` (bytes) is the grow-only
/// baseline the loop compares the request against BEFORE any kernel IO;
/// `apply_resize(requested_bytes)` performs the grow and returns the
/// resulting size in bytes, throwing on failure. The loop runs
/// `apply_resize` via elio::spawn_blocking, per the ublk control-plane
/// rule — on the real device the executor first grows the writable DATA
/// PLANE (merged view + writable top; a blocking layer grow, which is
/// why it must run on that same pool thread) and then issues the ublk
/// UPDATE_SIZE.
struct ResizeExecutor {
    std::function<uint64_t()> current_size;
    std::function<uint64_t(uint64_t)> apply_resize;
};

struct DeviceControlHooks {
    /// Invoked (coroutine context, best-effort) right after a recording
    /// successfully starts. The test fake uses it to run a scripted
    /// workload inside the recording window; the real device passes
    /// nothing (the workload is guest IO).
    std::function<void()> on_start;
    /// Graceful-shutdown trace-start admission guard. The device
    /// shutdown path sets `trace_start_stopping`, takes and releases
    /// `trace_start_gate`, then calls TraceRecorder::stop("shutdown").
    /// A trace_start that already passed admission finishes before that
    /// drain returns; every later trace_start gets a clean
    /// "device is shutting down" reply instead of arming a timer after
    /// the shutdown stop barrier.
    std::shared_ptr<std::atomic<bool>> trace_start_stopping;
    std::shared_ptr<elio::sync::mutex> trace_start_gate;
    /// D3 resize executor (null/empty members = resize unsupported).
    ResizeExecutor resize;
};

/// Builds the D3 `apply_resize` implementation shared by the real device
/// (and anything else wiring a resize seam), so its ordering and its
/// shutdown contract are testable without a device or kernel:
///
///   1. If `stopping` is set, throw obd::error(ECANCELED, "device is
///      shutting down; resize ignored") WITHOUT calling either grow.
///      This guard is load-bearing: a resize landing after the
///      graceful-shutdown began would otherwise rewrite the writable
///      layer's on-disk declared-size header AFTER the shutdown
///      checkpoint wrote its trailer at the old size — the mismatch
///      makes open_checkpointed reject the pair and leaves the upper
///      UNCOMMITTABLE.
///   2. Grow the writable data plane via `grow_data_plane` (returns
///      -errno; 0 = ok, including the idempotent equal-size no-op). May
///      be null for a read-only image (nothing to grow). A failure
///      throws before any kernel IO.
///   3. Grow the kernel device via `grow_device` (throws on failure;
///      returns the new size in bytes).
///
/// `gate` (non-null) is held for the whole of 1–3, which makes the
/// guard airtight for a grow already IN FLIGHT when the shutdown
/// begins: the shutdown path sets `stopping` and then takes and
/// releases `gate` once, which waits for that in-flight apply to finish
/// and blocks every later one — only then may it stop the device and
/// checkpoint. Without the gate, an apply could pass the flag check
/// just before it was set and still race the checkpoint's trailer
/// write.
///
/// Both callables run on the caller's thread — the device command loop
/// invokes apply_resize inside elio::spawn_blocking, and both grows are
/// BLOCKING by design (docs/supervisor.md, docs/ublk.md).
std::function<uint64_t(uint64_t)> make_resize_apply(
    std::shared_ptr<std::atomic<bool>> stopping,
    std::shared_ptr<std::mutex> gate,
    std::function<int(uint64_t)> grow_data_plane,
    std::function<uint64_t(uint64_t)> grow_device);

/// Serves device commands from `channel` until EOF (the supervisor
/// closed or died — the device keeps serving its block device; any
/// active recording is still finalized by its duration timer or by the
/// device shutdown path). `recorder` may be null on devices without a
/// recorder; trace commands then get a clean "unavailable" error reply.
/// Never throws: structurally malformed lines are logged and skipped,
/// while parsed commands with missing or wrong-typed fields get a clean
/// error REPLY (the supervisor awaits a reply — a skip would cost it a
/// 30 s timeout). Spawn with elio::go after the image is open.
elio::coro::task<void> run_device_control(
    ControlChannelWriterPtr channel,
    std::shared_ptr<image::TraceRecorder> recorder,
    DeviceControlHooks hooks = {});

}  // namespace obd::supervisor
