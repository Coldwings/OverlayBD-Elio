// Child device process lifecycle (ADR-0004: one isolated process per block
// device; a device crash never takes down its siblings).
//
// Spawning contract: socketpair(STREAM) + fork + execve (no PATH lookup —
// the supervisor resolves the obd-device binary itself). Between fork and
// execve the child performs ONLY async-signal-safe operations (dup2,
// close, execve, _exit) — the Elio runtime in the parent makes any other
// child-side work unsafe.
#pragma once

#include "supervisor/protocol.hpp"
#include "supervisor/reap_test_evidence.hpp"

#include <elio/sync/event.hpp>

#include <sys/types.h>

#include <memory>
#include <mutex>
#include <string>
#ifdef OBD_TEST_HOOKS
#include <functional>
#endif

namespace obd::supervisor {

#ifdef OBD_TEST_HOOKS
struct ChildSpawnTestHooks {
    std::function<void()> before_owner_allocation;
    std::function<void(pid_t, int)> parent_forked;
};
#endif

struct ChildSpec {
    std::string id;
    std::string device_bin;   // absolute path to obd-device
    std::string config_path;  // per-image config.json (blank devices leave
                              // this empty and set `blank` instead)
    std::string global_path;  // overlaybd.json (may be empty = device default)
    int dev_id_request = -1;
    /// Nonzero for the versioned startup-owner handshake. Chosen before fork;
    /// a child cannot publish device commands before its matching grant.
    uint64_t startup_generation = 0;
    /// ADR-0010: spawn in recovery mode (obd-device --recover): attach to
    /// the existing dev_id device instead of creating a new one.
    bool recover = false;
    /// D3 create-time headroom: optional dev_size override in bytes
    /// (0 = derive from the image's declared virtual size). The device
    /// validates it grow-only (>= the image size) before sizing itself.
    /// Unused (0) for a blank create, which sizes itself to blank_size.
    uint64_t virtual_size = 0;
    /// ADR-0014 creation modes 2/3 (blank raw device): when true the
    /// device builds an empty sealed LSMT zero base plus a writable
    /// LSMT-RW upper of `blank_size` bytes inside `blank_dir` instead of
    /// opening an image config (obd-device --blank-size/--blank-dir).
    bool blank = false;
    uint64_t blank_size = 0;  // bytes; positive, sector aligned
    std::string blank_dir;    // per-device workspace (device creates files)
#ifdef OBD_TEST_HOOKS
    // Borrowed only for the synchronous spawn call; never used by the child.
    const ChildSpawnTestHooks* spawn_test_hooks = nullptr;
#endif
};

class Child {
public:
    /// Forks and attempts to exec the device, returning its process owner.
    /// Socketpair/fork errors throw obd::error; preparation may also throw.
    /// Return does not confirm exec or readiness. Exec failure is observed
    /// after collection as exit_code 127, not as a synchronous exception.
    /// Owner storage is constructed before creating the channel or process.
    static std::unique_ptr<Child> spawn(const ChildSpec& spec);

    ~Child();  // Cannot force-kill an unresolved protected startup owner.
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    const std::string& id() const noexcept { return id_; }
    pid_t pid() const noexcept { return pid_; }
    /// Parent end of the status channel (JSON-lines from the device).
    int control_fd() const noexcept { return ctl_fd_; }
    /// Detaches the control fd (monitor coroutine takes ownership).
    int release_control_fd() noexcept {
        const int fd = ctl_fd_;
        ctl_fd_ = -1;
        return fd;
    }

    struct Status {
        std::string state = "starting";  // starting|ready|failed|stopped|exited
        std::string device;
        std::string error;
        int exit_code = -1;  // >= 0 once reaped
        std::optional<std::string> upper_parent_uuid = std::nullopt;
    };

    Status status() const;
    struct Snapshot {
        Status status;
        std::optional<StartupOwnerStatus> startup_owner;
        bool startup_protected = false;
    };
    /// One coherent generation observation for public list/status replies.
    Snapshot snapshot() const;
    /// Called by the monitor/reaper coroutines; publishes the status and
    /// wakes waiters once the state is terminal for creation purposes
    /// (ready/failed/exited). Ignores late updates after an actual reap.
    void update_status(const Status& st);

    /// Fires when the child first reaches ready/failed/exited.
    elio::sync::event& ready_event() noexcept { return ready_event_; }
    /// Fires on control EOF or reap: liveness, not a teardown barrier.
    elio::sync::event& exit_event() noexcept { return exit_event_; }
    /// Process ownership barrier: only actual waitpid collection fires it.
    /// EOF may arrive while device/kernel cleanup is still running.
    elio::sync::event& reaped_event() noexcept { return reaped_event_; }
    bool reaped() const;

    uint64_t startup_generation() const noexcept { return startup_generation_; }
    /// Install BEFORE attempting a grant write. Failed delivery does not undo
    /// protection. Serialized with terminate/kill, not a daemon-side TOCTOU test.
    bool protect_startup(uint64_t generation);
    bool startup_protected() const;
    bool startup_granted() const;
    /// Persist operator authority before serializing/writing the request.
    /// Retries reuse a pending attempt; only an observed refusal advances it.
    std::optional<uint64_t> admit_startup_release();
    /// Accept only current, monotonic ownership facts. A ready handoff also
    /// requires the ordinary lifecycle ready report, not just phase=ready.
    bool observe_startup(const StartupOwnerStatus& owner, bool ready_report);
    std::optional<StartupOwnerStatus> startup_status() const;

    void terminate() noexcept;  // SIGTERM
    void kill() noexcept;       // SIGKILL
    /// Nonblocking owned-PID wait plus terminal publication, serialized
    /// against terminate/kill. Returns true only for a newly collected exit.
    /// Callers must never separately waitpid this Child's PID.
    bool try_reap();
    /// Retry sticky lifecycle notifications without discarding waiters on
    /// dispatch-storage allocation failure. Owners yield and retry false.
    bool try_notify_waiters();

private:
    explicit Child(std::string id) : id_(std::move(id)) {}

    std::string id_;
    pid_t pid_ = -1;
    int ctl_fd_ = -1;
#ifdef OBD_TEST_HOOKS
    detail::ChildReapEvidence reap_evidence_;
#endif

    mutable std::mutex mu_;
    Status st_;
    bool ready_reached_ = false;
    bool exit_reached_ = false;
    bool reaped_ = false;
    uint64_t startup_generation_ = 0;
    bool startup_granted_ = false;
    bool startup_protected_ = false;
    bool startup_ready_ = false;
    bool startup_released_ = false;
    uint64_t startup_release_sequence_ = 0;
    std::optional<StartupOwnerStatus> startup_status_;
    elio::sync::event ready_event_;
    elio::sync::event exit_event_;
    elio::sync::event reaped_event_;
};

}  // namespace obd::supervisor
