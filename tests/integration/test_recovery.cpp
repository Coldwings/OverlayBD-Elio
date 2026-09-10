// Integration test: supervisor crash recovery via ublk USER_RECOVERY
// (ADR-0010) — pure process choreography with a fake obd-device; no
// kernel ublk needed (the recovery handshake itself is covered by the
// privileged ublk E2E, which self-skips without /dev/ublk-control).
#include "supervisor/daemon.hpp"
#include "supervisor/daemon_test_hooks.hpp"
#include "supervisor/protocol.hpp"

#include <elio/runtime/spawn.hpp>
#include <elio/time/timer.hpp>

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <atomic>
#include <charconv>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <system_error>
#include <chrono>
#include <string>
#include <thread>

using namespace obd;

namespace {

/// Minimal synchronous obdctl-style client: one JSON line per connection.
/// Runs on a helper thread, so NO Catch2 macros here (Catch2 assertion
/// state is not thread-safe by default); failures surface as exceptions.
std::string uds_rpc(const std::string& path, const std::string& line) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw std::system_error(errno, std::generic_category());
    const timeval timeout{8, 0};
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        const int e = errno;
        ::close(fd);
        throw std::system_error(e, std::generic_category(), "set RPC receive timeout");
    }
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        const int e = errno;
        ::close(fd);
        throw std::system_error(e, std::generic_category(), "set RPC send timeout");
    }
    sockaddr_un sa {};
    sa.sun_family = AF_UNIX;
    std::snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path.c_str());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        const int e = errno;
        ::close(fd);
        throw std::system_error(e, std::generic_category());
    }
    size_t sent = 0;
    while (sent < line.size()) {
        const ssize_t n = ::write(fd, line.data() + sent, line.size() - sent);
        if (n < 0) {
            const int e = errno;
            if (e == EINTR) continue;
            ::close(fd);
            throw std::system_error(e, std::generic_category(), "write RPC request");
        }
        if (n == 0) {
            ::close(fd);
            throw std::runtime_error("RPC write made no progress");
        }
        sent += static_cast<size_t>(n);
    }
    std::string reply;
    char buf[4096];
    for (;;) {
        const ssize_t r = ::read(fd, buf, sizeof(buf));
        if (r < 0) {
            const int e = errno;
            if (e == EINTR) continue;
            ::close(fd);
            throw std::system_error(e, std::generic_category(), "read RPC reply");
        }
        if (r == 0) break;
        reply.append(buf, static_cast<size_t>(r));
        if (reply.find('\n') != std::string::npos) break;
    }
    ::close(fd);
    return reply;
}

size_t count_lines(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return 0;
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return 0;
    }
    std::string text(static_cast<size_t>(st.st_size), ' ');
    const ssize_t r = ::read(fd, text.data(), text.size());
    ::close(fd);
    if (r < 0) return 0;
    text.resize(static_cast<size_t>(r));
    size_t n = 0;
    for (const char c : text) {
        if (c == '\n') ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("supervisor: crashed device child is recovered with bounded respawns",
          "[supervisor]") {
    test::TempDir dir;
    const std::string sock = dir / "supervisor.sock";
    const std::string argv_log = dir / "argv.log";
    const std::string fake_cfg = test::write_file(dir / "config.json",
                                                  std::vector<uint8_t>{'{', '}'});
    // Fake obd-device: logs argv, reports ready on fd 3, dies with exit 1.
    const std::string script = dir / "fake-device.sh";
    {
        const std::string content =
            "#!/bin/sh\necho \"$@\" >> \"" + argv_log +
            "\"\necho '{\"state\":\"ready\",\"device\":\"/dev/ublkb7\"}' >&3\n"
            "sleep 0.2\nexit 1\n";
        test::write_file(script,
                         std::vector<uint8_t>(content.begin(), content.end()));
        REQUIRE(::chmod(script.c_str(), 0755) == 0);
    }
    ::setenv("OBD_FAKE_ARGV", argv_log.c_str(), 1);

    // run_daemon requires signals blocked process-wide (signalfd model).
    sigset_t block, prev;
    ::sigemptyset(&block);
    ::sigaddset(&block, SIGTERM);
    ::sigaddset(&block, SIGINT);
    ::sigaddset(&block, SIGCHLD);
    REQUIRE(::sigprocmask(SIG_BLOCK, &block, &prev) == 0);

    // The synchronous RPC client runs on its own thread; the daemon and
    // the test driver share one Elio scheduler whose root coroutine stays
    // alive until the daemon exits (detached tasks must not outlive it).
    std::atomic<bool> client_done{false};
    std::atomic<int> failures{0};
    std::string fail_msg;
    auto check = [&](bool cond, const char* what) {
        if (!cond) {
            failures.fetch_add(1);
            fail_msg = what;
        }
    };
    std::thread client([&] {
      try {
        for (int i = 0; i < 250 && !std::filesystem::exists(sock); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!std::filesystem::exists(sock)) {
            check(false, "supervisor socket never appeared");
            client_done.store(true);
            return;
        }
        auto rpc_json = [&](const nlohmann::json& cmd) {
            return nlohmann::json::parse(uds_rpc(sock, cmd.dump() + "\n"));
        };
        const auto reply = rpc_json(
            {{"cmd", "create"}, {"id", "d1"}, {"config", fake_cfg}});
        check(reply.value("ok", false) == true, "create failed");

        // The fake dies right after reporting ready; the supervisor must
        // respawn it in recovery mode exactly max_recovery_attempts times.
        int recoveries = 0;
        for (int i = 0; i < 250; ++i) {
            const auto st =
                rpc_json({{"cmd", "status"}, {"id", "d1"}});
            if (st.value("ok", false)) {
                recoveries = st.value("recoveries", 0);
                if (recoveries >= 2) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        check(recoveries == 2, "recoveries != 2");
        // 1 original spawn + 2 recovery respawns. The third child's argv
        // line lags the recoveries counter (spawn is fork+exec+shell), so
        // poll instead of checking once.
        size_t lines = 0;
        for (int i = 0; i < 250; ++i) {
            lines = count_lines(argv_log);
            if (lines >= 3) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        check(lines == 3, "argv log line count != 3");

        const auto del = rpc_json({{"cmd", "destroy"}, {"id", "d1"}});
        check(del.value("ok", false) == true, "destroy failed");
      } catch (const std::exception& e) {
        check(false, "client RPC threw");
        fail_msg = e.what();
      }
        client_done.store(true);
    });

    std::atomic<int> daemon_rc{-1};
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        supervisor::DaemonConfig cfg;
        cfg.socket_path = sock;
        cfg.global_config = "";  // the fake ignores it
        cfg.device_bin = script;
        cfg.ready_timeout_sec = 5;
        cfg.stop_timeout_sec = 2;
        cfg.max_recovery_attempts = 2;
        elio::go([&]() -> elio::coro::task<void> {
            daemon_rc.store(co_await supervisor::run_daemon(cfg));
        });
        while (!client_done.load()) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(20));
        }
        // Process-directed (NOT raise(), which is thread-directed and a
        // signalfd on another worker thread would never observe it).
        ::kill(::getpid(), SIGTERM);
        while (daemon_rc.load() < 0) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(20));
        }
        co_return 0;
    });
    client.join();
    INFO(fail_msg);
    REQUIRE(rc == 0);
    REQUIRE(daemon_rc.load() >= 0);
    REQUIRE(failures.load() == 0);
    REQUIRE(::sigprocmask(SIG_SETMASK, &prev, nullptr) == 0);
    ::unsetenv("OBD_FAKE_ARGV");
}

namespace {

void check_recovery_observation(const std::string& point) {
    test::TempDir dir;
    const std::string sock = dir / "s.sock";
    const std::string config = test::write_file(
        dir / "config.json", std::vector<uint8_t>{'{', '}'});
    const std::string script = dir / "device.sh";
    // Stay alive until the client explicitly kills this generation. The
    // tiny protocol peer supports trace_start so EOF mutates real trace
    // metadata while a status/list reply is paused. No privileged ublk.
    const std::string content = R"(#!/bin/sh
printf '%s\n' '{"state":"ready","device":"/dev/ublkb7"}' >&3
while IFS= read -r line <&3; do
    seq=$(printf '%s\n' "$line" | sed -n 's/.*"seq":\([0-9][0-9]*\).*/\1/p')
    printf '{"reply":"trace_start","seq":%s,"ok":true,"path":"trace","duration_sec":60}\n' "$seq" >&3
done
)";
    test::write_file(script, std::vector<uint8_t>(content.begin(), content.end()));
    REQUIRE(::chmod(script.c_str(), 0755) == 0);
    sigset_t block, prev;
    ::sigemptyset(&block);
    ::sigaddset(&block, SIGTERM);
    ::sigaddset(&block, SIGINT);
    ::sigaddset(&block, SIGCHLD);
    REQUIRE(::sigprocmask(SIG_BLOCK, &block, &prev) == 0);

    auto gate_owner = std::make_shared<supervisor::detail::DaemonTestGate>();
    auto& gate = *gate_owner;
    std::atomic<bool> done{false};
    std::atomic<int> daemon_rc{-1};
    std::vector<std::string> failures;
    std::string observer_error;
    nlohmann::json paused_reply;
    pid_t original_pid = -1;
    auto check = [&](bool ok, const char* message) {
        if (!ok) failures.emplace_back(message);
    };
    std::thread client([&] {
        std::thread observer;
        auto rpc = [&](const nlohmann::json& command) {
            return nlohmann::json::parse(uds_rpc(sock, command.dump() + "\n"));
        };
        try {
            for (int i = 0; i < 500 && !std::filesystem::exists(sock); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const auto created = rpc({{"cmd", "create"}, {"id", "d1"},
                                      {"config", config}});
            if (!created.value("ok", false)) throw std::runtime_error("create failed");
            const auto started = rpc({{"cmd", "trace_start"}, {"id", "d1"},
                                      {"path", dir / "trace"}, {"duration_sec", 60}});
            if (!started.value("ok", false)) throw std::runtime_error("trace_start failed");
            const auto initial = rpc({{"cmd", "status"}, {"id", "d1"}});
            const auto old_pid = initial.at("pid").get<pid_t>();
            original_pid = old_pid;
            check(initial.at("trace").at("state") == "recording", "initial trace not recording");
            gate.arm(point);
            if (point != "recovery") {
                observer = std::thread([&] {
                    try {
                        paused_reply = rpc({{"cmd", point}, {"id", "d1"}});
                    } catch (const std::exception& e) {
                        observer_error = e.what();
                    }
                });
                if (!gate.wait()) throw std::runtime_error("observer did not reach gate");
            }
            check(::kill(old_pid, SIGKILL) == 0, "could not kill original child");
            if (point == "recovery" && !gate.wait()) {
                throw std::runtime_error("recovery did not reach gate");
            }
            nlohmann::json current;
            for (int i = 0; i < 500; ++i) {
                current = rpc({{"cmd", "status"}, {"id", "d1"}});
                if (current.value("recoveries", 0) == 1 &&
                    (point == "recovery" || current.value("state", "") == "ready")) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            check(current.value("recoveries", 0) == 1, "replacement not published");
            check(current.at("pid") != old_pid, "recovery count paired with old pid");
            check(current.at("trace").at("state") == "lost", "EOF did not update trace");
            if (point == "recovery") {
                auto replacement = gate.child().lock();
                check(static_cast<bool>(replacement), "replacement owner expired");
                const auto listed = rpc({{"cmd", "list"}}).at("devices").at(0);
                if (replacement) {
                    check(current.at("pid") == replacement->pid(), "status observed split publication");
                    check(listed.at("pid") == replacement->pid(), "list observed split publication");
                }
                check(listed.at("recoveries") == 1, "list recovery count missing");
            } else {
                check(!gate.child().expired(), "paused observer lost old child ownership");
            }
            check(gate.concurrent_workers(), "overlap did not execute on distinct workers");
        } catch (const std::exception& e) {
            failures.emplace_back(e.what());
        }
        // Unconditionally release and join before ANY Catch assertion or
        // daemon shutdown, including exception/failure paths above.
        gate.release();
        if (observer.joinable()) observer.join();
        if (!observer_error.empty()) failures.push_back(observer_error);
        if (point != "recovery" && !paused_reply.is_null()) {
            try {
                const auto fields = point == "list"
                    ? paused_reply.at("devices").at(0) : paused_reply;
                check(fields.at("recoveries") == 0, "paused reply mixed recovery generations");
                check(fields.at("state") == "ready", "paused reply lost original status");
                check(fields.at("trace").at("state") == "recording", "paused reply read live trace metadata");
                check(fields.at("pid") == original_pid, "paused reply mixed child pid and status");
            } catch (const std::exception& e) {
                failures.emplace_back(e.what());
            }
        }
        try {
            check(rpc({{"cmd", "destroy"}, {"id", "d1"}}).value("ok", false), "destroy failed");
        } catch (const std::exception& e) {
            failures.emplace_back(e.what());
        }
        done.store(true);
    });
    supervisor::DaemonConfig cfg;
    cfg.socket_path = sock;
    cfg.device_bin = script;
    cfg.global_config = "";
    cfg.max_recovery_attempts = 1;
    cfg.ready_timeout_sec = 3;
    cfg.stop_timeout_sec = 1;
    elio::run_config runtime;
    runtime.num_threads = 4;
    const int rc = elio::run([&]() -> elio::coro::task<int> {
        elio::go_to(0, [&]() -> elio::coro::task<void> {
            daemon_rc.store(co_await supervisor::detail::run_daemon_with_test_gate(cfg, gate_owner));
        });
        while (!done.load()) co_await elio::time::sleep_for(std::chrono::milliseconds(10));
        ::kill(::getpid(), SIGTERM);
        while (daemon_rc.load() < 0) co_await elio::time::sleep_for(std::chrono::milliseconds(10));
        co_return 0;
    }, runtime);
    client.join();
    const int mask_rc = ::sigprocmask(SIG_SETMASK, &prev, nullptr);
    std::string failure_text;
    for (const auto& failure : failures) failure_text += failure + "\n";
    INFO(failure_text);
    CHECK(failures.empty());
    CHECK_FALSE(gate.timeout());
    CHECK(rc == 0);
    CHECK(daemon_rc.load() == 0);
    CHECK(mask_rc == 0);
}

}  // namespace

TEST_CASE("supervisor: recovery publishes child and count together", "[supervisor][recovery-snapshot]") {
    check_recovery_observation("recovery");
}

TEST_CASE("supervisor: status owns its child generation and trace snapshot", "[supervisor][recovery-snapshot]") {
    check_recovery_observation("status");
}

TEST_CASE("supervisor: list owns its child generation and trace snapshot", "[supervisor][recovery-snapshot]") {
    check_recovery_observation("list");
}

namespace {

// These cleanup helpers run only on ordinary fixture threads, never workers.
pid_t read_fixture_pid(const std::string& path,
                       std::chrono::milliseconds grace = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + grace;
    for (;;) {
        std::ifstream input(path);
        char record[32]{};
        input.getline(record, sizeof(record));
        const std::string_view line(record);
        pid_t pid = -1;
        const auto parsed = std::from_chars(line.data(), line.data() + line.size(), pid);
        // getline must have consumed the newline, not just reached EOF in a
        // partially published number. Oversized or malformed records fail too.
        if (input && !input.eof() && parsed.ec == std::errc{} &&
            parsed.ptr == line.data() + line.size() && pid > 0) return pid;
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("fixture PID publication timed out: " + path);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool fixture_child_exited(pid_t pid, std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        const pid_t result = ::waitpid(pid, nullptr, WNOHANG);
        if (result == pid) return true;
        if (result == 0) return false;
        const int error = errno;
        if (error == ECHILD) return true;  // The daemon may have reaped it.
        if (error != EINTR) {
            throw std::system_error(error, std::generic_category(), "fixture waitpid");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("fixture waitpid interrupted until deadline");
        }
    }
}

void terminate_fixture_child(pid_t pid) {
    // Pin the signal target before checking child ownership: the daemon's
    // reaper may collect it between waitpid and signalling. Never signal a
    // numeric PID that could have been recycled in that interval.
    const int pidfd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
    if (pidfd < 0) {
        const int error = errno;
        if (error == ESRCH) return;
        throw std::system_error(error, std::generic_category(), "fixture pidfd_open");
    }
    try {
        if (!fixture_child_exited(pid, std::chrono::steady_clock::now() +
                                       std::chrono::seconds(5))) {
            if (::syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0) < 0 &&
                errno != ESRCH) {
                throw std::system_error(errno, std::generic_category(), "fixture pidfd_send_signal");
            }
        }
    } catch (...) {
        ::close(pidfd);
        throw;
    }
    ::close(pidfd);
}

void reap_fixture_child(pid_t pid,
                        std::chrono::milliseconds grace = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + grace;
    while (!fixture_child_exited(pid, deadline)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("fixture child reap timed out: " + std::to_string(pid));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void check_command_rejection(bool close_channel) {
    test::TempDir dir;
    const std::string sock = dir / "s.sock";
    const std::string config = test::write_file(
        dir / "config.json", std::vector<uint8_t>{'{', '}'});
    const std::string script = dir / "device.sh";
    const std::string pid_file = dir / "child.pid";
    const std::string commands = dir / "commands";
    const std::string control = dir / "control";
    REQUIRE(::mkfifo(control.c_str(), 0600) == 0);
    const std::string content = "#!/bin/sh\necho $$ > \"" + pid_file + "\"\n" +
        "printf '%s\\n' '{\"state\":\"ready\",\"device\":\"/dev/ublkb7\"}' >&3\n"
        "while IFS= read -r line <&3; do\n"
        "    printf '%s\\n' \"$line\" >> \"" + commands + "\"\n"
        "    seq=$(printf '%s\\n' \"$line\" | sed -n 's/.*\"seq\":\\([0-9][0-9]*\\).*/\\1/p')\n"
        "    IFS= read -r action < \"" + control + "\" || exit 1\n"
        "    [ \"$action\" = reply ] || exit 0\n"
        "    printf '{\"reply\":\"resize\",\"seq\":%s,\"ok\":true,\"size\":4096}\\n' \"$seq\" >&3\n"
        "done\n";
    test::write_file(script, std::vector<uint8_t>(content.begin(), content.end()));
    REQUIRE(::chmod(script.c_str(), 0755) == 0);

    // Restore the process signal mask even if runtime setup throws. Peer
    // threads are joined before this guard leaves scope.
    struct SignalMask {
        sigset_t previous{};
        bool active = false;
        int restore() {
            if (!active) return 0;
            const int rc = ::sigprocmask(SIG_SETMASK, &previous, nullptr);
            if (rc == 0) active = false;
            return rc;
        }
        ~SignalMask() { restore(); }
    } mask;
    sigset_t block;
    ::sigemptyset(&block);
    ::sigaddset(&block, SIGTERM);
    ::sigaddset(&block, SIGINT);
    ::sigaddset(&block, SIGCHLD);
    REQUIRE(::sigprocmask(SIG_BLOCK, &block, &mask.previous) == 0);
    mask.active = true;

    auto gate = std::make_shared<supervisor::detail::DaemonTestGate>();
    std::atomic<bool> client_done{false};
    std::atomic<int> daemon_rc{-1};
    std::vector<std::string> failures;
    std::string daemon_error;
    nlohmann::json first_reply, rejected_reply;
    pid_t child_pid = -1;
    auto check = [&](bool ok, const char* message) {
        if (!ok) failures.emplace_back(message);
    };
    auto wait_until = [](auto&& condition) {
        for (int i = 0; i < 500; ++i) {
            if (condition()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return condition();
    };
    std::jthread client([&] {
        std::thread first, rejected;
        std::string first_error, rejected_error;
        int control_fd = -1;
        bool create_sent = false;
        auto rpc = [&](const nlohmann::json& command) {
            return nlohmann::json::parse(uds_rpc(sock, command.dump() + "\n"));
        };
        try {
            // A read/write fixture endpoint makes open and the tiny action
            // write nonblocking even before the child opens its FIFO reader.
            control_fd = ::open(control.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (control_fd < 0) throw std::system_error(errno, std::generic_category(), "fixture FIFO open");
            if (!wait_until([&] { return std::filesystem::exists(sock); })) {
                throw std::runtime_error("listener did not appear");
            }
            create_sent = true;
            const auto created = rpc({{"cmd", "create"}, {"id", "d1"}, {"config", config}});
            if (!created.value("ok", false)) throw std::runtime_error("create failed");
            child_pid = read_fixture_pid(pid_file);
            first = std::thread([&] {
                try {
                    first_reply = rpc({{"cmd", "resize"}, {"id", "d1"}, {"size", 4096}});
                } catch (const std::exception& error) { first_error = error.what(); }
            });
            if (!wait_until([&] { return count_lines(commands) == 1; })) {
                throw std::runtime_error("fake did not receive the first command");
            }
            std::ifstream input(commands);
            std::string line;
            std::getline(input, line);
            const auto forwarded = nlohmann::json::parse(line);
            check(forwarded.at("cmd") == "resize", "wrong forwarded command");
            check(forwarded.at("seq").get<uint64_t>() > 0, "missing forwarding sequence");
            check(forwarded.at("size") == 4096, "wrong forwarded size");
            gate->arm("command_rejected");
            rejected = std::thread([&] {
                try {
                    rejected_reply = rpc({{"cmd", "resize"}, {"id", "d1"}, {"size", 8192}});
                } catch (const std::exception& error) { rejected_error = error.what(); }
            });
            if (!gate->wait()) throw std::runtime_error("rejected handler did not reach gate");
            const std::string action = close_channel ? "eof\n" : "reply\n";
            ssize_t written;
            do { written = ::write(control_fd, action.data(), action.size()); }
            while (written < 0 && errno == EINTR);
            if (written != static_cast<ssize_t>(action.size())) {
                throw std::runtime_error("fixture action write failed");
            }
            // Complete A's real monitor transition and handler cleanup while
            // B stays parked. No later admission can steal A's pending reply.
            first.join();
            if (!first_error.empty()) throw std::runtime_error(first_error);
            if (close_channel) {
                check(!first_reply.value("ok", true), "EOF did not fail first command");
                check(first_reply.value("error", "") == "device control channel closed", "wrong first command EOF error");
            } else {
                check(first_reply.value("ok", false), "matching reply did not complete first command");
                check(first_reply.value("size", 0) == 4096, "first reply lost device fields");
            }
            check(wait_until([&] { return gate->concurrent_workers(); }), "handler and monitor did not overlap on distinct workers");
        } catch (const std::exception& error) {
            failures.emplace_back(error.what());
        }
        // Cleanup is unconditional and independent of daemon RPC service.
        // Killing the fixture also releases A if setup failed before action.
        gate->release();
        if (control_fd >= 0) ::close(control_fd);
        if (create_sent) {
            try {
                if (child_pid <= 0) child_pid = read_fixture_pid(pid_file);
                terminate_fixture_child(child_pid);
            } catch (const std::exception& error) { failures.emplace_back(error.what()); }
        }
        if (first.joinable()) first.join();
        if (rejected.joinable()) rejected.join();
        if (!first_error.empty()) failures.push_back(first_error);
        if (!rejected_error.empty()) failures.push_back(rejected_error);
        check(count_lines(commands) == 1, "rejected command reached the fake device");
        client_done.store(true);
    });
    supervisor::DaemonConfig cfg;
    cfg.socket_path = sock;
    cfg.device_bin = script;
    cfg.global_config = "";
    cfg.ready_timeout_sec = 2;
    cfg.stop_timeout_sec = 1;
    cfg.max_recovery_attempts = 0;
    elio::run_config runtime;
    runtime.num_threads = 4;
    int rc = -1;
    try {
        rc = elio::run([&]() -> elio::coro::task<int> {
            elio::go_to(0, [&]() -> elio::coro::task<void> {
                try {
                    daemon_rc.store(co_await supervisor::detail::run_daemon_with_test_gate(cfg, gate));
                } catch (const std::exception& error) {
                    daemon_error = error.what();
                    daemon_rc.store(1);
                }
            });
            while (!client_done.load()) co_await elio::time::sleep_for(std::chrono::milliseconds(10));
            if (daemon_rc.load() < 0) ::kill(::getpid(), SIGTERM);
            while (daemon_rc.load() < 0) co_await elio::time::sleep_for(std::chrono::milliseconds(10));
            co_return 0;
        }, runtime);
    } catch (const std::exception& error) { daemon_error = error.what(); }
    client.join();
    if (child_pid > 0) {
        try { reap_fixture_child(child_pid); }
        catch (const std::exception& error) { failures.emplace_back(error.what()); }
    }
    const int mask_rc = mask.restore();
    std::string failure_text;
    for (const auto& failure : failures) failure_text += failure + "\n";
    INFO(failure_text);
    INFO(daemon_error);
    CHECK(failures.empty());
    CHECK_FALSE(gate->timeout());
    CHECK(rc == 0);
    CHECK(daemon_rc.load() == 0);
    CHECK(daemon_error.empty());
    CHECK(mask_rc == 0);
    CHECK_FALSE(rejected_reply.value("ok", true));
    CHECK(rejected_reply.value("error", "") == "another device command is in flight: d1");
}

void check_daemon_lifetime(const std::string& point) {
    test::TempDir dir;
    const std::string sock = dir / "s.sock";
    const std::string config = test::write_file(
        dir / "config.json", std::vector<uint8_t>{'{', '}'});
    const std::string script = dir / "device.sh";
    const std::string pid_file = dir / "child.pid";
    const std::string content = "#!/bin/sh\necho $$ > \"" + pid_file + "\"\n" + R"(printf '%s\n' '{"state":"ready","device":"/dev/ublkb7"}' >&3
while IFS= read -r line <&3; do :; done
)";
    test::write_file(script, std::vector<uint8_t>(content.begin(), content.end()));
    REQUIRE(::chmod(script.c_str(), 0755) == 0);
    sigset_t block, prev;
    ::sigemptyset(&block);
    ::sigaddset(&block, SIGTERM);
    ::sigaddset(&block, SIGINT);
    ::sigaddset(&block, SIGCHLD);
    REQUIRE(::sigprocmask(SIG_BLOCK, &block, &prev) == 0);

    auto gate = std::make_shared<supervisor::detail::DaemonTestGate>();
    if (point == "startup_failure") gate->arm(point);
    std::atomic<int> daemon_rc{-1};
    std::atomic<bool> client_done{false};
    std::vector<std::string> failures;
    std::string daemon_error;
    pid_t child_pid = -1;  // Written by client; reused only after its join.
    auto check = [&](bool ok, const char* error) {
        if (!ok) failures.emplace_back(error);
    };
    auto wait_until = [](auto&& condition) {
        for (int i = 0; i < 500; ++i) {
            if (condition()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return condition();
    };
    std::thread client([&] {
        int fd = -1;
        bool signalled = false;
        auto rpc = [&](const nlohmann::json& command) {
            return nlohmann::json::parse(uds_rpc(sock, command.dump() + "\n"));
        };
        try {
            if (!wait_until([&] { return std::filesystem::exists(sock); })) {
                throw std::runtime_error("listener did not appear");
            }
            if (point == "monitor_eof") {
                const auto created = rpc({{"cmd", "create"}, {"id", "d1"},
                                          {"config", config}});
                if (!created.value("ok", false)) throw std::runtime_error("create failed");
                gate->arm(point);
                const auto destroyed = rpc({{"cmd", "destroy"}, {"id", "d1"}});
                check(destroyed.value("ok", false), "destroy did not finish before monitor EOF cleanup");
                if (!gate->wait()) throw std::runtime_error("monitor EOF gate not reached");
                check(rpc({{"cmd", "list"}}).at("devices").empty(), "destroyed entry remains registered");
            } else {
                if (point != "startup_failure") gate->arm(point);
                fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
                if (fd < 0) throw std::system_error(errno, std::generic_category(), "client socket");
                sockaddr_un address{};
                address.sun_family = AF_UNIX;
                std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", sock.c_str());
                if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
                    throw std::system_error(errno, std::generic_category(), "client connect");
                }
                const timeval send_timeout{8, 0};
                if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout)) != 0) {
                    throw std::system_error(errno, std::generic_category(), "fixture send timeout");
                }
                auto send_all = [&](std::string_view bytes) {
                    size_t sent = 0;
                    while (sent < bytes.size()) {
                        const ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
                        if (n < 0) {
                            if (errno == EINTR) continue;
                            throw std::system_error(errno, std::generic_category(), "fixture send");
                        }
                        if (n == 0) throw std::runtime_error("fixture send made no progress");
                        sent += static_cast<size_t>(n);
                    }
                };
                if (point == "command") {
                    const std::string command = nlohmann::json(
                        {{"cmd", "create"}, {"id", "late"}, {"config", config}}).dump() + "\n";
                    send_all(command);
                    if (!gate->wait()) throw std::runtime_error("active create gate not reached");
                } else if (point == "accepted") {
                    if (!gate->wait()) throw std::runtime_error("accept gate not reached");
                } else {
                    if (point == "partial") {
                        send_all("{");
                    }
                    if (!wait_until([&] { return gate->count("client_read") != 0; })) {
                        throw std::runtime_error("accepted handler did not start reading");
                    }
                }
            }
            if (point == "startup_failure") {
                if (!gate->wait()) throw std::runtime_error("startup failure gate not reached");
                gate->release();
            } else {
                check(::kill(::getpid(), SIGTERM) == 0, "cannot signal shutdown");
            }
            signalled = true;
            if (!wait_until([&] { return gate->count("shutdown") != 0; })) {
                throw std::runtime_error("shutdown did not start");
            }
            if (point == "monitor_eof" || point == "command" || point == "accepted") {
                // The held task has genuinely reached a known boundary. A
                // shutdown already observed by the daemon cannot return until
                // that admitted task's final frame/captures have departed.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                check(daemon_rc.load() < 0, "daemon returned with a held task still admitted");
                gate->release();
            }
            check(wait_until([&] { return daemon_rc.load() >= 0; }), "daemon did not drain while client remained open");
            if (fd >= 0) {
                timeval timeout{1, 0};
                if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
                    throw std::system_error(errno, std::generic_category(), "fixture receive timeout");
                }
                char byte;
                ssize_t received;
                do { received = ::recv(fd, &byte, 1, 0); } while (received < 0 && errno == EINTR);
                check(received == 0 || (received < 0 && errno == ECONNRESET), "shutdown left an accepted socket open");
            }
            if (point == "accepted") {
                check(gate->count("client_read") == 1, "late registered handler did not observe cancelled admission");
            }
            if (point == "monitor_eof" || point == "command") {
                check(wait_until([&] { return gate->count("monitor_started") == 1; }), "expected monitor did not start");
                check(gate->count("monitor_departed") == 1, "daemon returned before monitor departure");
            }
        } catch (const std::exception& error) {
            failures.emplace_back(error.what());
        }
        // Failure cleanup never relies on the parked peer to send more data.
        // Release every gate and close the peer before any Catch assertion.
        gate->release();
        if (fd >= 0) ::close(fd);
        // The fixture also owns its fake process independently of the daemon.
        // A broken daemon may have returned before an admitted create spawns
        // it, so cleanup cannot depend on the already-closed control listener.
        if (point == "command" || point == "monitor_eof") {
            try {
                // Redirection creates an empty file before echo publishes PID.
                child_pid = read_fixture_pid(pid_file);
                terminate_fixture_child(child_pid);
            } catch (const std::exception& error) {
                failures.emplace_back(error.what());
            }
        }
        // Releasing the startup gate already injects its shutdown exception;
        // a SIGTERM here could remain unread when the signal mask is restored.
        if (!signalled && point != "startup_failure") ::kill(::getpid(), SIGTERM);
        client_done.store(true);
    });
    supervisor::DaemonConfig cfg;
    cfg.socket_path = sock;
    cfg.device_bin = script;
    cfg.global_config = "";
    cfg.ready_timeout_sec = 2;
    cfg.stop_timeout_sec = 1;
    cfg.max_recovery_attempts = 0;
    elio::run_config runtime;
    runtime.num_threads = 4;
    const int rc = elio::run([&]() -> elio::coro::task<int> {
        elio::go_to(0, [&]() -> elio::coro::task<void> {
            try {
                daemon_rc.store(co_await supervisor::detail::run_daemon_with_test_gate(cfg, gate));
            } catch (const std::exception& error) {
                daemon_error = error.what();
                daemon_rc.store(1);
            }
        });
        while (!client_done.load() || daemon_rc.load() < 0) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(10));
        }
        co_return 0;
    }, runtime);
    client.join();
    // The scheduler has drained every monitor; reap any fake that a broken
    // baseline failed to include in its shutdown registry snapshot.
    if (child_pid > 0) {
        try {
            reap_fixture_child(child_pid);
        } catch (const std::exception& error) {
            failures.emplace_back(error.what());
        }
    }
    const int mask_rc = ::sigprocmask(SIG_SETMASK, &prev, nullptr);
    std::string failure_text;
    for (const auto& failure : failures) failure_text += failure + "\n";
    INFO(failure_text);
    CHECK(failures.empty());
    CHECK_FALSE(gate->timeout());
    CHECK(rc == 0);
    CHECK(daemon_rc.load() == (point == "startup_failure" ? 1 : 0));
    CHECK(daemon_error == (point == "startup_failure" ? "injected daemon startup failure" : ""));
    CHECK(mask_rc == 0);
}

}  // namespace

TEST_CASE("supervisor: rejected command keeps busy reason after pending reply", "[supervisor][command-rejection]") {
    check_command_rejection(false);
}

TEST_CASE("supervisor: rejected command keeps busy reason after channel EOF", "[supervisor][command-rejection]") {
    check_command_rejection(true);
}

TEST_CASE("supervisor: shutdown drains a monitor after its entry was erased", "[supervisor][daemon-lifetime]") {
    check_daemon_lifetime("monitor_eof");
}

TEST_CASE("supervisor: shutdown drains an admitted create and its late monitor", "[supervisor][daemon-lifetime]") {
    check_daemon_lifetime("command");
}

TEST_CASE("supervisor: shutdown drains an accept racing handler registration", "[supervisor][daemon-lifetime]") {
    check_daemon_lifetime("accepted");
}

TEST_CASE("supervisor: shutdown wakes an idle accepted client", "[supervisor][daemon-lifetime]") {
    check_daemon_lifetime("idle");
}

TEST_CASE("supervisor: shutdown wakes a partial accepted command", "[supervisor][daemon-lifetime]") {
    check_daemon_lifetime("partial");
}

TEST_CASE("supervisor: startup failure drains already admitted client work", "[supervisor][daemon-startup-lifetime]") {
    check_daemon_lifetime("startup_failure");
}
