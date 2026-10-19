#include "supervisor/child.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

// The failing baseline never returns a Child. Pin the exact forked process
// and socket while the factory still owns them, then recover only those
// resources after observing whether the factory cleaned up its exception.
struct SpawnFailureProbe {
    pid_t pid = -1;
    int original_fd = -1;
    int socket_pin = -1;
    int pidfd = -1;
    struct stat socket_identity {};
    bool identity_known = false;
    bool collected = false;

    void capture(pid_t child, int channel) {
        pid = child;
        original_fd = channel;
        if (::fstat(channel, &socket_identity) != 0) {
            throw std::runtime_error("cannot identify forked status socket");
        }
        identity_known = true;
        pidfd = static_cast<int>(::syscall(SYS_pidfd_open, child, 0));
        if (pidfd < 0) throw std::runtime_error("cannot pin forked child");
        socket_pin = ::fcntl(channel, F_DUPFD_CLOEXEC, 0);
        if (socket_pin < 0) throw std::runtime_error("cannot pin status socket");
    }

    bool original_channel_open() const {
        if (!identity_known) return false;
        struct stat current {};
        return ::fstat(original_fd, &current) == 0 &&
               current.st_dev == socket_identity.st_dev &&
               current.st_ino == socket_identity.st_ino;
    }

    bool factory_collected() {
        if (pid <= 0) return true;
        pid_t result;
        do {
            result = ::waitpid(pid, nullptr, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == pid) {
            collected = true;  // The fixture, rather than the factory, reaped it.
            return false;
        }
        if (result < 0 && errno == ECHILD) {
            collected = true;
            return true;
        }
        return false;
    }

    void cleanup() noexcept {
        if (pid > 0 && !collected) {
            if (pidfd >= 0) {
                ::syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0);
            } else {
                // Capture itself failed before pinning. This synchronous
                // fixture is the sole creator/waiter of this unreaped PID;
                // no registry, monitor or reaper has ever received it.
                ::kill(pid, SIGKILL);
            }
            pid_t result;
            do {
                result = ::waitpid(pid, nullptr, 0);
            } while (result < 0 && errno == EINTR);
            collected = result == pid || (result < 0 && errno == ECHILD);
        }
        // The pinned socket prevents inode reuse. No other parent thread
        // runs between this identity check and close in this fixture.
        if (original_channel_open()) ::close(original_fd);
        original_fd = -1;
        if (socket_pin >= 0) ::close(socket_pin);
        if (pidfd >= 0) ::close(pidfd);
        socket_pin = pidfd = -1;
    }
    ~SpawnFailureProbe() { cleanup(); }
};

}  // namespace

TEST_CASE("supervisor: failed child ownership setup leaves no process or channel",
          "[supervisor][spawn-owner]") {
    bool parent_failure = false;
    SECTION("owner allocation") {}
    SECTION("parent operation after fork") { parent_failure = true; }
    SpawnFailureProbe probe;
    obd::supervisor::ChildSpawnTestHooks hooks;
    hooks.parent_forked = [&](pid_t pid, int fd) {
        probe.capture(pid, fd);
        if (parent_failure) throw std::bad_alloc();
    };
    hooks.before_owner_allocation = [&] {
        if (!parent_failure) throw std::bad_alloc();
    };
    obd::supervisor::ChildSpec spec;
    spec.id = std::string(96, 'x');  // The copied ID also requires allocation.
    spec.device_bin = "/bin/true";
    spec.config_path = "/ignored";
    spec.spawn_test_hooks = &hooks;
    bool allocation_failed = false;
    try {
        auto unexpected = obd::supervisor::Child::spawn(spec);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    const bool reaped_by_factory = probe.factory_collected();
    const bool channel_closed_by_factory = !probe.original_channel_open();
    probe.cleanup();
    CHECK(allocation_failed);
    CHECK(reaped_by_factory);
    CHECK(channel_closed_by_factory);
    CHECK((probe.pid <= 0 || probe.collected));
}

TEST_CASE("supervisor: child ownership handoff retains the process and channel",
          "[supervisor][spawn-owner]") {
    unsigned allocations = 0;
    pid_t forked_pid = -1;
    int forked_channel = -1;
    obd::supervisor::ChildSpawnTestHooks hooks;
    hooks.before_owner_allocation = [&] { ++allocations; };
    hooks.parent_forked = [&](pid_t pid, int fd) {
        forked_pid = pid;
        forked_channel = fd;
    };
    obd::supervisor::ChildSpec spec;
    spec.id = std::string(96, 'y');
    spec.device_bin = "/bin/true";
    spec.config_path = "/ignored";
    spec.spawn_test_hooks = &hooks;
    auto child = obd::supervisor::Child::spawn(spec);
    CHECK(allocations == 1);
    CHECK(child->id() == spec.id);
    CHECK(child->pid() == forked_pid);
    CHECK(child->control_fd() == forked_channel);
    const int flags = ::fcntl(child->control_fd(), F_GETFL, 0);
    CHECK(flags >= 0);
    CHECK((flags & O_NONBLOCK) != 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!child->reaped() && std::chrono::steady_clock::now() < deadline) {
        child->try_reap();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(child->reaped());
    CHECK(child->status().exit_code == 0);
    const int owned_channel = child->control_fd();
    child.reset();
    const bool closed = ::fcntl(owned_channel, F_GETFD) < 0 && errno == EBADF;
    CHECK(closed);
}
