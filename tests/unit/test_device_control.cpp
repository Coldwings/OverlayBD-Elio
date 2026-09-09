// Unit tests for the device-side trace command loop
// (src/supervisor/device_control.hpp): the loop answers parsed commands
// with missing or wrong-typed fields with a CLEAN ERROR REPLY — never
// an exception escaping the coroutine (the never-throws contract: an
// escaping type_error from json::value() would kill the detached loop
// and silently strand every later command on a 30 s supervisor
// timeout). Drives the loop over a real socketpair.
#include "supervisor/device_control.hpp"

#include "../support.hpp"

#include <elio/io/io_awaitables.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/time/timer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <unistd.h>

#include <optional>
#include <string>

using namespace obd;
using namespace std::chrono_literals;

namespace {

/// Writes one command line and reads one reply line (coroutine
/// context). No Catch2 macros here.
elio::coro::task<nlohmann::json> rpc_exchange(int fd, const nlohmann::json& cmd) {
    const std::string line = cmd.dump() + "\n";
    const auto w = co_await elio::io::async_write(fd, line.data(),
                                                  line.size(), -1);
    if (w.result != static_cast<ssize_t>(line.size())) {
        co_return nlohmann::json{{"transport_error", true}};
    }
    std::string buf;
    char tmp[1024];
    for (;;) {
        if (const auto nl = buf.find('\n'); nl != std::string::npos) {
            co_return nlohmann::json::parse(buf.substr(0, nl));
        }
        const auto r = co_await elio::io::async_read(fd, tmp, sizeof(tmp),
                                                     -1);
        if (r.result <= 0) co_return nlohmann::json{{"transport_eof", true}};
        buf.append(tmp, static_cast<size_t>(r.result));
    }
}

}  // namespace

TEST_CASE("supervisor: device trace control answers malformed-typed fields with clean errors",
          "[supervisor]") {
    test::TempDir dir;
    const std::string out = dir / "out.trace";
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);

    nlohmann::json bad_path, bad_dur, ok_start, ok_stop;
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        elio::go([fd = fds[1], rec]() -> elio::coro::task<void> {
            co_await supervisor::run_trace_control(fd, rec);
        });
        // Wrong-typed path / duration_sec: clean error replies (the
        // pre-fix code threw type_error out of the loop here). Named
        // locals: a brace-init json temporary as a coroutine argument
        // trips GCC 12's "array used as initializer" bug.
        nlohmann::json cmd_bad_path = {{"cmd", "trace_start"},
                                       {"path", 123},
                                       {"duration_sec", 60},
                                       {"seq", 1}};
        nlohmann::json cmd_bad_dur = {{"cmd", "trace_start"},
                                      {"path", "/tmp/x.trace"},
                                      {"duration_sec", "60"},
                                      {"seq", 2}};
        nlohmann::json cmd_start = {{"cmd", "trace_start"},
                                    {"path", out},
                                    {"duration_sec", 300},
                                    {"seq", 3}};
        nlohmann::json cmd_stop = {{"cmd", "trace_stop"}, {"seq", 4}};
        bad_path = co_await rpc_exchange(fds[0], cmd_bad_path);
        bad_dur = co_await rpc_exchange(fds[0], cmd_bad_dur);
        // The loop is still alive: a valid start/stop cycle works.
        ok_start = co_await rpc_exchange(fds[0], cmd_start);
        ok_stop = co_await rpc_exchange(fds[0], cmd_stop);
        // EOF ends the loop before teardown (no parked reader).
        ::shutdown(fds[1], SHUT_RDWR);
        co_await elio::time::sleep_for(20ms);
        co_return 0;
    });
    ::close(fds[0]);
    ::close(fds[1]);
    REQUIRE(rc == 0);

    REQUIRE(bad_path.value("reply", "") == "trace_start");
    REQUIRE(bad_path.value("ok", true) == false);
    REQUIRE(bad_path.value("error", "").find("requires") !=
            std::string::npos);
    REQUIRE(bad_path.value("seq", 0) == 1);  // correlation echoed
    REQUIRE(bad_dur.value("ok", true) == false);
    REQUIRE(bad_dur.value("seq", 0) == 2);
    REQUIRE(ok_start.value("ok", false) == true);
    REQUIRE(ok_start.value("seq", 0) == 3);
    REQUIRE(ok_stop.value("ok", false) == true);
    REQUIRE(ok_stop.value("seq", 0) == 4);
    REQUIRE(ok_stop.value("records", 1) == 0);  // empty window
}
