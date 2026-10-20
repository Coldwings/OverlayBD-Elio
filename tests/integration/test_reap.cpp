// EOF is a liveness notification, not a process-teardown barrier (#119).
// The fake closes its status channel and holds its final teardown behind
// a fixture gate. Every stop-and-remove path must keep owning it until reap.
#include "supervisor/daemon.hpp"
#include "supervisor/daemon_test_hooks.hpp"
#include "supervisor/protocol.hpp"

#include "../support.hpp"
#include "../ublk_prerequisites.hpp"

#include <catch2/catch_test_macros.hpp>
#include <elio/runtime/spawn.hpp>
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef OBD_TEST_FAKE_DEVICE_BIN
#define OBD_TEST_FAKE_DEVICE_BIN "obd-test-fake-device"
#endif

using namespace obd;
using namespace std::chrono_literals;

namespace {

// O_PATH pins pathname identity without invoking a block/character driver's
// open or doing device I/O. Keeping the inode referenced also prevents inode
// reuse from making a later generation appear to be the original node.
class FixtureNodeGeneration {
public:
    explicit FixtureNodeGeneration(std::string path) : path_(std::move(path)) {
        fd_ = ::open(path_.c_str(), O_PATH | O_CLOEXEC);
        if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "pin " + path_);
        if (::fstat(fd_, &identity_) != 0) {
            const int error = errno;
            ::close(fd_);
            throw std::system_error(error, std::generic_category(), "identify " + path_);
        }
    }
    ~FixtureNodeGeneration() { if (fd_ >= 0) ::close(fd_); }
    FixtureNodeGeneration(const FixtureNodeGeneration&) = delete;
    FixtureNodeGeneration& operator=(const FixtureNodeGeneration&) = delete;
    FixtureNodeGeneration(FixtureNodeGeneration&& other) noexcept
        : path_(std::move(other.path_)), fd_(std::exchange(other.fd_, -1)),
          identity_(other.identity_) {}

    bool present() const {
        struct stat current{};
        if (::stat(path_.c_str(), &current) != 0) {
            if (errno == ENOENT || errno == ENOTDIR) return false;
            throw std::system_error(errno, std::generic_category(), "observe " + path_);
        }
        return current.st_dev == identity_.st_dev && current.st_ino == identity_.st_ino;
    }
private:
    std::string path_;
    int fd_ = -1;
    struct stat identity_{};
};

#ifdef OBD_TEST_DEVICE_BIN
struct UblkFixtureGeneration {
    int id;
    std::vector<FixtureNodeGeneration> nodes;
    explicit UblkFixtureGeneration(int number) : id(number) {
        const std::string suffix = std::to_string(id);
        nodes.emplace_back("/sys/class/ublk-char/ublkc" + suffix);
        nodes.emplace_back("/dev/ublkc" + suffix);
        nodes.emplace_back("/dev/ublkb" + suffix);
    }
    bool removed() const {
        for (const auto& node : nodes) if (node.present()) return false;
        return true;
    }
};
#endif

struct ReapSignalMask {
    sigset_t previous{};
    ReapSignalMask() {
        sigset_t blocked;
        ::sigemptyset(&blocked);
        ::sigaddset(&blocked, SIGTERM);
        ::sigaddset(&blocked, SIGINT);
        ::sigaddset(&blocked, SIGCHLD);
        if (::sigprocmask(SIG_BLOCK, &blocked, &previous) != 0) {
            throw std::system_error(errno, std::generic_category(), "block signals");
        }
    }
    ~ReapSignalMask() { ::sigprocmask(SIG_SETMASK, &previous, nullptr); }
};

struct CleanupEnvironment {
    std::string previous;
    bool existed = false;
    explicit CleanupEnvironment(const std::string& path) {
        if (const char* old = std::getenv("OBD_TEST_CLEANUP_GATE")) {
            previous = old;
            existed = true;
        }
        if (::setenv("OBD_TEST_CLEANUP_GATE", path.c_str(), 1) != 0) {
            throw std::system_error(errno, std::generic_category(), "set cleanup gate");
        }
    }
    ~CleanupEnvironment() {
        if (existed) ::setenv("OBD_TEST_CLEANUP_GATE", previous.c_str(), 1);
        else ::unsetenv("OBD_TEST_CLEANUP_GATE");
    }
};

nlohmann::json reap_rpc(const std::string& path, const nlohmann::json& command) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "RPC socket");
    try {
        const timeval timeout{10, 0};
        if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
            throw std::system_error(errno, std::generic_category(), "RPC timeout");
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path.c_str());
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            throw std::system_error(errno, std::generic_category(), "RPC connect");
        }
        const std::string line = command.dump() + "\n";
        size_t sent = 0;
        while (sent < line.size()) {
            const ssize_t n = ::send(fd, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("RPC send failed");
            sent += static_cast<size_t>(n);
        }
        std::string reply;
        while (reply.find('\n') == std::string::npos) {
            char buffer[4096];
            const ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("RPC receive failed");
            reply.append(buffer, static_cast<size_t>(n));
        }
        auto result = nlohmann::json::parse(reply);
        ::close(fd);
        return result;
    } catch (...) {
        ::close(fd);
        throw;
    }
}

template <typename F>
bool wait_for_reap_fixture(F&& condition) {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

class FailedMkfs final : public supervisor::MkfsRunner {
public:
    std::atomic<bool> release{false};
    elio::coro::task<int> run(const std::string&, const std::string&,
                             std::string* error) override {
        while (!release.load()) co_await elio::time::sleep_for(1ms);
        *error = "intentional mkfs failure";
        co_return 1;
    }
};

void check_reap_barrier(const std::string& operation) {
    test::TempDir directory;
    const std::string socket = directory / "s.sock";
    const std::string upper = directory / "upper";
    const std::string config = directory / "config.json";
    const nlohmann::json image{{"upper", {{"type", "lsmt"}, {"dir", upper}}}};
    const std::string text = image.dump();
    test::write_file(config, std::vector<uint8_t>(text.begin(), text.end()));
    CleanupEnvironment environment(directory.str());
    ReapSignalMask mask;
    auto gate = std::make_shared<supervisor::detail::DaemonTestGate>();
    supervisor::DaemonConfig cfg;
    cfg.socket_path = socket;
    cfg.device_bin = OBD_TEST_FAKE_DEVICE_BIN;
    cfg.global_config = "";
    cfg.blank_dir = directory / "blank";
    cfg.max_recovery_attempts = 0;
    cfg.ready_timeout_sec = 5;
    cfg.stop_timeout_sec = 5;
    auto mkfs = std::make_shared<FailedMkfs>();
    cfg.mkfs_runner = mkfs;

    std::atomic<bool> client_done{false}, operation_done{false};
    std::atomic<int> daemon_result{-1};
    std::vector<std::string> failures;
    std::string daemon_error;
    pid_t child_pid = -1;
    int pidfd = -1;
    auto check = [&](bool condition, std::string error) {
        if (!condition) failures.emplace_back(std::move(error));
    };
    std::thread client([&] {
        std::thread request;
        nlohmann::json response;
        std::string request_error;
        auto start_request = [&](nlohmann::json command) {
            request = std::thread([&, command = std::move(command)] {
                try { response = reap_rpc(socket, command); }
                catch (const std::exception& e) { request_error = e.what(); }
                operation_done.store(true);
            });
        };
        try {
            if (!wait_for_reap_fixture([&] { return std::filesystem::exists(socket); })) {
                throw std::runtime_error("daemon listener did not appear");
            }
            if (operation == "create_failure") {
                start_request({{"cmd", "create"}, {"id", "d"},
                               {"blank", {{"size", 32768}, {"mkfs", "ext4"}}}});
                if (!wait_for_reap_fixture([&] {
                        const auto status = reap_rpc(socket, {{"cmd", "status"}, {"id", "d"}});
                        child_pid = status.value("pid", -1);
                        return child_pid > 0;
                    })) throw std::runtime_error("failed create child not published");
            } else {
                const auto created = reap_rpc(socket, {{"cmd", "create"}, {"id", "d"},
                                                       {"config", config}});
                if (!created.value("ok", false)) throw std::runtime_error(created.dump());
                child_pid = created.value("pid", -1);
            }
            pidfd = static_cast<int>(::syscall(SYS_pidfd_open, child_pid, 0));
            if (pidfd < 0) throw std::system_error(errno, std::generic_category(), "pin child PID");
            if (operation == "shutdown") {
                if (::kill(::getpid(), SIGTERM) != 0) throw std::runtime_error("signal daemon");
            } else if (operation == "create_failure") {
                mkfs->release.store(true);
            } else if (operation != "create_failure") {
                start_request({{"cmd", operation}, {"id", "d"}});
            }
            if (!wait_for_reap_fixture([&] {
                    return std::filesystem::exists(directory / "closed") &&
                           gate->count("device_eof_completed") > 0;
                })) throw std::runtime_error("child EOF not processed");
            // The process is deliberately alive for the entire observation
            // window. EOF is fully processed; a reply/return now is a bug.
            std::this_thread::sleep_for(200ms);
            check(operation == "shutdown" ? daemon_result.load() < 0 : !operation_done.load(),
                  operation + " completed before actual child exit");
            pollfd alive{pidfd, POLLIN, 0};
            check(::poll(&alive, 1, 0) == 0, "child killed while final cleanup was held");
        } catch (const std::exception& e) {
            check(false, e.what());
        }
        // Always release and join fixture resources before any Catch2
        // assertion, including when testing the broken baseline.
        mkfs->release.store(true);
        try { test::write_file(directory / "release", {}); }
        catch (const std::exception& e) {
            check(false, e.what());
            if (pidfd >= 0) ::syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0);
        }
        if (request.joinable()) request.join();
        check(request_error.empty(), request_error);
        if (operation != "shutdown") {
            check(response.value("ok", false) == (operation != "create_failure"),
                  "unexpected operation result: " + response.dump());
        } else {
            check(wait_for_reap_fixture([&] { return daemon_result.load() != -1; }),
                  "shutdown did not finish after cleanup release");
        }
        check(std::filesystem::exists(directory / "cleaned"), "operation completed before final cleanup marker");
        if (pidfd >= 0) {
            pollfd exited{pidfd, POLLIN, 0};
            check(::poll(&exited, 1, 0) == 1, "operation returned before child exit");
        }
        client_done.store(true);
    });
    int result = -1;
    try {
        result = test::run_coro([&]() -> elio::coro::task<int> {
            elio::go([&]() -> elio::coro::task<void> {
                try {
                    daemon_result.store(co_await supervisor::detail::run_daemon_with_test_gate(cfg, gate));
                } catch (const std::exception& e) {
                    daemon_error = e.what();
                    daemon_result.store(1);
                }
            });
            while (!client_done.load()) co_await elio::time::sleep_for(2ms);
            if (daemon_result.load() == -1) ::kill(::getpid(), SIGTERM);
            while (daemon_result.load() == -1) co_await elio::time::sleep_for(2ms);
            co_return 0;
        });
    } catch (const std::exception& e) { daemon_error = e.what(); }
    client.join();
    // The daemon has fully drained. Only now may the fixture wait on its
    // owned PID; never race or steal the daemon reaper's status.
    if (pidfd >= 0) {
        ::syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0);
        ::close(pidfd);
        int status = 0;
        pid_t waited;
        do { waited = ::waitpid(child_pid, &status, 0); } while (waited < 0 && errno == EINTR);
        check(waited < 0 && errno == ECHILD, "daemon returned without reaping child");
    }
    INFO("operation: " << operation);
    INFO(daemon_error);
    for (const auto& failure : failures) WARN(failure);
    CHECK(failures.empty());
    CHECK(daemon_error.empty());
    CHECK(result == 0);
    CHECK(daemon_result.load() == 0);
}

}  // namespace

TEST_CASE("supervisor: released device node checks preserve a replacement generation",
          "[supervisor][fixture-ownership]") {
    test::TempDir directory;
    const std::string path = directory / "ublkc7";
    test::write_file(path, {1});
    FixtureNodeGeneration original(path);
    CHECK(original.present());
    // Regular files deterministically model pathname reuse without creating
    // or deleting any host ublk ID. The old O_PATH pin prevents inode reuse.
    REQUIRE(std::filesystem::remove(path));
    test::write_file(path, {2});
    CHECK_FALSE(original.present());
    CHECK(std::filesystem::exists(path));
    std::ifstream replacement(path, std::ios::binary);
    CHECK(replacement.get() == 2);
}

TEST_CASE("supervisor: destroy waits for reap after control EOF", "[supervisor][reap-barrier]") {
    check_reap_barrier("destroy");
}

TEST_CASE("supervisor: commit waits for reap before sealing", "[supervisor][reap-barrier]") {
    check_reap_barrier("commit");
}

TEST_CASE("supervisor: failed blank create waits for reap before removal", "[supervisor][reap-barrier]") {
    check_reap_barrier("create_failure");
}

TEST_CASE("supervisor: shutdown waits for reap after control EOF", "[supervisor][reap-barrier]") {
    check_reap_barrier("shutdown");
}

#ifdef OBD_TEST_DEVICE_BIN
TEST_CASE("integration: supervisor reaps children before releasing ublk devices", "[ublk][supervisor]") {
    if (const auto reason = test::ublk_prerequisite_skip_reason();
        !reason.empty()) {
        SKIP(reason);
    }
    test::TempDir directory;
    const std::string socket = directory / "s.sock";
    const std::string global = directory / "global.json";
    const std::string global_text = R"({"ublkConfig":{"enableRecovery":true}})";
    test::write_file(global, std::vector<uint8_t>(global_text.begin(), global_text.end()));
    ReapSignalMask mask;
    supervisor::DaemonConfig cfg;
    cfg.socket_path = socket;
    cfg.device_bin = OBD_TEST_DEVICE_BIN;
    cfg.global_config = global;
    cfg.blank_dir = directory / "blank";
    cfg.max_recovery_attempts = 0;
    cfg.ready_timeout_sec = 10;
    cfg.stop_timeout_sec = 5;
    auto mkfs = std::make_shared<FailedMkfs>();
    mkfs->release.store(true);
    cfg.mkfs_runner = mkfs;

    std::atomic<bool> client_done{false};
    std::atomic<int> daemon_result{-1};
    std::vector<std::string> failures;
    std::string daemon_error;
    std::vector<UblkFixtureGeneration> live_generations;
    auto check = [&](bool condition, std::string error) {
        if (!condition) failures.emplace_back(std::move(error));
    };
    // Observe only the pinned generation. A successful lifecycle operation
    // releases its numeric ID, so even a failed check cannot authorize a
    // fallback STOP/DEL of whichever device now occupies that number.
    auto verify_removed = [&](const UblkFixtureGeneration& generation) {
        check(generation.removed(), "original kernel ublk nodes survived child reap: " +
                                    std::to_string(generation.id));
    };
    std::thread client([&] {
        try {
            if (!wait_for_reap_fixture([&] { return std::filesystem::exists(socket); })) {
                throw std::runtime_error("daemon listener did not appear");
            }
            auto rpc = [&](const nlohmann::json& command) { return reap_rpc(socket, command); };
            auto create = [&](const std::string& name) {
                const auto result = rpc({{"cmd", "create"}, {"id", name},
                                         {"blank", {{"size", 1048576}}}});
                if (!result.value("ok", false)) throw std::runtime_error(result.dump());
                const int id = supervisor::dev_id_from_bdev_path(result.value("device", ""));
                if (id < 0) throw std::runtime_error("invalid ready device path");
                return id;
            };
            for (int cycle = 0; cycle < 5; ++cycle) {
                const int id = create("cycle");
                live_generations.emplace_back(id);
                check(rpc({{"cmd", "destroy"}, {"id", "cycle"}}).value("ok", false), "destroy failed");
                verify_removed(live_generations.back());
                live_generations.clear();
            }
            const int committed_id = create("commit");
            live_generations.emplace_back(committed_id);
            check(rpc({{"cmd", "commit"}, {"id", "commit"}}).value("ok", false), "commit failed");
            verify_removed(live_generations.back());
            live_generations.clear();
            check(rpc({{"cmd", "destroy"}, {"id", "commit"}}).value("ok", false), "committed entry removal failed");

            // Hold the failed mkfs until the fixture has captured this
            // generation's real device ID; cleanup must wait for its reap.
            mkfs->release.store(false);
            nlohmann::json failed_create;
            std::string create_error;
            std::thread failing([&] {
                try { failed_create = rpc({{"cmd", "create"}, {"id", "failed"},
                                           {"blank", {{"size", 1048576}, {"mkfs", "ext4"}}}}); }
                catch (const std::exception& e) { create_error = e.what(); }
            });
            int failed_id = -1;
            try {
                check(wait_for_reap_fixture([&] {
                    const auto status = rpc({{"cmd", "status"}, {"id", "failed"}});
                    failed_id = supervisor::dev_id_from_bdev_path(status.value("device", ""));
                    return failed_id >= 0;
                }), "failed create did not become ready");
                if (failed_id >= 0) live_generations.emplace_back(failed_id);
            } catch (const std::exception& e) { check(false, e.what()); }
            mkfs->release.store(true);
            failing.join();
            check(create_error.empty(), create_error);
            check(!failed_create.value("ok", true), "failed mkfs was reported successful");
            if (!live_generations.empty()) verify_removed(live_generations.back());
            live_generations.clear();
            for (int child = 0; child < 3; ++child) {
                live_generations.emplace_back(create("shutdown" + std::to_string(child)));
            }
        } catch (const std::exception& e) { check(false, e.what()); }
        mkfs->release.store(true);
        client_done.store(true);
    });
    int result = -1;
    try {
        result = test::run_coro([&]() -> elio::coro::task<int> {
            elio::go([&]() -> elio::coro::task<void> {
                try { daemon_result.store(co_await supervisor::run_daemon(cfg)); }
                catch (const std::exception& e) {
                    daemon_error = e.what();
                    daemon_result.store(1);
                }
            });
            while (!client_done.load()) co_await elio::time::sleep_for(2ms);
            if (daemon_result.load() == -1) ::kill(::getpid(), SIGTERM);
            while (daemon_result.load() == -1) co_await elio::time::sleep_for(2ms);
            co_return 0;
        });
    } catch (const std::exception& e) { daemon_error = e.what(); }
    client.join();
    for (const auto& generation : live_generations) verify_removed(generation);
    INFO(daemon_error);
    for (const auto& failure : failures) WARN(failure);
    CHECK(failures.empty());
    CHECK(daemon_error.empty());
    CHECK(result == 0);
    CHECK(daemon_result.load() == 0);
}
#endif
