// Integration test: the supervisor `hello` handshake over the real control
// socket, plus the "bad input is answered, never dropped" guarantee. No
// device is created, so no ublk/kernel facility is needed.
#include "supervisor/daemon.hpp"
#include "supervisor/protocol.hpp"

#include <elio/runtime/spawn.hpp>
#include <elio/time/timer.hpp>

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
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

/// Restores the previous signal mask on destruction, however the test
/// exits — including a failed Catch2 assertion, which unwinds via
/// exception. (A trailing sigprocmask restore would be skipped by an
/// earlier failure and leave SIGTERM/SIGINT/SIGCHLD blocked for the rest
/// of the test process.)
struct SigMaskGuard {
    sigset_t prev {};
    ~SigMaskGuard() { ::sigprocmask(SIG_SETMASK, &prev, nullptr); }
};

}  // namespace

TEST_CASE("supervisor: daemon answers hello and never drops bad input",
          "[supervisor]") {
    test::TempDir dir;
    const std::string sock = dir / "supervisor.sock";

    // run_daemon requires signals blocked process-wide (signalfd model).
    // The guard restores the previous mask on every exit path.
    sigset_t block;
    ::sigemptyset(&block);
    ::sigaddset(&block, SIGTERM);
    ::sigaddset(&block, SIGINT);
    ::sigaddset(&block, SIGCHLD);
    SigMaskGuard mask_guard;
    REQUIRE(::sigprocmask(SIG_BLOCK, &block, &mask_guard.prev) == 0);

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

        // The documented hello shape: protocol integer >= 1, non-empty
        // version string, features array; extra request fields ignored.
        // contains() first: operator[] on a missing key is UB, and a
        // broken implementation must fail the test, not crash it.
        const auto hello =
            rpc_json({{"cmd", "hello"}, {"future_field", 42}});
        check(hello.value("ok", false) == true, "hello not ok");
        check(hello.contains("protocol") &&
                  hello["protocol"].is_number_integer() &&
                  hello["protocol"].get<int>() >= 1,
              "hello protocol not an integer >= 1");
        check(hello.contains("version") && hello["version"].is_string() &&
                  !hello["version"].get<std::string>().empty(),
              "hello version not a non-empty string");
        check(hello.contains("features") && hello["features"].is_array(),
              "hello features not an array");

        // Unknown cmd is answered with an error, never dropped.
        const auto unknown = rpc_json({{"cmd", "bogus"}});
        check(unknown.value("ok", true) == false, "unknown cmd not an error");
        check(unknown.contains("error") &&
                  unknown["error"].get<std::string>().find("unknown cmd") !=
                      std::string::npos,
              "unknown cmd error text mismatch");

        // Malformed JSON is answered with an error too.
        const auto malformed =
            nlohmann::json::parse(uds_rpc(sock, "not json\n"));
        check(malformed.value("ok", true) == false,
              "malformed JSON not an error");
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
        cfg.global_config = "";
        cfg.device_bin = "/nonexistent-obd-device";  // never spawned here
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
}
