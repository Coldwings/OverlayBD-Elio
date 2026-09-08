// The supervisor daemon (ADR-0004). One process supervises one isolated
// obd-device child per block device; obdctl talks to it over a Unix domain
// socket with JSON-lines commands (docs/supervisor.md, protocol.hpp).
#pragma once

#include <elio/coro/task.hpp>

#include <string>

namespace obd::supervisor {

struct DaemonConfig {
    std::string socket_path = "/run/overlaybd-elio/supervisor.sock";
    /// Default overlaybd.json handed to children when a create command
    /// does not specify one.
    std::string global_config = "/etc/overlaybd-elio/overlaybd.json";
    /// obd-device binary; empty = sibling of the supervisor executable.
    std::string device_bin;
    int ready_timeout_sec = 60;   // create: wait for the child's "ready"
    int stop_timeout_sec = 10;    // destroy: SIGTERM grace before SIGKILL
    /// ADR-0010: how many times a crashed device process is replaced via
    /// ublk USER_RECOVERY before the device is left failed. 0 disables
    /// respawn.
    int max_recovery_attempts = 3;
};

/// Runs the daemon until SIGTERM/SIGINT (graceful: children are terminated
/// first). Returns the process exit code. Signals must already be blocked
/// process-wide by the caller (signalfd model).
elio::coro::task<int> run_daemon(const DaemonConfig& cfg);

}  // namespace obd::supervisor
