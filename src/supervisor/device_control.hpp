// Device-side trace recording control (ADR-0013, record path): the
// obd-device half of the supervisor's trace_start/trace_stop commands.
//
// The supervisor forwards trace commands as JSON-lines on the same
// control socketpair the device reports lifecycle status on (protocol
// v3). This loop reads them, drives the image's TraceRecorder, and
// writes reply lines back with the "reply" discriminator:
//
//   {"reply":"trace_start","ok":true,"path":...,"duration_sec":N}
//   {"reply":"trace_stop","ok":true,"path":...,"sha256":...,"size":N,
//    "records":N,"dropped":N}
//   {"reply":"trace_event","event":"expired", ...same stats...}
//
// The duration bound is enforced HERE, in the device process (a timer
// armed by TraceRecorder::start) — a dead or disconnected CLI can never
// leak a recording device. Shared by obd-device and the test fake
// device so the wire behavior exists in exactly one place.
#pragma once

#include "image/trace_record.hpp"

#include <elio/coro/task.hpp>

#include <functional>

namespace obd::supervisor {

struct TraceControlHooks {
    /// Invoked (coroutine context, best-effort) right after a recording
    /// successfully starts. The test fake uses it to run a scripted
    /// workload inside the recording window; the real device passes
    /// nothing (the workload is guest IO).
    std::function<void()> on_start;
};

/// Serves trace commands from `control_fd` until EOF (the supervisor
/// closed or died — the device keeps serving its block device; any
/// active recording is still finalized by its duration timer or by the
/// device shutdown path). Never throws; malformed lines are logged and
/// skipped. Spawn with elio::go after the image is open.
elio::coro::task<void> run_trace_control(
    int control_fd, std::shared_ptr<image::TraceRecorder> recorder,
    TraceControlHooks hooks = {});

}  // namespace obd::supervisor
