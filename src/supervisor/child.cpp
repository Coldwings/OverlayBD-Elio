// Child device process lifecycle. See child.hpp.
#include "supervisor/child.hpp"

#include "common/errors.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <new>
#include <vector>

extern char** environ;

namespace obd::supervisor {

std::unique_ptr<Child> Child::spawn(const ChildSpec& spec) {
    // Everything allocated/derived BEFORE fork: after fork the child runs
    // only async-signal-safe calls until execve (the parent process hosts a
    // live Elio runtime; anything else is UB).
    struct Argv {
        std::string bin;
        std::string config_flag = "--config";
        std::string config;
        std::string global_flag = "--global";
        std::string global;
        std::string ctl_flag = "--control-fd";
        std::string ctl_fd;
        std::string devid_flag = "--dev-id";
        std::string dev_id;
        std::string recover_flag = "--recover";
        std::string vsize_flag = "--virtual-size";
        std::string vsize;
        std::string blank_size_flag = "--blank-size";
        std::string blank_size;
        std::string blank_dir_flag = "--blank-dir";
        std::string blank_dir;
        std::vector<char*> argv;
    } a;
    a.bin = spec.device_bin;
    a.config = spec.config_path;
    a.global = spec.global_path;
    a.ctl_fd = "3";  // see dup2 below: the status channel is always fd 3
    if (spec.dev_id_request >= 0) {
        a.dev_id = std::to_string(spec.dev_id_request);
    }
    if (spec.virtual_size > 0) {
        a.vsize = std::to_string(spec.virtual_size);
    }
    a.argv.push_back(a.bin.data());
    if (spec.blank) {
        // ADR-0014 modes 2/3: no --config; the device assembles the blank
        // stack from the size and its per-device workspace.
        a.blank_size = std::to_string(spec.blank_size);
        a.blank_dir = spec.blank_dir;
        a.argv.push_back(a.blank_size_flag.data());
        a.argv.push_back(a.blank_size.data());
        a.argv.push_back(a.blank_dir_flag.data());
        a.argv.push_back(a.blank_dir.data());
    } else {
        a.argv.push_back(a.config_flag.data());
        a.argv.push_back(a.config.data());
    }
    // --global applies to BOTH modes (a blank device still reads
    // ublkConfig.enableRecovery and the other daemon-wide knobs from
    // overlaybd.json); the ordering matches docs/supervisor.md's argv
    // contract.
    if (!a.global.empty()) {
        a.argv.push_back(a.global_flag.data());
        a.argv.push_back(a.global.data());
    }
    a.argv.push_back(a.ctl_flag.data());
    a.argv.push_back(a.ctl_fd.data());
    if (!a.dev_id.empty()) {
        a.argv.push_back(a.devid_flag.data());
        a.argv.push_back(a.dev_id.data());
    }
    if (!a.vsize.empty()) {
        a.argv.push_back(a.vsize_flag.data());
        a.argv.push_back(a.vsize.data());
    }
    if (spec.recover) {
        a.argv.push_back(a.recover_flag.data());
    }
    a.argv.push_back(nullptr);

    // Construct all ownership storage before creating a channel or process.
    // In particular, copying the ID can throw as well as allocating Child.
#ifdef OBD_TEST_HOOKS
    if (spec.spawn_test_hooks && spec.spawn_test_hooks->before_owner_allocation) {
        spec.spawn_test_hooks->before_owner_allocation();
    }
#endif
    auto child = std::unique_ptr<Child>(new Child(spec.id));

    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        throw_errno(errno, "socketpair for device child failed");
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        const int e = errno;
        ::close(sv[0]);
        ::close(sv[1]);
        throw_errno(e, "fork for device child failed");
    }
    if (pid == 0) {
        // Child: async-signal-safe section only.
        ::close(sv[0]);
        if (sv[1] != 3) {
            ::dup2(sv[1], 3);
            ::close(sv[1]);
        }
        ::fcntl(3, F_SETFD, 0);  // survive execve
        ::execve(a.bin.c_str(), a.argv.data(), environ);
        _exit(127);  // exec failed; parent sees EOF + exit code 127
    }

    ::close(sv[1]);
    // Nonthrowing parent handoff precedes every fallible parent operation.
    child->pid_ = pid;
    child->ctl_fd_ = sv[0];
    int flags = ::fcntl(sv[0], F_GETFL, 0);
    ::fcntl(sv[0], F_SETFL, flags | O_NONBLOCK);

#ifdef OBD_TEST_HOOKS
    if (spec.spawn_test_hooks && spec.spawn_test_hooks->parent_forked) {
        spec.spawn_test_hooks->parent_forked(pid, sv[0]);
    }
#endif
    return child;
}

Child::~Child() {
    if (ctl_fd_ >= 0) ::close(ctl_fd_);
    std::lock_guard<std::mutex> lk(mu_);
    if (pid_ > 0 && !reaped_) {
        ::kill(pid_, SIGKILL);
        while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
    }
}

Child::Status Child::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_;
}

void Child::update_status(const Status& st) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        // A monitor may have parsed a buffered status before the reaper
        // collected the process. Never restore its stale state/exit_code.
        if (!reaped_) {
            st_ = st;
        }
        if (st.state == "ready" || st.state == "failed" ||
            st.state == "exited") {
            ready_reached_ = true;
        }
        if (st.state == "exited") {
            exit_reached_ = true;
        }
    }
    try_notify_waiters();
}

void Child::terminate() noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    if (pid_ > 0 && !reaped_) ::kill(pid_, SIGTERM);
}

void Child::kill() noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    if (pid_ > 0 && !reaped_) ::kill(pid_, SIGKILL);
}

bool Child::reaped() const {
    std::lock_guard<std::mutex> lk(mu_);
    return reaped_;
}

bool Child::try_reap() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (pid_ <= 0 || reaped_) return false;
        int wait_status = 0;
        pid_t result;
        do {
            result = ::waitpid(pid_, &wait_status, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result != pid_) return false;
        // Keep collection and publication inside the signal lock: the PID
        // can be recycled as soon as waitpid returns, before any separate
        // note_reaped call could protect terminate()/kill().
        reaped_ = true;
        ready_reached_ = true;
        exit_reached_ = true;
        st_.state = "exited";
        if (WIFEXITED(wait_status)) {
            st_.exit_code = WEXITSTATUS(wait_status);
        } else if (WIFSIGNALED(wait_status)) {
            st_.exit_code = 128 + WTERMSIG(wait_status);
        }
        if (st_.exit_code == 127 && st_.error.empty()) {
            st_.error = "exec failed (obd-device not found or not executable)";
        }
    }
    try_notify_waiters();
    return true;
}

bool Child::try_notify_waiters() {
    bool ready;
    bool exited;
    bool reaped;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ready = ready_reached_;
        exited = exit_reached_;
        reaped = reaped_;
    }
    bool complete = true;
    auto signal = [&](elio::sync::event& event, bool reached) {
        if (!reached || event.is_set()) return;
        try {
            event.set();
        } catch (const std::bad_alloc&) {
            // Native set fails before signalling/claiming waiters. Keep
            // the sticky fact and let the retained coroutine owner retry.
            complete = false;
        }
    };
    signal(reaped_event_, reaped);
    signal(ready_event_, ready);
    signal(exit_event_, exited);
    return complete;
}

}  // namespace obd::supervisor
