// Integration test: supervisor crash recovery via ublk USER_RECOVERY
// (ADR-0010) — pure process choreography with a fake obd-device; no
// kernel ublk needed (the recovery handshake itself is covered by the
// privileged ublk E2E, which self-skips without /dev/ublk-control).
#include "supervisor/daemon.hpp"
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
#include <sys/un.h>

#include <atomic>
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
    sockaddr_un sa {};
    sa.sun_family = AF_UNIX;
    std::snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path.c_str());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        const int e = errno;
        ::close(fd);
        throw std::system_error(e, std::generic_category());
    }
    if (::write(fd, line.data(), line.size()) !=
        static_cast<ssize_t>(line.size())) {
        const int e = errno;
        ::close(fd);
        throw std::system_error(e, std::generic_category());
    }
    std::string reply;
    char buf[4096];
    for (;;) {
        const ssize_t r = ::read(fd, buf, sizeof(buf));
        if (r < 0) {
            const int e = errno;
            ::close(fd);
            throw std::system_error(e, std::generic_category());
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
