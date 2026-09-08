// Integration test: the supervisor `commit` command (ADR-0014) over the
// real control socket — a live device is stopped, then its checkpointed
// LSMT-RW upper is sealed offline by the supervisor process. A fake
// obd-device provides the process choreography; no ublk/kernel facility
// is needed (the upper is written and checkpointed by the test itself,
// standing in for the device's graceful-shutdown checkpoint).
#include "format/lsmt.hpp"
#include "format/lsmt_rw.hpp"
#include "supervisor/daemon.hpp"
#include "supervisor/protocol.hpp"

#include <elio/runtime/spawn.hpp>
#include <elio/time/timer.hpp>

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

TEST_CASE("supervisor: commit stops the device and seals its upper offline",
          "[supervisor]") {
    test::TempDir dir;
    const std::string sock = dir / "supervisor.sock";

    // The upper the "device" would have written: an LSMT-RW file with one
    // data write, checkpointed (what obd-device does on graceful
    // shutdown). The commit must seal exactly this content.
    const std::string upper_dir = dir / "upper";
    std::filesystem::create_directories(upper_dir);
    const std::string upper = upper_dir + "/overlaybd.rw";
    const auto payload = test::pattern_bytes(512 * 16, 4242);
    REQUIRE(test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtRwLayer::create(upper, 512 * 64);
        ssize_t r = co_await layer->pwrite(payload.data(), payload.size(),
                                           0);
        REQUIRE(r == static_cast<ssize_t>(payload.size()));
        const int crc = co_await layer->checkpoint();
        REQUIRE(crc == 0);
        co_return 0;
    }) == 0);

    // Image configs: only the `upper` section matters to commit; the fake
    // device never assembles an image.
    auto write_config = [&](const std::string& name,
                            const nlohmann::json& j) {
        const std::string text = j.dump();
        return test::write_file(
            dir / name, std::vector<uint8_t>(text.begin(), text.end()));
    };
    const std::string cfg_lsmt = write_config(
        "config-lsmt.json",
        {{"upper", {{"dir", upper_dir}, {"type", "lsmt"}}}});
    const std::string cfg_sparse = write_config(
        "config-sparse.json",
        {{"upper", {{"dir", upper_dir}, {"type", "sparse"}}}});
    const std::string cfg_plain = test::write_file(
        dir / "config-plain.json", std::vector<uint8_t>{'{', '}'});

    // Fake obd-device: reports ready on fd 3, then stays alive briefly.
    // It cannot die from the supervisor's SIGTERM itself: the test blocks
    // SIGTERM process-wide (signalfd model) and forked children inherit
    // the blocked mask, so a shell trap would never fire. The 2 s grace is
    // far longer than the client needs to issue commit, so commit still
    // catches the device LIVE: it SIGTERMs the child and its bounded reap
    // completes when the fake exits on its own — proving the upper is
    // never sealed while the device process is alive.
    const std::string script = dir / "fake-device.sh";
    {
        const std::string content =
            "#!/bin/sh\n"
            "echo '{\"state\":\"ready\",\"device\":\"/dev/ublkb7\"}' >&3\n"
            "sleep 2\n"
            "exit 0\n";
        test::write_file(script,
                         std::vector<uint8_t>(content.begin(), content.end()));
        REQUIRE(::chmod(script.c_str(), 0755) == 0);
    }

    // run_daemon requires signals blocked process-wide (signalfd model).
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

        // Unknown id is a clear error.
        const auto unknown =
            rpc_json({{"cmd", "commit"}, {"id", "ghost"}});
        check(unknown.value("ok", true) == false,
              "commit on unknown id not an error");
        check(unknown.contains("error") &&
                  unknown["error"].get<std::string>().find("no such device") !=
                      std::string::npos,
              "unknown id error text mismatch");

        // Three devices: lsmt upper (committable), sparse upper (never
        // seals, upstream parity), no upper.
        check(rpc_json({{"cmd", "create"},
                        {"id", "d1"},
                        {"config", cfg_lsmt}})
                  .value("ok", false),
              "create d1 failed");
        check(rpc_json({{"cmd", "create"},
                        {"id", "d2"},
                        {"config", cfg_sparse}})
                  .value("ok", false),
              "create d2 failed");
        check(rpc_json({{"cmd", "create"},
                        {"id", "d3"},
                        {"config", cfg_plain}})
                  .value("ok", false),
              "create d3 failed");

        const auto sparse =
            rpc_json({{"cmd", "commit"}, {"id", "d2"}});
        check(sparse.value("ok", true) == false,
              "commit on sparse upper not an error");
        check(sparse.contains("error") &&
                  sparse["error"].get<std::string>().find("sparse") !=
                      std::string::npos,
              "sparse error text mismatch");
        const auto no_upper =
            rpc_json({{"cmd", "commit"}, {"id", "d3"}});
        check(no_upper.value("ok", true) == false,
              "commit without upper not an error");
        check(no_upper.contains("error") &&
                  no_upper["error"].get<std::string>().find(
                      "no writable upper") != std::string::npos,
              "no-upper error text mismatch");

        // The main path: commit a LIVE device — the documented contract is
        // stop-then-seal, so the reply proves the device was stopped.
        const auto commit = rpc_json({{"cmd", "commit"},
                                      {"id", "d1"},
                                      {"user_tag", "v1"}});
        check(commit.value("ok", false) == true, "commit d1 failed");
        check(commit.contains("path") &&
                  commit["path"].get<std::string>() == upper,
              "commit path mismatch");
        check(commit.contains("sha256") &&
                  commit["sha256"].get<std::string>().size() == 64,
              "commit sha256 not 64 hex chars");
        check(commit.contains("size") &&
                  commit["size"].get<uint64_t>() > 0,
              "commit size not positive");

        // The device was stopped by commit (bounded poll, no fixed sleep).
        std::string state;
        for (int i = 0; i < 250; ++i) {
            const auto st =
                rpc_json({{"cmd", "status"}, {"id", "d1"}});
            if (st.value("ok", false)) {
                state = st.value("state", "");
                if (state == "exited") break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        check(state == "exited", "device not stopped by commit");

        // A second commit is a clear error: the upper is already sealed.
        const auto again =
            rpc_json({{"cmd", "commit"}, {"id", "d1"}});
        check(again.value("ok", true) == false,
              "second commit not an error");
        check(again.contains("error") &&
                  again["error"].get<std::string>().find("already sealed") !=
                      std::string::npos,
              "already-sealed error text mismatch");
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
        cfg.device_bin = script;
        cfg.ready_timeout_sec = 5;
        cfg.stop_timeout_sec = 5;
        // The fakes exit on their own; that must NOT trigger ADR-0010
        // recovery respawns (they would keep the monitors — and with a
        // 2 s fake the respawn loop — alive past the test's end).
        cfg.max_recovery_attempts = 0;
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

    // The sealed output re-opens as a valid standard LSMT RO layer with
    // the checkpointed content (data region starts at sector 8).
    REQUIRE(test::run_coro([&]() -> elio::coro::task<int> {
        auto ro = co_await source::LocalFileSource::open(upper);
        source::BlobSourcePtr base = std::move(ro);
        auto layer = co_await format::LsmtLayer::open(std::move(base));
        REQUIRE(layer->virtual_size() == 512 * 64);
        REQUIRE(layer->segments().size() == 1);
        REQUIRE(layer->header().user_tag == "v1");
        std::vector<uint8_t> buf(payload.size());
        const ssize_t r = co_await layer->data_source().pread(
            buf.data(), buf.size(), 8 * 512);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(std::memcmp(buf.data(), payload.data(), buf.size()) == 0);
        co_return 0;
    }) == 0);
}
