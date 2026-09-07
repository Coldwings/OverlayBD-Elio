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
        std::vector<char*> argv;
    } a;
    a.bin = spec.device_bin;
    a.config = spec.config_path;
    a.global = spec.global_path;
    a.ctl_fd = "3";  // see dup2 below: the status channel is always fd 3
    if (spec.dev_id_request >= 0) {
        a.dev_id = std::to_string(spec.dev_id_request);
    }
    a.argv.push_back(a.bin.data());
    a.argv.push_back(a.config_flag.data());
    a.argv.push_back(a.config.data());
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
    a.argv.push_back(nullptr);

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
    int flags = ::fcntl(sv[0], F_GETFL, 0);
    ::fcntl(sv[0], F_SETFL, flags | O_NONBLOCK);

    auto child = std::unique_ptr<Child>(new Child(spec.id));
    child->pid_ = pid;
    child->ctl_fd_ = sv[0];
    return child;
}

Child::~Child() {
    if (ctl_fd_ >= 0) ::close(ctl_fd_);
    if (pid_ > 0) {
        ::kill(pid_, SIGKILL);
        ::waitpid(pid_, nullptr, 0);
    }
}

Child::Status Child::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_;
}

void Child::update_status(const Status& st) {
    bool fire_ready = false;
    bool fire_exit = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_ = st;
        if (!ready_fired_ &&
            (st.state == "ready" || st.state == "failed" ||
             st.state == "exited")) {
            ready_fired_ = true;
            fire_ready = true;
        }
        if (!exit_fired_ && st.state == "exited") {
            exit_fired_ = true;
            fire_exit = true;
        }
    }
    if (fire_ready) ready_event_.set();
    if (fire_exit) exit_event_.set();
}

void Child::terminate() noexcept {
    if (pid_ > 0) ::kill(pid_, SIGTERM);
}

void Child::kill() noexcept {
    if (pid_ > 0) ::kill(pid_, SIGKILL);
}

void Child::note_reaped(int wait_status) {
    Status st = status();
    if (WIFEXITED(wait_status)) {
        st.exit_code = WEXITSTATUS(wait_status);
    } else if (WIFSIGNALED(wait_status)) {
        st.exit_code = 128 + WTERMSIG(wait_status);
    } else {
        st.exit_code = -1;
    }
    // Do not clobber a clean "stopped"/"ready" history with exit detail;
    // the state transitions to "exited" uniformly for bookkeeping.
    st.state = "exited";
    if (st.exit_code == 127 && st.error.empty()) {
        st.error = "exec failed (obd-device not found or not executable)";
    }
    update_status(st);
}

}  // namespace obd::supervisor
