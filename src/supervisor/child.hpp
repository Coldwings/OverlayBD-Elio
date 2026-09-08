// Child device process lifecycle (ADR-0004: one isolated process per block
// device; a device crash never takes down its siblings).
//
// Spawning contract: socketpair(STREAM) + fork + execve (no PATH lookup —
// the supervisor resolves the obd-device binary itself). Between fork and
// execve the child performs ONLY async-signal-safe operations (dup2,
// close, execve, _exit) — the Elio runtime in the parent makes any other
// child-side work unsafe.
#pragma once

#include <elio/sync/event.hpp>

#include <sys/types.h>

#include <memory>
#include <mutex>
#include <string>

namespace obd::supervisor {

struct ChildSpec {
    std::string id;
    std::string device_bin;   // absolute path to obd-device
    std::string config_path;  // per-image config.json
    std::string global_path;  // overlaybd.json (may be empty = device default)
    int dev_id_request = -1;
    /// ADR-0010: spawn in recovery mode (obd-device --recover): attach to
    /// the existing dev_id device instead of creating a new one.
    bool recover = false;
};

class Child {
public:
    /// Forks and execs the device process. Throws obd::error on failure
    /// (fork/socketpair errors, or the child reporting an early exec
    /// failure through the closed pipe).
    static std::unique_ptr<Child> spawn(const ChildSpec& spec);

    ~Child();  // best-effort SIGKILL + reap if still alive
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
    };

    Status status() const;
    /// Called by the monitor/reaper coroutines; publishes the status and
    /// wakes waiters once the state is terminal for creation purposes
    /// (ready/failed/exited).
    void update_status(const Status& st);

    /// Fires when the child first reaches ready/failed/exited.
    elio::sync::event& ready_event() noexcept { return ready_event_; }
    /// Fires when the child has exited (reaped or control channel EOF).
    elio::sync::event& exit_event() noexcept { return exit_event_; }

    void terminate() noexcept;  // SIGTERM
    void kill() noexcept;       // SIGKILL
    /// Records a reap result (waitpid). Marks exited.
    void note_reaped(int wait_status);

private:
    explicit Child(std::string id) : id_(std::move(id)) {}

    std::string id_;
    pid_t pid_ = -1;
    int ctl_fd_ = -1;

    mutable std::mutex mu_;
    Status st_;
    bool ready_fired_ = false;
    bool exit_fired_ = false;
    elio::sync::event ready_event_;
    elio::sync::event exit_event_;
};

}  // namespace obd::supervisor
