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

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

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

/// Reads the next buffered line as JSON; false on EOF/error.
elio::coro::task<bool> rpc_read_line(int fd, std::string& rbuf,
                                     nlohmann::json& out) {
    char tmp[1024];
    for (;;) {
        if (const auto nl = rbuf.find('\n'); nl != std::string::npos) {
            out = nlohmann::json::parse(rbuf.substr(0, nl));
            rbuf.erase(0, nl + 1);
            co_return true;
        }
        const auto r = co_await elio::io::async_read(fd, tmp, sizeof(tmp), -1);
        if (r.result <= 0) co_return false;
        rbuf.append(tmp, static_cast<size_t>(r.result));
    }
}

}  // namespace

TEST_CASE("supervisor: device trace control answers malformed-typed fields with clean errors",
          "[supervisor]") {
    test::TempDir dir;
    const std::string out = dir / "out.trace";
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);

    nlohmann::json bad_path, bad_dur, bad_float, bad_neg, bad_huge,
        ok_start, ok_stop;
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        auto channel =
            std::make_shared<supervisor::ControlChannelWriter>(fds[1]);
        elio::go([channel, rec]() -> elio::coro::task<void> {
            co_await supervisor::run_device_control(channel, rec);
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
        // Non-integer/out-of-range durations: floats would truncate
        // silently, negatives and huge values wrap in get<uint32_t>
        // (2^40 + 300 would alias to 300) — all must be clean errors.
        nlohmann::json cmd_float = {{"cmd", "trace_start"},
                                    {"path", "/tmp/x.trace"},
                                    {"duration_sec", 1.5},
                                    {"seq", 5}};
        nlohmann::json cmd_neg = {{"cmd", "trace_start"},
                                  {"path", "/tmp/x.trace"},
                                  {"duration_sec", -5},
                                  {"seq", 6}};
        nlohmann::json cmd_huge = {{"cmd", "trace_start"},
                                   {"path", "/tmp/x.trace"},
                                   {"duration_sec", 1099511627776},
                                   {"seq", 7}};
        bad_path = co_await rpc_exchange(fds[0], cmd_bad_path);
        bad_dur = co_await rpc_exchange(fds[0], cmd_bad_dur);
        bad_float = co_await rpc_exchange(fds[0], cmd_float);
        bad_neg = co_await rpc_exchange(fds[0], cmd_neg);
        bad_huge = co_await rpc_exchange(fds[0], cmd_huge);
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
    REQUIRE(bad_float.value("ok", true) == false);
    REQUIRE(bad_float.value("seq", 0) == 5);
    REQUIRE(bad_neg.value("ok", true) == false);
    REQUIRE(bad_neg.value("seq", 0) == 6);
    REQUIRE(bad_huge.value("ok", true) == false);
    REQUIRE(bad_huge.value("seq", 0) == 7);
    REQUIRE(ok_start.value("ok", false) == true);
    REQUIRE(ok_start.value("seq", 0) == 3);
    REQUIRE(ok_stop.value("ok", false) == true);
    REQUIRE(ok_stop.value("seq", 0) == 4);
    REQUIRE(ok_stop.value("records", 1) == 0);  // empty window
}

TEST_CASE("supervisor: control channel writer loops short writes and never throws",
          "[supervisor]") {
    // A single ::write on SOCK_STREAM may write short (buffer
    // pressure), which would truncate/fuse protocol lines. The writer
    // loops until all bytes are out; a hard error is reported (logged
    // + dropped), never thrown. Pin WITHOUT assuming a fixed pipe
    // capacity (it varies across kernels/config): fill the NONBLOCKING
    // pipe to capacity, drain half back, then write a line larger than
    // half the capacity — the first ::write short-writes and the next
    // EAGAINs, so write_line must return false after looping. A normal
    // line over a socketpair succeeds afterwards.
    int pfd[2];
    REQUIRE(::pipe(pfd) == 0);
    const int flags = ::fcntl(pfd[1], F_GETFL);
    REQUIRE(::fcntl(pfd[1], F_SETFL, flags | O_NONBLOCK) == 0);
    size_t cap = 0;
    std::string junk(8192, 'y');
    for (;;) {
        const ssize_t w = ::write(pfd[1], junk.data(), junk.size());
        if (w <= 0) break;  // EAGAIN (or error): pipe full
        cap += static_cast<size_t>(w);
    }
    // Drain half the capacity back so roughly half is free.
    size_t freed = 0;
    while (freed < cap / 2) {
        const ssize_t r = ::read(pfd[0], junk.data(), junk.size());
        if (r <= 0) break;
        freed += static_cast<size_t>(r);
    }
    supervisor::ControlChannelWriter w(pfd[1]);
    const std::string big(cap, 'x');  // > free space by construction
    bool threw = false;
    bool ok = true;
    try {
        ok = w.write_line(big);
    } catch (...) {
        threw = true;
    }
    REQUIRE(!threw);
    REQUIRE(!ok);  // short first write then EAGAIN: reported
    ::close(pfd[0]);
    ::close(pfd[1]);

    int sfd[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sfd) ==
            0);
    supervisor::ControlChannelWriter w2(sfd[1]);
    REQUIRE(w2.write_line(nlohmann::json{{"reply", "ping"}, {"ok", true}}));
    char buf[64];
    const ssize_t r = ::read(sfd[0], buf, sizeof(buf));
    REQUIRE(r > 0);
    REQUIRE(std::string(buf, static_cast<size_t>(r)).find("ping") !=
            std::string::npos);
    ::close(sfd[0]);
    ::close(sfd[1]);
}

TEST_CASE("supervisor: device trace control skips an oversized line and stays alive",
          "[supervisor]") {
    // An oversized command line (> kMaxMessageBytes with no '\n') must be
    // DISCARDED, not mistaken for channel EOF — the pre-fix code returned
    // nullopt and ended the control loop, stranding every later command
    // on a 30 s timeout. After skipping the junk, a valid command still
    // gets its reply on the same channel.
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    test::TempDir dir;
    const std::string out = dir / "out.trace";
    nlohmann::json reply, reply2;
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        auto channel =
            std::make_shared<supervisor::ControlChannelWriter>(fds[1]);
        elio::go([channel, rec]() -> elio::coro::task<void> {
            co_await supervisor::run_device_control(channel, rec);
        });
        // Junk line bigger than the 64 KiB cap (no '\n' inside), then a
        // valid command. The DEVICE reader must discard the oversized
        // line and still serve the command.
        const std::string junk(70 * 1024, 'x');
        const std::string line =
            junk + "\n" +
            (nlohmann::json{{"cmd", "trace_start"},
                            {"path", out},
                            {"duration_sec", 300},
                            {"seq", 1}})
                .dump() +
            "\n";
        const auto w = co_await elio::io::async_write(fds[0], line.data(),
                                                      line.size(), -1);
        if (w.result != static_cast<ssize_t>(line.size())) co_return 1;
        std::string rbuf;
        if (!co_await rpc_read_line(fds[0], rbuf, reply)) co_return 2;
        // Stop the recording: the 300 s expiry timer armed by start is a
        // DETACHED coroutine that must be cancelled by stop() before the
        // scheduler drains, or run_coro teardown hangs.
        const std::string stop =
            (nlohmann::json{{"cmd", "trace_stop"}, {"seq", 2}}).dump() + "\n";
        const auto w2 = co_await elio::io::async_write(fds[0], stop.data(),
                                                       stop.size(), -1);
        if (w2.result != static_cast<ssize_t>(stop.size())) co_return 3;
        if (!co_await rpc_read_line(fds[0], rbuf, reply2)) co_return 4;
        ::shutdown(fds[1], SHUT_RDWR);
        // Give the detached loop ample time to observe the EOF and exit
        // before run_coro returns (a short settle raced teardown and
        // SEGFAULTed — the known live-coroutine-teardown hazard, #28).
        co_await elio::time::sleep_for(300ms);
        co_return 0;
    });
    ::close(fds[0]);
    ::close(fds[1]);
    REQUIRE(rc == 0);
    REQUIRE(reply.value("reply", "") == "trace_start");
    REQUIRE(reply.value("ok", false) == true);
    REQUIRE(reply.value("seq", 0) == 1);
    REQUIRE(reply2.value("reply", "") == "trace_stop");
    REQUIRE(reply2.value("ok", false) == true);
    REQUIRE(reply2.value("seq", 0) == 2);
}

TEST_CASE("supervisor: control channel writer survives a closed peer without SIGPIPE",
          "[supervisor]") {
    // An EPIPE (the supervisor vanishing mid-write) must surface as a
    // dropped line (write_line returns false), NOT SIGPIPE-terminate the
    // device process — a real finalize/event write could race the
    // supervisor's death. Sockets write via send(MSG_NOSIGNAL); this
    // test would die by SIGPIPE under the pre-fix ::write.
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    ::close(fds[0]);  // peer gone: writes now hit EPIPE
    supervisor::ControlChannelWriter w(fds[1]);
    bool threw = false;
    bool ok = true;
    try {
        ok = w.write_line(nlohmann::json{{"reply", "ping"}, {"ok", true}});
    } catch (...) {
        threw = true;
    }
    REQUIRE(!threw);
    REQUIRE(!ok);  // EPIPE reported as a dropped line, process alive
    ::close(fds[1]);
}

TEST_CASE("supervisor: device resize executor grows and rejects shrink or no-op",
          "[supervisor]") {
    // D3 grow-only resize: the device-side executor (run_device_control's
    // "resize" branch) compares the request against the executor seam's
    // current size BEFORE any kernel IO. A grow is applied (once, with
    // the seq echoed and the new size reported); an equal (no-op) or
    // smaller (shrink) request is a clean ok:false reply that never
    // reaches the apply seam. Runs with a recorder-less device, so it
    // also pins the loop's null-recorder tolerance for resize.
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    nlohmann::json grow, equal, shrink, misalign, zero, trace_unavail;
    auto cur = std::make_shared<uint64_t>(512 * 64);   // 32 KiB
    auto calls = std::make_shared<int>(0);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto channel =
            std::make_shared<supervisor::ControlChannelWriter>(fds[1]);
        supervisor::DeviceControlHooks hooks;
        hooks.resize.current_size = [cur]() -> uint64_t { return *cur; };
        hooks.resize.apply_resize = [cur, calls](uint64_t bytes) {
            ++*calls;
            *cur = bytes;
            return bytes;
        };
        elio::go([channel, hooks]() -> elio::coro::task<void> {
            // recorder = nullptr: resize needs no recorder (trace cmds
            // on the same loop still get clean "unavailable" errors).
            co_await supervisor::run_device_control(channel, nullptr, hooks);
        });
        nlohmann::json cmd_grow = {{"cmd", "resize"},
                                   {"size", 65536},
                                   {"seq", 1}};
        nlohmann::json cmd_equal = {{"cmd", "resize"},
                                    {"size", 65536},
                                    {"seq", 2}};
        nlohmann::json cmd_shrink = {{"cmd", "resize"},
                                     {"size", 512 * 64},
                                     {"seq", 3}};
        nlohmann::json cmd_misalign = {{"cmd", "resize"},
                                       {"size", 1000},
                                       {"seq", 4}};
        nlohmann::json cmd_zero = {{"cmd", "resize"},
                                   {"size", 0},
                                   {"seq", 5}};
        nlohmann::json cmd_trace = {{"cmd", "trace_start"},
                                    {"path", "/tmp/x.trace"},
                                    {"duration_sec", 300},
                                    {"seq", 6}};
        grow = co_await rpc_exchange(fds[0], cmd_grow);
        equal = co_await rpc_exchange(fds[0], cmd_equal);
        shrink = co_await rpc_exchange(fds[0], cmd_shrink);
        misalign = co_await rpc_exchange(fds[0], cmd_misalign);
        zero = co_await rpc_exchange(fds[0], cmd_zero);
        trace_unavail = co_await rpc_exchange(fds[0], cmd_trace);
        ::shutdown(fds[1], SHUT_RDWR);
        // Let the detached loop observe the EOF before teardown.
        co_await elio::time::sleep_for(300ms);
        co_return 0;
    });
    ::close(fds[0]);
    ::close(fds[1]);
    REQUIRE(rc == 0);

    // The executor checks are order-independent of the scheduler: assert
    // the exact semantics via the reply shapes.
    REQUIRE(grow.value("reply", "") == "resize");
    REQUIRE(grow.value("ok", false) == true);
    REQUIRE(grow.value("size", 0) == 65536);
    REQUIRE(grow.value("seq", 0) == 1);
    REQUIRE(*calls == 1);  // the grow reached the apply seam exactly once
    REQUIRE(*cur == 65536);
    REQUIRE(equal.value("reply", "") == "resize");
    REQUIRE(equal.value("ok", true) == false);
    REQUIRE(equal.value("error", "").find("grow-only") !=
            std::string::npos);
    REQUIRE(equal.value("seq", 0) == 2);
    REQUIRE(shrink.value("reply", "") == "resize");
    REQUIRE(shrink.value("ok", true) == false);
    REQUIRE(shrink.value("error", "").find("grow-only") !=
            std::string::npos);
    REQUIRE(shrink.value("seq", 0) == 3);
    REQUIRE(misalign.value("reply", "") == "resize");
    REQUIRE(misalign.value("ok", true) == false);
    REQUIRE(misalign.value("error", "").find("multiple of 512") !=
            std::string::npos);
    REQUIRE(misalign.value("seq", 0) == 4);
    // A zero-byte request is pinned too: positive-multiple-of-512 rule
    // rejects it BEFORE the grow-only comparison.
    REQUIRE(zero.value("reply", "") == "resize");
    REQUIRE(zero.value("ok", true) == false);
    REQUIRE(zero.value("error", "").find("multiple of 512") !=
            std::string::npos);
    REQUIRE(zero.value("seq", 0) == 5);
    // The loop is recorder-less: trace commands are answered, never
    // dereferenced (never-throws contract).
    REQUIRE(trace_unavail.value("reply", "") == "trace_start");
    REQUIRE(trace_unavail.value("ok", true) == false);
    REQUIRE(trace_unavail.value("error", "").find("unavailable") !=
            std::string::npos);
}

TEST_CASE("supervisor: device resize answers unsupported without a seam and survives malformed sizes",
          "[supervisor]") {
    // Two properties on one loop: (1) a device without a resize executor
    // seam answers resize with a clean "unsupported" error; (2) wrong
    // field TYPES (float / negative size) are clean error replies — the
    // never-throws contract (a type_error escaping would strand every
    // later command on the supervisor's 30 s timeout).
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    nlohmann::json unsup, bad_float, bad_neg, ok_grow;
    auto cur = std::make_shared<uint64_t>(512 * 64);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto channel =
            std::make_shared<supervisor::ControlChannelWriter>(fds[1]);
        // The resize executor is created on the SECOND loop below; this
        // first loop runs with hooks but no resize seam (unsupported).
        elio::go([channel]() -> elio::coro::task<void> {
            co_await supervisor::run_device_control(channel, nullptr, {});
        });
        nlohmann::json cmd_unsup = {{"cmd", "resize"},
                                    {"size", 65536},
                                    {"seq", 1}};
        unsup = co_await rpc_exchange(fds[0], cmd_unsup);
        ::shutdown(fds[1], SHUT_RDWR);
        co_await elio::time::sleep_for(200ms);
        co_return 0;
    });
    ::close(fds[0]);
    ::close(fds[1]);
    REQUIRE(rc == 0);
    REQUIRE(unsup.value("reply", "") == "resize");
    REQUIRE(unsup.value("ok", true) == false);
    REQUIRE(unsup.value("error", "").find("unsupported") !=
            std::string::npos);

    // Second loop: a seam IS present; malformed sizes get clean errors
    // and the loop stays alive for a valid grow.
    int fds2[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds2) == 0);
    const int rc2 = test::run_coro([&]() -> elio::coro::task<int> {
        auto channel =
            std::make_shared<supervisor::ControlChannelWriter>(fds2[1]);
        supervisor::DeviceControlHooks hooks;
        hooks.resize.current_size = [cur]() -> uint64_t { return *cur; };
        hooks.resize.apply_resize = [cur](uint64_t bytes) {
            *cur = bytes;
            return bytes;
        };
        elio::go([channel, hooks]() -> elio::coro::task<void> {
            co_await supervisor::run_device_control(channel, nullptr, hooks);
        });
        nlohmann::json cmd_float = {{"cmd", "resize"},
                                    {"size", 65536.5},
                                    {"seq", 2}};
        nlohmann::json cmd_neg = {{"cmd", "resize"},
                                  {"size", -1},
                                  {"seq", 3}};
        nlohmann::json cmd_missing = {{"cmd", "resize"}, {"seq", 4}};
        nlohmann::json cmd_grow = {{"cmd", "resize"},
                                   {"size", 65536},
                                   {"seq", 5}};
        bad_float = co_await rpc_exchange(fds2[0], cmd_float);
        bad_neg = co_await rpc_exchange(fds2[0], cmd_neg);
        const auto missing = co_await rpc_exchange(fds2[0], cmd_missing);
        bad_neg["_missing"] = missing.value("error", "");
        ok_grow = co_await rpc_exchange(fds2[0], cmd_grow);
        ::shutdown(fds2[1], SHUT_RDWR);
        co_await elio::time::sleep_for(300ms);
        co_return 0;
    });
    ::close(fds2[0]);
    ::close(fds2[1]);
    REQUIRE(rc2 == 0);
    REQUIRE(bad_float.value("reply", "") == "resize");
    REQUIRE(bad_float.value("ok", true) == false);
    REQUIRE(bad_float.value("error", "").find("non-negative integer") !=
            std::string::npos);
    REQUIRE(bad_float.value("seq", 0) == 2);
    REQUIRE(bad_neg.value("reply", "") == "resize");
    REQUIRE(bad_neg.value("ok", true) == false);
    REQUIRE(bad_neg.value("seq", 0) == 3);
    // Missing 'size': also a clean error (never an exception).
    REQUIRE(bad_neg.value("_missing", "").find("non-negative integer") !=
            std::string::npos);
    REQUIRE(ok_grow.value("reply", "") == "resize");
    REQUIRE(ok_grow.value("ok", false) == true);
    REQUIRE(ok_grow.value("size", 0) == 65536);
    REQUIRE(ok_grow.value("seq", 0) == 5);
}

TEST_CASE("supervisor: resize executor grows the data plane first and rejects during shutdown",
          "[supervisor]") {
    // FIX A regression, deterministic (no device, no kernel):
    // make_resize_apply is the SAME closure the real obd-device installs
    // as its resize seam. It must
    //   (1) reject ONCE the device began its graceful shutdown — without
    //       calling either grow — because a header rewrite after the
    //       shutdown checkpoint wrote its trailer leaves the upper
    //       uncommittable (header/trailer virtual-size mismatch);
    //   (2) grow the writable data plane BEFORE the kernel gendisk;
    //   (3) surface a data-plane grow failure without touching the
    //       kernel, and a kernel failure without swallowing it.
    auto stopping = std::make_shared<std::atomic<bool>>(false);
    auto gate = std::make_shared<std::mutex>();
    std::vector<std::string> order;
    auto data_plane = [&order](uint64_t) {
        order.push_back("data-plane");
        return 0;
    };
    auto device_grow = [&order](uint64_t bytes) -> uint64_t {
        order.push_back("device");
        return bytes;
    };

    // (2) Ordering: data plane first, then the kernel.
    auto apply = supervisor::make_resize_apply(stopping, gate, data_plane,
                                               device_grow);
    REQUIRE(apply(65536) == 65536);
    REQUIRE(order == std::vector<std::string>({"data-plane", "device"}));

    // A read-only image passes no data-plane grow: the kernel alone is
    // grown (and the request still succeeds).
    order.clear();
    auto apply_ro = supervisor::make_resize_apply(
        stopping, gate, std::function<int(uint64_t)>(), device_grow);
    REQUIRE(apply_ro(131072) == 131072);
    REQUIRE(order == std::vector<std::string>({"device"}));

    // (3a) Data-plane failure: the kernel is NOT touched.
    order.clear();
    auto data_plane_fail = [&order](uint64_t) {
        order.push_back("data-plane");
        return -EINVAL;
    };
    auto apply_dp_fail = supervisor::make_resize_apply(
        stopping, gate, data_plane_fail, device_grow);
    bool threw = false;
    std::string what;
    try {
        apply_dp_fail(262144);
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    REQUIRE(threw);
    REQUIRE(order == std::vector<std::string>({"data-plane"}));
    REQUIRE(what.find("data plane grow") != std::string::npos);

    // (3b) Kernel failure propagates (never swallowed, so the loop
    // replies ok:false instead of claiming success).
    order.clear();
    auto device_fail = [&order](uint64_t) -> uint64_t {
        order.push_back("device");
        // Kernel ENOTSUPP (524) — the pre-6.15 ublk control-dispatch
        // default for an unknown command; not in glibc's <errno.h>.
        throw std::system_error(524, std::generic_category(),
                                "no UPDATE_SIZE");
        return 0;
    };
    auto apply_dev_fail = supervisor::make_resize_apply(
        stopping, gate, data_plane, device_fail);
    threw = false;
    try {
        apply_dev_fail(524288);
    } catch (const std::system_error&) {
        threw = true;
    }
    REQUIRE(threw);
    REQUIRE(order == std::vector<std::string>({"data-plane", "device"}));

    // (1) Shutdown rejection: NEITHER grow runs, and the error is the
    // documented "device is shutting down" (ECANCELED).
    stopping->store(true, std::memory_order_release);
    order.clear();
    auto apply_stopping = supervisor::make_resize_apply(
        stopping, gate, data_plane, device_grow);
    threw = false;
    what.clear();
    int code = 0;
    try {
        apply_stopping(1048576);
    } catch (const std::system_error& e) {
        threw = true;
        what = e.what();
        code = e.code().value();
    }
    REQUIRE(threw);
    REQUIRE(order.empty());  // no header rewrite after the checkpoint
    REQUIRE(what.find("shutting down") != std::string::npos);
    REQUIRE(code == ECANCELED);

    // The gate serializes applies: two concurrent grows never overlap
    // (the shutdown path relies on this to drain an in-flight grow
    // before it checkpoints).
    auto overlap_gate = std::make_shared<std::mutex>();
    auto stopping2 = std::make_shared<std::atomic<bool>>(false);
    std::atomic<int> in_grow{0};
    std::atomic<int> max_in_grow{0};
    auto slow_data_plane = [&in_grow, &max_in_grow](uint64_t) {
        const int now = in_grow.fetch_add(1) + 1;
        int seen = max_in_grow.load();
        while (now > seen &&
               !max_in_grow.compare_exchange_weak(seen, now)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        in_grow.fetch_sub(1);
        return 0;
    };
    auto apply_gated = supervisor::make_resize_apply(
        stopping2, overlap_gate, slow_data_plane, device_grow);
    std::thread t1([&] { apply_gated(65536); });
    std::thread t2([&] { apply_gated(131072); });
    t1.join();
    t2.join();
    REQUIRE(max_in_grow.load() == 1);

    // A malformed construction (no stopping flag / no gate / no device
    // grow) is refused loudly instead of silently accepting resizes.
    bool ctor_threw = false;
    try {
        (void)supervisor::make_resize_apply(nullptr, gate, data_plane,
                                            device_grow);
    } catch (const std::invalid_argument&) {
        ctor_threw = true;
    }
    REQUIRE(ctor_threw);
}
