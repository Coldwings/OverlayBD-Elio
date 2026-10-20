#include "supervisor/child.hpp"
#include "supervisor/protocol.hpp"

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

extern char** environ;

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

class FdOwner {
public:
    explicit FdOwner(int fd) : fd_(fd) {
        if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "open test fd");
    }
    ~FdOwner() { ::close(fd_); }
    FdOwner(const FdOwner&) = delete;
    FdOwner& operator=(const FdOwner&) = delete;
    int get() const noexcept { return fd_; }
private:
    int fd_;
};

// An open proc directory identifies the original process even if its numeric
// PID is later reused. Its stat disappears only after that process is reaped;
// a zombie still has a readable stat. This checks more than entry removal.
class ProcessObservation {
public:
    explicit ProcessObservation(pid_t pid)
        : directory_(::open(("/proc/" + std::to_string(pid)).c_str(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {}
    bool reaped() const {
        const int fd = ::openat(directory_.get(), "stat", O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            if (errno == ENOENT || errno == ESRCH) return true;
            throw std::system_error(errno, std::generic_category(), "observe fake process");
        }
        FdOwner stat(fd);
        char byte;
        const auto count = ::read(stat.get(), &byte, 1);
        if (count >= 0) return false;
        if (errno == ENOENT || errno == ESRCH) return true;
        throw std::system_error(errno, std::generic_category(), "read fake process stat");
    }
private:
    FdOwner directory_;
};

void wait_socket(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now()).count();
        if (left <= 0) throw std::runtime_error("startup test RPC deadline expired");
        pollfd pfd{fd, events, 0};
        const int rc = ::poll(&pfd, 1, left > 5000 ? 5000 : static_cast<int>(left));
        if (rc > 0) return; // recv/send also report EOF and socket errors.
        if (rc < 0 && errno != EINTR)
            throw std::system_error(errno, std::generic_category(), "poll");
    }
}

Json rpc(const std::string& path, const Json& request,
         std::chrono::milliseconds timeout = 5s) {
    const auto deadline = Clock::now() + timeout;
    FdOwner fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path))
        throw std::runtime_error("startup test socket path too long");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        if (errno != EINPROGRESS)
            throw std::system_error(errno, std::generic_category(), "connect");
        wait_socket(fd.get(), POLLOUT, deadline);
        int error = 0;
        socklen_t size = sizeof(error);
        if (::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &error, &size) < 0)
            throw std::system_error(errno, std::generic_category(), "getsockopt");
        if (error) throw std::system_error(error, std::generic_category(), "connect");
    }
    const auto line = request.dump() + "\n";
    size_t sent = 0;
    while (sent < line.size()) {
        const auto rc = ::send(fd.get(), line.data() + sent, line.size() - sent,
                               MSG_NOSIGNAL);
        if (rc > 0) sent += static_cast<size_t>(rc);
        else if (rc < 0 && errno == EINTR) continue;
        else if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            wait_socket(fd.get(), POLLOUT, deadline);
        else throw std::runtime_error("startup test RPC send failed");
    }
    std::string reply;
    for (;;) {
        char buffer[4096];
        const auto rc = ::recv(fd.get(), buffer, sizeof(buffer), 0);
        if (rc > 0) {
            reply.append(buffer, static_cast<size_t>(rc));
            if (const auto end = reply.find('\n'); end != std::string::npos)
                return Json::parse(reply.substr(0, end));
            if (reply.size() > 65536) throw std::runtime_error("oversized test RPC reply");
        } else if (rc < 0 && errno == EINTR) {
            continue;
        } else if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            wait_socket(fd.get(), POLLIN, deadline);
        } else {
            throw std::runtime_error("startup test RPC EOF before reply");
        }
    }
}

// A separate executable keeps SIGTERM and its signalfd out of the Catch2
// process. A fresh process group contains only this daemon and its test fake.
// Keeping the direct child waitable (WNOWAIT) pins its PID until group cleanup.
// Subreaping lets assertion-failure cleanup collect orphaned fake descendants,
// instead of leaving zombies to an unrelated container init.
class OwnedDaemon {
public:
    OwnedDaemon(const obd::test::TempDir& dir, bool refuse_once,
                std::string device_bin = OBD_TEST_FAKE_DEVICE_BIN)
        : socket_(dir / "supervisor.sock") {
        if (::prctl(PR_GET_CHILD_SUBREAPER, &old_subreaper_) < 0)
            throw std::system_error(errno, std::generic_category(), "get subreaper");
        if (::prctl(PR_SET_CHILD_SUBREAPER, 1) < 0)
            throw std::system_error(errno, std::generic_category(), "set subreaper");
        restore_subreaper_ = true;
        try {
            std::vector<std::string> environment;
            for (char** item = environ; *item; ++item) {
                const std::string entry(*item);
                if (!entry.starts_with("OBD_TEST_STARTUP_RETAIN=") &&
                    !entry.starts_with("OBD_TEST_STARTUP_REFUSE_ONCE="))
                    environment.push_back(entry);
            }
            environment.emplace_back("OBD_TEST_STARTUP_RETAIN=1");
            if (refuse_once) environment.emplace_back("OBD_TEST_STARTUP_REFUSE_ONCE=1");
            std::vector<char*> envp;
            for (auto& entry : environment) envp.push_back(entry.data());
            envp.push_back(nullptr);
            std::vector<std::string> arguments{
                OBD_TEST_SUPERVISOR_BIN, "--socket", socket_,
                "--global", "", "--device-bin", std::move(device_bin),
                "--ready-timeout", "2", "--stop-timeout", "1",
                "--blank-dir", dir / "blank"};
            std::vector<char*> argv;
            for (auto& argument : arguments) argv.push_back(argument.data());
            argv.push_back(nullptr);
            posix_spawnattr_t attributes;
            int error = ::posix_spawnattr_init(&attributes);
            if (error) throw std::system_error(error, std::generic_category(), "spawn attributes");
            sigset_t empty;
            ::sigemptyset(&empty);
            sigset_t defaults;
            ::sigemptyset(&defaults);
            ::sigaddset(&defaults, SIGTERM);
            ::sigaddset(&defaults, SIGINT);
            ::sigaddset(&defaults, SIGCHLD);
            error = ::posix_spawnattr_setpgroup(&attributes, 0);
            if (!error) error = ::posix_spawnattr_setsigmask(&attributes, &empty);
            if (!error) error = ::posix_spawnattr_setsigdefault(&attributes, &defaults);
            if (!error) error = ::posix_spawnattr_setflags(
                &attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
                             POSIX_SPAWN_SETSIGDEF);
            pid_t spawned = -1;
            if (!error) error = ::posix_spawn(&spawned, OBD_TEST_SUPERVISOR_BIN,
                nullptr, &attributes, argv.data(), envp.data());
            ::posix_spawnattr_destroy(&attributes);
            if (error) throw std::system_error(error, std::generic_category(), "spawn supervisor");
            pid_ = spawned;
            const auto deadline = Clock::now() + 5s;
            for (;;) {
                try {
                    if (rpc(socket_, {{"cmd", "hello"}}, 200ms).value("ok", false))
                        break;
                } catch (const std::exception&) {}
                if (exited() || Clock::now() >= deadline)
                    throw std::runtime_error("test supervisor did not start");
                std::this_thread::sleep_for(10ms);
            }
        } catch (...) {
            (void)cleanup();
            restore_subreaper();
            throw;
        }
    }
    ~OwnedDaemon() {
        (void)cleanup();
        restore_subreaper();
    }
    OwnedDaemon(const OwnedDaemon&) = delete;
    OwnedDaemon& operator=(const OwnedDaemon&) = delete;

    Json call(const Json& request) const { return rpc(socket_, request); }
    bool exited() {
        siginfo_t info{};
        if (::waitid(P_PID, static_cast<id_t>(pid_), &info,
                     WEXITED | WNOHANG | WNOWAIT) < 0)
            throw std::system_error(errno, std::generic_category(), "observe supervisor");
        if (info.si_pid == 0) return false;
        exit_code_ = info.si_code == CLD_EXITED ? info.si_status : -1;
        return true;
    }
    void terminate() {
        if (::kill(pid_, SIGTERM) < 0)
            throw std::system_error(errno, std::generic_category(), "signal supervisor");
    }
    bool wait_exit() {
        const auto deadline = Clock::now() + 5s;
        do {
            if (exited()) return true;
            std::this_thread::sleep_for(10ms);
        } while (Clock::now() < deadline);
        return false;
    }
    int exit_code() const noexcept { return exit_code_; }
    bool cleanup() noexcept {
        if (pid_ <= 0) return true;
        // Before the first collection the leader is still waitable, pinning
        // the group ID. A retry after a cleanup timeout only collects; it must
        // not signal a numeric group whose leader may already be reaped.
        if (!group_signaled_) {
            ::kill(-pid_, SIGKILL);
            group_signaled_ = true;
        }
        const auto deadline = Clock::now() + 5s;
        do {
            const pid_t collected = ::waitpid(-pid_, nullptr, WNOHANG);
            if (collected > 0) continue;
            if (collected < 0 && errno == ECHILD) {
                pid_ = -1;
                return true;
            }
            if (collected < 0 && errno != EINTR) return false;
            std::this_thread::sleep_for(1ms);
        } while (Clock::now() < deadline);
        return false;
    }
private:
    void restore_subreaper() noexcept {
        if (restore_subreaper_) {
            ::prctl(PR_SET_CHILD_SUBREAPER, old_subreaper_);
            restore_subreaper_ = false;
        }
    }
    std::string socket_;
    pid_t pid_ = -1;
    int exit_code_ = -1;
    int old_subreaper_ = 0;
    bool restore_subreaper_ = false;
    bool group_signaled_ = false;
};

Json create_retained(OwnedDaemon& daemon) {
    const auto created = daemon.call(
        {{"cmd", "create"}, {"id", "held"}, {"blank", {{"size", 4096}}}});
    REQUIRE_FALSE(created.value("ok", true));
    const auto status = daemon.call({{"cmd", "status"}, {"id", "held"}});
    REQUIRE(status.value("ok", false));
    REQUIRE(status.at("state") == "failed");
    REQUIRE(status.at("startup_protected") == true);
    REQUIRE(status.at("exit_code") == -1);
    REQUIRE(status.at("pid").get<int>() > 0);
    REQUIRE(status.at("startup_owner").at("phase") == "retained");
    REQUIRE(status.at("startup_owner").at("raw_published") == 2);
    REQUIRE(status.at("startup_owner").at("raw_retired") == 0);
    return status;
}

void require_same_retained_owner(const Json& status, const Json& original) {
    REQUIRE(status.value("ok", false));
    REQUIRE(status.at("startup_protected") == true);
    REQUIRE(status.at("pid") == original.at("pid"));
    REQUIRE(status.at("startup_owner").at("generation") ==
            original.at("startup_owner").at("generation"));
    REQUIRE(status.at("exit_code") == -1);
}

} // namespace

#ifdef OBD_TEST_DEVICE_BIN
TEST_CASE("supervisor: real device startup preserves the original assembly diagnostic",
          "[supervisor][startup-supervisor]") {
    obd::test::TempDir dir;
    OwnedDaemon daemon(dir, false, OBD_TEST_DEVICE_BIN);
    // This fails in image configuration, before any ublk control/ring opens.
    const std::string config = dir / "invalid-image.json";
    const std::string sentinel = "startup-diagnostic-sentinel";
    const auto text = Json{{"upper", {{"type", sentinel}, {"dir", dir / "upper"}}}}.dump();
    obd::test::write_file(config, std::vector<uint8_t>(text.begin(), text.end()));
    const auto failed = daemon.call(
        {{"cmd", "create"}, {"id", "invalid"}, {"config", config}});
    REQUIRE_FALSE(failed.value("ok", true));
    REQUIRE(failed.at("error").get<std::string>().find(sentinel) != std::string::npos);
    const auto deadline = Clock::now() + 3s;
    Json status;
    do {
        status = daemon.call({{"cmd", "status"}, {"id", "invalid"}});
        if (!status.value("startup_protected", true)) break;
        std::this_thread::sleep_for(10ms);
    } while (Clock::now() < deadline);
    REQUIRE(status.value("ok", false));
    REQUIRE_FALSE(status.value("startup_protected", true));
    REQUIRE(status.at("startup_owner").at("phase") == "released");
    REQUIRE(daemon.call({{"cmd", "destroy"}, {"id", "invalid"}}).value("ok", false));
    daemon.terminate();
    REQUIRE(daemon.wait_exit());
    REQUIRE(daemon.exit_code() == 0);
    REQUIRE(daemon.cleanup());
}
#endif

TEST_CASE("supervisor: retained startup survives release refusal then releases on retry",
          "[supervisor][startup-supervisor]") {
    obd::test::TempDir dir;
    OwnedDaemon daemon(dir, true);
    const auto original = create_retained(daemon);
    ProcessObservation fake(original.at("pid").get<pid_t>());
    REQUIRE_FALSE(fake.reaped());
    const auto listed = daemon.call({{"cmd", "list"}});
    REQUIRE(listed.value("ok", false));
    REQUIRE(listed.at("devices").size() == 1);
    REQUIRE(listed.at("devices").at(0).at("id") == "held");
    REQUIRE(listed.at("devices").at(0).at("startup_protected") == true);
    REQUIRE(listed.at("devices").at(0).at("pid") == original.at("pid"));

    const auto refused = daemon.call({{"cmd", "destroy"}, {"id", "held"}});
    REQUIRE_FALSE(refused.value("ok", true));
    const auto retained = daemon.call({{"cmd", "status"}, {"id", "held"}});
    require_same_retained_owner(retained, original);
    REQUIRE(retained.at("startup_owner").at("release") == "refused");
    REQUIRE(retained.at("startup_owner").at("release_sequence") == 1);
    REQUIRE(retained.at("startup_owner").at("release_errno") == EIO);
    REQUIRE_FALSE(fake.reaped());
    const auto after_refusal = daemon.call({{"cmd", "list"}});
    REQUIRE(after_refusal.at("devices").size() == 1);
    REQUIRE(after_refusal.at("devices").at(0).at("startup_protected") == true);

    const auto destroyed = daemon.call({{"cmd", "destroy"}, {"id", "held"}});
    REQUIRE(destroyed.value("ok", false));
    REQUIRE(fake.reaped());
    REQUIRE_FALSE(daemon.call({{"cmd", "status"}, {"id", "held"}}).value("ok", true));
    REQUIRE(daemon.call({{"cmd", "list"}}).at("devices").empty());
    daemon.terminate();
    REQUIRE(daemon.wait_exit());
    REQUIRE(daemon.exit_code() == 0);
    REQUIRE(daemon.cleanup());
}

TEST_CASE("supervisor: SIGTERM preserves retained owners and serves maintenance commands",
          "[supervisor][startup-supervisor]") {
    obd::test::TempDir dir;
    OwnedDaemon daemon(dir, false);
    const auto original = create_retained(daemon);
    ProcessObservation fake(original.at("pid").get<pid_t>());
    daemon.terminate();
    Json status;
    const auto deadline = Clock::now() + 3s;
    do {
        status = daemon.call({{"cmd", "status"}, {"id", "held"}});
        if (status.value("shutdown_pending", false)) break;
        std::this_thread::sleep_for(10ms);
    } while (Clock::now() < deadline);
    REQUIRE(status.value("shutdown_pending", false));
    require_same_retained_owner(status, original);
    REQUIRE_FALSE(fake.reaped());
    REQUIRE_FALSE(daemon.exited());
    const auto listed = daemon.call({{"cmd", "list"}});
    REQUIRE(listed.value("ok", false));
    REQUIRE(listed.at("devices").size() == 1);
    REQUIRE(listed.at("devices").at(0).at("shutdown_pending") == true);

    const auto create = daemon.call(
        {{"cmd", "create"}, {"id", "other"}, {"blank", {{"size", 4096}}}});
    REQUIRE_FALSE(create.value("ok", true));
    REQUIRE(create.at("error").get<std::string>().find("shutdown") != std::string::npos);
    const auto commit = daemon.call({{"cmd", "commit"}, {"id", "held"}});
    REQUIRE_FALSE(commit.value("ok", true));
    REQUIRE(commit.at("error").get<std::string>().find("shutdown") != std::string::npos);
    require_same_retained_owner(
        daemon.call({{"cmd", "status"}, {"id", "held"}}), original);
    REQUIRE_FALSE(daemon.exited());

    REQUIRE(daemon.call({{"cmd", "destroy"}, {"id", "held"}}).value("ok", false));
    REQUIRE(fake.reaped());
    REQUIRE(daemon.wait_exit());
    REQUIRE(daemon.exit_code() == 0);
    REQUIRE(daemon.cleanup());
}

TEST_CASE("supervisor: startup release admission preserves identity and retry sequence",
          "[supervisor][startup-supervisor]") {
    obd::supervisor::ChildSpec spec;
    spec.id = "identity";
    spec.device_bin = "/bin/true"; // Exits on its own even if an assertion fails.
    spec.startup_generation = 19;
    auto child = obd::supervisor::Child::spawn(spec);
    REQUIRE_FALSE(child->protect_startup(18));
    REQUIRE(child->protect_startup(19));
    obd::supervisor::StartupOwnerStatus owner;
    owner.generation = 19;
    owner.revision = 2;
    owner.phase = "retained";
    owner.device_id = 7;
    owner.reason_errno = EIO;
    owner.raw_published = 2;
    REQUIRE(child->observe_startup(owner, false));
    const auto first = child->admit_startup_release();
    REQUIRE(first == 1);
    REQUIRE(child->admit_startup_release() == first);

    auto invalid = owner;
    invalid.generation = 20;
    REQUIRE_FALSE(child->observe_startup(invalid, false));
    invalid = owner;
    invalid.revision = 1;
    REQUIRE_FALSE(child->observe_startup(invalid, false));
    invalid = owner;
    invalid.device_id = 8;
    invalid.revision = 4;
    REQUIRE_FALSE(child->observe_startup(invalid, false));
    invalid = owner;
    invalid.phase = "released";
    invalid.raw_retired = 2;
    invalid.release = "released";
    invalid.release_sequence = 2; // Not admitted by this parent.
    invalid.revision = 4;
    REQUIRE_FALSE(child->observe_startup(invalid, false));
    REQUIRE(child->startup_protected());

    owner.revision = 4;
    owner.release_sequence = 1;
    owner.release = "refused";
    owner.release_errno = EIO;
    REQUIRE(child->observe_startup(owner, false));
    REQUIRE(child->admit_startup_release() == 2);
    REQUIRE(child->admit_startup_release() == 2);
    owner.revision = 6;
    owner.phase = "released";
    owner.raw_retired = 2;
    owner.release_sequence = 2;
    owner.release = "released";
    owner.release_errno = 0;
    REQUIRE(child->observe_startup(owner, false));
    REQUIRE_FALSE(child->startup_protected());
    REQUIRE_FALSE(child->admit_startup_release());
}
