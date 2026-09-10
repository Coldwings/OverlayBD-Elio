// The supervisor daemon (ADR-0004). One process supervises one isolated
// obd-device child per block device; obdctl talks to it over a Unix domain
// socket with JSON-lines commands (docs/supervisor.md, protocol.hpp).
#pragma once

#include <elio/coro/task.hpp>

#include <sys/types.h>

#include <memory>
#include <string>
#include <vector>

namespace obd::supervisor {

/// Host `mkfs.<type>` runner for ADR-0014 create mode 3 (blank raw device
/// plus a convenience mkfs). The daemon invokes it against the new block
/// device path ONLY when a create command's `blank` object explicitly
/// requests a type — host mkfs output is non-deterministic (UUIDs, hash
/// seeds, timestamps), so it is a runtime convenience and never an
/// image-build input (ADR-0014; docs/operations.md). The default
/// implementation (daemon.cpp) forks `mkfs.<type> <device>` and bounds it
/// by DaemonConfig::mkfs_timeout_sec; tests inject a mock so the suite
/// never runs host mkfs.
class MkfsRunner {
public:
    virtual ~MkfsRunner() = default;
    /// Runs `mkfs.<fs_type>` against `device`. Returns 0 on success; on
    /// failure a positive exit code, negative -errno, or -ETIMEDOUT, with
    /// `error` populated (human readable).
    virtual elio::coro::task<int> run(const std::string& fs_type,
                                      const std::string& device,
                                      std::string* error) = 0;

    /// Pids whose reap `run()` had to ABANDON: the bounded post-SIGKILL
    /// wait expired, so the helper is still dying somewhere. The daemon's
    /// reaper drains this and reaps them with WNOHANG whenever it wakes on
    /// SIGCHLD, which keeps a wedged helper from becoming a zombie that
    /// outlives the device it was formatting — deterministically, without a
    /// detached task or thread that shutdown would have to wait for.
    /// Default (mocks): nothing to hand over.
    virtual std::vector<pid_t> take_orphan_pids() { return {}; }
};
using MkfsRunnerPtr = std::shared_ptr<MkfsRunner>;

/// Factory for the default MkfsRunner (defined in daemon.cpp): forks
/// `mkfs.<type> <device>` resolved on PATH and reaps it with a
/// non-blocking WNOHANG poll bounded by `timeout_sec`, mapping exit
/// codes (127 → "not found or not executable"), signals, and a timeout
/// (SIGKILL, `-ETIMEDOUT`, with the pid handed to the reaper via
/// take_orphan_pids() if the bounded reap expires) into the result.
/// Installed by the daemon when
/// `DaemonConfig::mkfs_runner` is empty; exposed so tests can exercise
/// the REAL runner's mappings without a daemon (and so the runner's
/// child ownership is testable against the daemon's reaper).
MkfsRunnerPtr make_default_mkfs_runner(int timeout_sec);

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
    /// ADR-0014 blank-device workspace root (modes 2/3): each blank device
    /// owns `<blank_dir>/<id>/` with `overlaybd.zero` (the sealed empty
    /// LSMT zero base) and `overlaybd.rw` (the writable upper) inside.
    std::string blank_dir = "/var/lib/overlaybd-elio/devices";
    /// Mode-3 mkfs runner (see MkfsRunner); empty = the default fork/exec
    /// runner with mkfs_timeout_sec. Injectable so tests never run host
    /// mkfs.
    MkfsRunnerPtr mkfs_runner;
    int mkfs_timeout_sec = 300;  // default mode-3 mkfs bound
};

/// Runs the daemon until SIGTERM/SIGINT (graceful: children are terminated
/// first). Returns the process exit code. Signals must already be blocked
/// process-wide by the caller (signalfd model).
elio::coro::task<int> run_daemon(const DaemonConfig& cfg);

}  // namespace obd::supervisor
