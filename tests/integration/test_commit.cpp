// Integration test: the supervisor `commit` command (ADR-0014) over the
// real control socket — a live device is stopped, then its LSMT-RW upper
// is sealed offline by the supervisor process.
//
// The device is played by obd-test-fake-device (tests/fake_device_main.cpp),
// a small signalfd-based binary that speaks the real fd-3 lifecycle
// protocol and — exactly like the real obd-device — checkpoints its
// LSMT-RW upper only when SIGTERM arrives. This is what pins the
// stop-then-seal contract: the upper's on-disk checkpoint exists only
// after the supervisor's SIGTERM, so a commit that sealed without stopping
// the device first finds no checkpoint and fails. (A shell fake cannot do
// this: the tests block SIGTERM process-wide for the daemon's signalfd
// model and forked children inherit the blocked mask, so a shell trap
// never fires — the signalfd fake consumes the pending signal just like
// the real device.) No ublk/kernel facility is needed.
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
#include <functional>
#include <string>
#include <system_error>
#include <thread>

using namespace obd;
using namespace std::chrono_literals;

#ifndef OBD_TEST_FAKE_DEVICE_BIN
#define OBD_TEST_FAKE_DEVICE_BIN "obd-test-fake-device"
#endif

namespace {

/// Must match tests/fake_device_main.cpp (the payload the fake writes
/// before it can be checkpointed).
constexpr uint64_t kFakeVsize = 512 * 64;
constexpr uint32_t kFakePayloadSeed = 4242;
constexpr size_t kFakePayloadBytes = 512 * 16;

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

/// Blocks the daemon's signalfd signals process-wide; the returned guard
/// restores the previous mask. run_daemon requires them blocked.
[[nodiscard]] SigMaskGuard block_daemon_signals() {
    sigset_t block;
    ::sigemptyset(&block);
    ::sigaddset(&block, SIGTERM);
    ::sigaddset(&block, SIGINT);
    ::sigaddset(&block, SIGCHLD);
    SigMaskGuard guard;
    REQUIRE(::sigprocmask(SIG_BLOCK, &block, &guard.prev) == 0);
    return guard;
}

/// Drives a real supervisor daemon while `client_body(sock, check)` runs
/// on a helper thread (no Catch2 macros in there), then SIGTERMs the
/// daemon. Returns the number of failed client checks. Catch2 assertions
/// happen here, on the calling thread.
/// Client-side check recorder (concrete std::function type: a generic
/// lambda parameter would turn nlohmann::json member-template calls in
/// the client lambda dependent).
using CheckFn = std::function<void(bool, const char*)>;

template <typename F>
int run_daemon_case(supervisor::DaemonConfig cfg, F&& client_body) {
    std::atomic<bool> client_done{false};
    std::atomic<int> failures{0};
    std::string fail_msg;
    CheckFn check = [&](bool cond, const char* what) {
        if (!cond) {
            failures.fetch_add(1);
            fail_msg = what;
            // stderr, not INFO: Catch2 assertion/INFO state is not
            // thread-safe, and a scoped INFO would expire before the
            // REQUIRE that reports this failure runs.
            std::fprintf(stderr, "[client check failed] %s\n", what);
        }
    };
    std::thread client([&] {
        try {
            client_body(cfg.socket_path, check);
        } catch (const std::exception& e) {
            check(false, "client RPC threw");
            fail_msg = e.what();
        }
        client_done.store(true);
    });

    std::atomic<int> daemon_rc{-1};
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        elio::go([&]() -> elio::coro::task<void> {
            daemon_rc.store(co_await supervisor::run_daemon(cfg));
        });
        while (!client_done.load()) {
            co_await elio::time::sleep_for(20ms);
        }
        // Process-directed (NOT raise(), which is thread-directed and a
        // signalfd on another worker thread would never observe it).
        ::kill(::getpid(), SIGTERM);
        while (daemon_rc.load() < 0) {
            co_await elio::time::sleep_for(20ms);
        }
        co_return 0;
    });
    client.join();
    INFO(fail_msg);
    REQUIRE(rc == 0);
    REQUIRE(daemon_rc.load() >= 0);
    return failures.load();
}

supervisor::DaemonConfig commit_test_cfg(const std::string& sock) {
    supervisor::DaemonConfig cfg;
    cfg.socket_path = sock;
    cfg.global_config = "";
    cfg.device_bin = OBD_TEST_FAKE_DEVICE_BIN;
    cfg.ready_timeout_sec = 5;
    cfg.stop_timeout_sec = 5;
    // The fakes exit only on SIGTERM; an unexpected fake death must not
    // trigger ADR-0010 recovery respawns (they would re-truncate the
    // upper and confuse the assertions).
    cfg.max_recovery_attempts = 0;
    return cfg;
}

std::string write_config(const test::TempDir& dir, const std::string& name,
                         const nlohmann::json& j) {
    const std::string text = j.dump();
    return test::write_file(dir / name,
                            std::vector<uint8_t>(text.begin(), text.end()));
}

/// The sealed output must re-open as a valid standard LSMT RO layer with
/// the fake's payload (data region starts at sector 8). Asserting the
/// payload pins stop-then-seal: the payload reaches the sealed file only
/// through the fake's SIGTERM-triggered checkpoint.
void require_sealed_payload(const std::string& upper,
                            const std::string& user_tag) {
    REQUIRE(test::run_coro([&]() -> elio::coro::task<int> {
        auto ro = co_await source::LocalFileSource::open(upper);
        source::BlobSourcePtr base = std::move(ro);
        auto layer = co_await format::LsmtLayer::open(std::move(base));
        REQUIRE(layer->virtual_size() == kFakeVsize);
        REQUIRE(layer->segments().size() == 1);
        REQUIRE(layer->header().user_tag == user_tag);
        const auto payload =
            test::pattern_bytes(kFakePayloadBytes, kFakePayloadSeed);
        std::vector<uint8_t> buf(payload.size());
        const ssize_t r = co_await layer->data_source().pread(
            buf.data(), buf.size(), 8 * 512);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(std::memcmp(buf.data(), payload.data(), buf.size()) == 0);
        co_return 0;
    }) == 0);
}

}  // namespace

TEST_CASE("supervisor: commit stops the device and seals its upper offline",
          "[supervisor]") {
    test::TempDir dir;
    const std::string sock = dir / "supervisor.sock";
    const std::string upper_dir = dir / "upper";
    const std::string upper = upper_dir + "/overlaybd.rw";

    // Image configs: only the `upper` section matters to commit.
    const std::string cfg_lsmt = write_config(
        dir, "config-lsmt.json",
        {{"upper", {{"dir", upper_dir}, {"type", "lsmt"}}}});
    const std::string cfg_sparse = write_config(
        dir, "config-sparse.json",
        {{"upper", {{"dir", upper_dir}, {"type", "sparse"}}}});
    const std::string cfg_plain = test::write_file(
        dir / "config-plain.json", std::vector<uint8_t>{'{', '}'});

    auto guard = block_daemon_signals();

    const int failures = run_daemon_case(
        commit_test_cfg(sock),
        [&](const std::string& s, const CheckFn& check) {
            for (int i = 0; i < 250 && !std::filesystem::exists(s); ++i) {
                std::this_thread::sleep_for(20ms);
            }
            if (!std::filesystem::exists(s)) {
                check(false, "supervisor socket never appeared");
                return;
            }
            auto rpc_json = [&](const nlohmann::json& cmd) {
                return nlohmann::json::parse(uds_rpc(s, cmd.dump() + "\n"));
            };

            // Unknown id is a clear error.
            const auto unknown =
                rpc_json({{"cmd", "commit"}, {"id", "ghost"}});
            check(unknown.value("ok", true) == false,
                  "commit on unknown id not an error");
            check(unknown.contains("error") &&
                      unknown["error"].get<std::string>().find(
                          "no such device") != std::string::npos,
                  "unknown id error text mismatch");

            // Malformed field types are clean protocol errors (answered
            // over the socket), not a dropped connection; the daemon
            // keeps serving afterwards.
            const auto bad_id = rpc_json({{"cmd", "commit"}, {"id", 123}});
            check(bad_id.value("ok", true) == false,
                  "numeric commit id not an error");
            check(bad_id.contains("error") &&
                      bad_id["error"].get<std::string>().find(
                          "must be a string") != std::string::npos,
                  "numeric id error text mismatch");
            const auto bad_tag = rpc_json({{"cmd", "commit"},
                                           {"id", "ghost"},
                                           {"user_tag", 42}});
            check(bad_tag.value("ok", true) == false,
                  "numeric user_tag not an error");
            check(bad_tag.contains("error") &&
                      bad_tag["error"].get<std::string>().find("user_tag") !=
                          std::string::npos,
                  "numeric user_tag error text mismatch");
            check(rpc_json({{"cmd", "hello"}}).value("ok", false) == true,
                  "daemon unusable after malformed commit commands");

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

            const auto sparse = rpc_json({{"cmd", "commit"}, {"id", "d2"}});
            check(sparse.value("ok", true) == false,
                  "commit on sparse upper not an error");
            check(sparse.contains("error") &&
                      sparse["error"].get<std::string>().find("sparse") !=
                          std::string::npos,
                  "sparse error text mismatch");
            const auto no_upper = rpc_json({{"cmd", "commit"}, {"id", "d3"}});
            check(no_upper.value("ok", true) == false,
                  "commit without upper not an error");
            check(no_upper.contains("error") &&
                      no_upper["error"].get<std::string>().find(
                          "no writable upper") != std::string::npos,
                  "no-upper error text mismatch");

            // Upper-path provenance (ADR-0014): the config file's CURRENT
            // content must not matter — commit uses what create recorded.
            // Overwriting the config with "{}" must not redirect or break
            // the commit below.
            test::write_file(cfg_lsmt, std::vector<uint8_t>{'{', '}'});

            // The main path: commit a LIVE device — the documented
            // contract is stop-then-seal, and the reply can only be ok if
            // the SIGTERM made the fake checkpoint the upper first.
            const auto commit = rpc_json({{"cmd", "commit"},
                                          {"id", "d1"},
                                          {"user_tag", "v1"}});
            check(commit.value("ok", false) == true, "commit d1 failed");
            if (!commit.value("ok", false)) {
                std::fprintf(stderr, "[commit d1 reply] %s\n",
                             commit.dump().c_str());
            }
            check(commit.contains("path") &&
                      commit["path"].get<std::string>() == upper,
                  "commit path mismatch");
            check(commit.contains("sha256") &&
                      commit["sha256"].get<std::string>().size() == 64,
                  "commit sha256 not 64 hex chars");
            check(commit.contains("size") &&
                      commit["size"].get<uint64_t>() > 0,
                  "commit size not positive");

            // The device was stopped by commit (bounded poll, no fixed
            // sleep).
            std::string state;
            for (int i = 0; i < 250; ++i) {
                const auto st = rpc_json({{"cmd", "status"}, {"id", "d1"}});
                if (st.value("ok", false)) {
                    state = st.value("state", "");
                    if (state == "exited") break;
                }
                std::this_thread::sleep_for(20ms);
            }
            check(state == "exited", "device not stopped by commit");

            // A second commit is a clear error: the upper is already
            // sealed.
            const auto again = rpc_json({{"cmd", "commit"}, {"id", "d1"}});
            check(again.value("ok", true) == false,
                  "second commit not an error");
            check(again.contains("error") &&
                      again["error"].get<std::string>().find(
                          "already sealed") != std::string::npos,
                  "already-sealed error text mismatch");
        });
    REQUIRE(failures == 0);

    require_sealed_payload(upper, "v1");
}

TEST_CASE("supervisor: concurrent commits are serialized and reject the loser",
          "[supervisor]") {
    // Two barrier-synchronized commits of the same device: exactly one
    // succeeds; the loser gets a precise error ("commit already in
    // progress" when it races the critical section, "already sealed" when
    // it arrives just after); the sealed file stays intact (no interleaved
    // O_TRUNC writes into a shared tmp file).
    test::TempDir dir;
    const std::string sock = dir / "supervisor.sock";
    const std::string upper_dir = dir / "upper";
    const std::string upper = upper_dir + "/overlaybd.rw";
    const std::string cfg_lsmt = write_config(
        dir, "config-lsmt.json",
        {{"upper", {{"dir", upper_dir}, {"type", "lsmt"}}}});

    auto guard = block_daemon_signals();

    const int failures = run_daemon_case(
        commit_test_cfg(sock),
        [&](const std::string& s, const CheckFn& check) {
            for (int i = 0; i < 250 && !std::filesystem::exists(s); ++i) {
                std::this_thread::sleep_for(20ms);
            }
            if (!std::filesystem::exists(s)) {
                check(false, "supervisor socket never appeared");
                return;
            }
            auto rpc_json = [&](const nlohmann::json& cmd) {
                return nlohmann::json::parse(uds_rpc(s, cmd.dump() + "\n"));
            };
            check(rpc_json({{"cmd", "create"},
                            {"id", "d1"},
                            {"config", cfg_lsmt}})
                      .value("ok", false),
                  "create d1 failed");

            // Deterministic barrier: both threads wait for the go flag, no
            // timing assumptions on a shared machine.
            std::atomic<int> armed{0};
            std::atomic<bool> go{false};
            nlohmann::json r1, r2;
            auto fire = [&](nlohmann::json* out) {
                armed.fetch_add(1);
                while (!go.load()) std::this_thread::yield();
                *out = rpc_json({{"cmd", "commit"}, {"id", "d1"}});
            };
            std::thread t1([&] { fire(&r1); });
            std::thread t2([&] { fire(&r2); });
            while (armed.load() < 2) std::this_thread::sleep_for(1ms);
            go.store(true);
            t1.join();
            t2.join();

            const bool ok1 = r1.value("ok", false);
            const bool ok2 = r2.value("ok", false);
            check(ok1 != ok2, "expected exactly one successful commit");
            const nlohmann::json& loser = ok1 ? r2 : r1;
            check(loser.value("ok", true) == false,
                  "loser reply not an error");
            check(loser.contains("error") &&
                      (loser["error"].get<std::string>().find(
                           "already in progress") != std::string::npos ||
                       loser["error"].get<std::string>().find(
                           "already sealed") != std::string::npos),
                  "loser error text mismatch");
        });
    REQUIRE(failures == 0);

    // Whichever commit won, the file is a valid sealed layer with the
    // payload — never a half-written interleaving.
    require_sealed_payload(upper, "");
}
