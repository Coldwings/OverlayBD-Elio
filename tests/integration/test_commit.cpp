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

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
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

namespace {

/// ADR-0014 mode-3 mkfs mock: records invocations instead of running host
/// mkfs (the suite never executes mkfs.<type>). `fail_rc` != 0 makes the
/// runner fail, for the clean-error path. The daemon coroutine and the
/// client thread touch the records, so they are mutex-guarded.
struct MockMkfs final : public supervisor::MkfsRunner {
    std::atomic<int> calls{0};
    std::atomic<int> fail_rc{0};  // set before the create that should fail
    /// When false, run() parks (async sleep loop) until the test releases
    /// it — lets a test hold a create inside its mkfs step and act on the
    /// device underneath it.
    std::atomic<bool> release{true};

    elio::coro::task<int> run(const std::string& fs_type,
                              const std::string& device,
                              std::string* error) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            calls.fetch_add(1);
            last_type_ = fs_type;
            last_device_ = device;
        }
        while (!release.load()) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(10));
        }
        if (fail_rc.load() != 0) {
            if (error) *error = "injected mkfs failure";
            co_return fail_rc.load();
        }
        co_return 0;
    }

    /// Pids this runner "abandoned" (the REAL runner abandons a helper that
    /// outlives its bounded post-SIGKILL reap): the daemon's reaper must
    /// collect them. Touched by the test thread and the daemon's reaper, so
    /// both the queue and the drain are mutex-guarded.
    void add_orphan(pid_t pid) {
        std::lock_guard<std::mutex> lk(mu_);
        orphans_.push_back(pid);
    }
    std::vector<pid_t> take_orphan_pids() override {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<pid_t> out;
        out.swap(orphans_);
        return out;
    }

    std::string last_type() const {
        std::lock_guard<std::mutex> lk(mu_);
        return last_type_;
    }
    std::string last_device() const {
        std::lock_guard<std::mutex> lk(mu_);
        return last_device_;
    }

private:
    mutable std::mutex mu_;
    std::string last_type_;
    std::string last_device_;
    std::vector<pid_t> orphans_;
};

/// The sealed blank upper must re-open as a standard sealed LSMT RO layer
/// carrying the fake's payload (written through the blank merged stack at
/// virtual offset 0, so the data sits at sector 8) with the requested
/// virtual size.
void require_sealed_blank_upper(const std::string& upper,
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

/// Saves/restores PATH: the daemon's DEFAULT mkfs runner resolves
/// `mkfs.<type>` on the process PATH (in-process daemon in these tests).
struct PathGuard {
    std::string prev;
    PathGuard() {
        const char* p = ::getenv("PATH");
        prev = p != nullptr ? p : "";
    }
    ~PathGuard() { ::setenv("PATH", prev.c_str(), 1); }
    PathGuard(const PathGuard&) = delete;
    PathGuard& operator=(const PathGuard&) = delete;
};

}  // namespace

// NOTE: the TEST_CASE name stays on ONE source line — scripts/check-docs.sh
// extracts names line-wise and the docs cite them verbatim.
TEST_CASE("supervisor: default mkfs runner completes without the reaper stealing it",
          "[supervisor]") {
    // F2 regression: the daemon's SIGCHLD reaper must NOT reap the mkfs
    // helper child. A wildcard waitpid(-1) stole its exit status, so the
    // runner's waitpid returned ECHILD and every mode-3 create failed.
    // Here the daemon runs its REAL default runner against a PATH shim
    // (no host mkfs involved): create must succeed and the shim must have
    // seen the device path.
    test::TempDir dir;
    const std::string sock = dir / "supervisor.sock";
    const std::string blank_root = dir / "blank";
    const std::string bin = dir / "bin";
    REQUIRE(::mkdir(bin.c_str(), 0755) == 0);
    const std::string argv_log = dir / "mkfs-argv.log";
    {
        const std::string path = bin + "/mkfs.ext4";
        const std::string content = "#!/bin/sh\necho \"$@\" > \"" +
                                    argv_log + "\"\nexit 0\n";
        test::write_file(path,
                         std::vector<uint8_t>(content.begin(),
                                              content.end()));
        REQUIRE(::chmod(path.c_str(), 0755) == 0);
    }
    PathGuard path_guard;
    REQUIRE(::setenv("PATH", (bin + ":/usr/bin:/bin").c_str(), 1) == 0);

    auto guard = block_daemon_signals();
    const int failures = run_daemon_case(
        [&] {
            supervisor::DaemonConfig cfg = commit_test_cfg(sock);
            cfg.blank_dir = blank_root;
            cfg.mkfs_timeout_sec = 30;
            // Deliberately NOT setting mkfs_runner: the daemon installs
            // its real default (fork/exec) runner.
            return cfg;
        }(),
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
            const auto created =
                rpc_json({{"cmd", "create"},
                          {"id", "m1"},
                          {"blank", {{"size", kFakeVsize},
                                     {"mkfs", "ext4"}}}});
            check(created.value("ok", false) == true,
                  "mode-3 create with the default runner failed");
            if (!created.value("ok", false)) {
                std::fprintf(stderr, "[default-runner mkfs reply] %s\n",
                             created.dump().c_str());
            }
            check(created.value("mkfs", "") == "ext4",
                  "default-runner reply lacks mkfs");
            // The daemon stays healthy after the helper child ran.
            check(rpc_json({{"cmd", "hello"}}).value("ok", false) == true,
                  "daemon unusable after the mkfs helper exited");
        });
    REQUIRE(failures == 0);

    // The shim really was executed, with the device path the fake
    // reported — i.e. the runner, not the reaper, owned the child.
    const int fd = ::open(argv_log.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    std::string logged;
    char buf[256];
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    REQUIRE(n > 0);
    logged.assign(buf, static_cast<size_t>(n));
    ::close(fd);
    REQUIRE(logged.find("/dev/ublkb70") != std::string::npos);
}

// NOTE: name on one line (check-docs name extraction is line-wise).
TEST_CASE("supervisor: blank create serves a writable zero base and commit seals its upper",
          "[supervisor]") {
    // ADR-0014 mode 2 end to end (no ublk): create a blank raw device of a
    // requested size through the real daemon → the fake device assembles
    // the empty LSMT zero base + LSMT-RW upper via open_blank_device and
    // round-trips a payload through the merged stack (unwritten regions
    // read zero) → commit stops it and seals the blank-born upper offline.
    // Host mkfs is NEVER involved: a recording mock runner asserts zero
    // invocations.
    test::TempDir dir;
    const std::string sock = dir / "supervisor.sock";
    const std::string blank_root = dir / "blank";
    const std::string upper = blank_root + "/d1/overlaybd.rw";

    auto mock = std::make_shared<MockMkfs>();
    auto guard = block_daemon_signals();

    const int failures = run_daemon_case(
        [&] {
            supervisor::DaemonConfig cfg = commit_test_cfg(sock);
            cfg.blank_dir = blank_root;
            cfg.mkfs_runner = mock;
            return cfg;
        }(),
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

            // Config-less create without blank is a clean protocol error.
            const auto no_mode = rpc_json({{"cmd", "create"}, {"id", "x"}});
            check(no_mode.value("ok", true) == false,
                  "create without config/blank not an error");
            // Malformed blank specs are clean protocol errors.
            check(rpc_json({{"cmd", "create"},
                            {"id", "x"},
                            {"blank", {{"size", 100}}}})
                      .value("ok", true) == false,
                  "unaligned blank size not an error");
            check(rpc_json({{"cmd", "create"},
                            {"id", "x"},
                            {"blank", {{"size", 512 * 64},
                                       {"mkfs", "EXT4"}}}}
                          )
                      .value("ok", true) == false,
                  "uppercase mkfs type not an error");

            // Mode 2: blank create without mkfs. The fake's internal
            // zero-read + pwrite/pread round-trip through the blank stack
            // must have succeeded for the device to report ready.
            const auto created = rpc_json({{"cmd", "create"},
                                           {"id", "d1"},
                                           {"blank", {{"size", kFakeVsize}}}});
            check(created.value("ok", false) == true, "blank create failed");
            if (!created.value("ok", false)) {
                std::fprintf(stderr, "[blank create reply] %s\n",
                             created.dump().c_str());
                return;
            }
            check(created.value("mode", "") == "blank",
                  "blank create reply lacks mode");
            check(created.value("size", uint64_t{0}) == kFakeVsize,
                  "blank create reply size mismatch");
            check(mock->calls.load() == 0,
                  "mode-2 create must not invoke mkfs");

            // The device workspace holds the sealed empty zero base and the
            // unsealed upper file.
            check(std::filesystem::exists(blank_root + "/d1/overlaybd.zero"),
                  "blank zero base missing");
            check(std::filesystem::exists(upper), "blank upper missing");

            // Commit seals the blank-born upper (stop-then-seal), exactly
            // like an image-born upper.
            const auto commit =
                rpc_json({{"cmd", "commit"}, {"id", "d1"},
                          {"user_tag", "blank-v1"}});
            check(commit.value("ok", false) == true, "blank commit failed");
            if (!commit.value("ok", false)) {
                std::fprintf(stderr, "[blank commit reply] %s\n",
                             commit.dump().c_str());
                return;
            }
            check(commit.contains("path") &&
                      commit["path"].get<std::string>() == upper,
                  "blank commit path mismatch");
            check(commit.contains("sha256") &&
                      commit["sha256"].get<std::string>().size() == 64,
                  "blank commit sha256 not 64 hex chars");
            check(mock->calls.load() == 0,
                  "commit must not invoke mkfs");

            // A second commit of the blank device is the usual
            // already-sealed error.
            const auto again = rpc_json({{"cmd", "commit"}, {"id", "d1"}});
            check(again.value("ok", true) == false,
                  "second blank commit not an error");
        });
    REQUIRE(failures == 0);
    require_sealed_blank_upper(upper, "blank-v1");
}

TEST_CASE("supervisor: mode-3 mkfs runs only when the blank spec requests it",
          "[supervisor]") {
    // ADR-0014 mode 3: the daemon runs host mkfs.<type> on the new block
    // device ONLY when the create's blank object explicitly requests a
    // type. The mock runner records the invocation; a plain mode-2 create
    // never calls it, and a failing mkfs answers with a clean error and
    // removes the device entry.
    test::TempDir dir;
    const std::string sock = dir / "supervisor.sock";
    const std::string blank_root = dir / "blank";

    auto mock = std::make_shared<MockMkfs>();
    auto guard = block_daemon_signals();

    const int failures = run_daemon_case(
        [&] {
            supervisor::DaemonConfig cfg = commit_test_cfg(sock);
            cfg.blank_dir = blank_root;
            cfg.mkfs_runner = mock;
            return cfg;
        }(),
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

            // Mode 2 first: no mkfs anywhere.
            const auto plain = rpc_json({{"cmd", "create"},
                                         {"id", "p1"},
                                         {"blank", {{"size", kFakeVsize}}}});
            check(plain.value("ok", false) == true, "plain blank create failed");
            check(mock->calls.load() == 0,
                  "plain blank create invoked mkfs");

            // Mode 3: explicit mkfs → exactly one invocation, with the
            // fake's reported block device path and the requested type.
            const auto formatted = rpc_json({{"cmd", "create"},
                                             {"id", "f1"},
                                             {"blank", {{"size", kFakeVsize},
                                                        {"mkfs", "ext4"}}}});
            check(formatted.value("ok", false) == true,
                  "mode-3 blank create failed");
            if (formatted.value("ok", false)) {
                check(formatted.value("mkfs", "") == "ext4",
                      "mode-3 reply lacks mkfs field");
            }
            check(mock->calls.load() == 1, "mkfs invoked != once");
            check(mock->last_type() == "ext4", "mkfs type mismatch");
            check(mock->last_device() == "/dev/ublkb70",
                  "mkfs device path mismatch");

            // ADR-0014 boundary: an upper the supervisor formatted with
            // host mkfs (mode 3) is never sealed — commit refuses it
            // before any stop/stop seal.
            const auto refused =
                rpc_json({{"cmd", "commit"}, {"id", "f1"}});
            check(refused.value("ok", true) == false,
                  "mode-3 commit not refused");
            check(refused.contains("error") &&
                      refused["error"].get<std::string>().find(
                          "host mkfs") != std::string::npos,
                  "mode-3 commit refusal text mismatch");

            // A failing mkfs is a clean create error and the entry is
            // removed (no half-created device left behind).
            mock->fail_rc = 1;
            const auto bad = rpc_json({{"cmd", "create"},
                                       {"id", "g1"},
                                       {"blank", {{"size", kFakeVsize},
                                                  {"mkfs", "xfs"}}}});
            check(bad.value("ok", true) == false,
                  "failing mkfs not an error");
            check(bad.contains("error") &&
                      bad["error"].get<std::string>().find("mkfs.xfs") !=
                          std::string::npos,
                  "failing mkfs error text mismatch");
            check(mock->calls.load() == 2, "failing mkfs not invoked");
            const auto ghost = rpc_json({{"cmd", "status"}, {"id", "g1"}});
            check(ghost.value("ok", true) == false,
                  "failed-mkfs device entry still present");
        });
    REQUIRE(failures == 0);
}


TEST_CASE("supervisor: stale blank-create failure leaves a newer device alone",
          "[supervisor]") {
    // F3 regression: a create can sit inside its (up to 300 s) mkfs step
    // while the operator destroys that id and creates it again. When the
    // stale create's mkfs finally fails, its cleanup must not terminate or
    // remove the NEW device that now owns the id.
    test::TempDir dir;
    const std::string sock = dir / "supervisor.sock";
    const std::string blank_root = dir / "blank";

    auto mock = std::make_shared<MockMkfs>();
    mock->release = false;  // hold the first create inside mkfs
    mock->fail_rc = 1;      // ... and make it fail once released
    auto guard = block_daemon_signals();

    const int failures = run_daemon_case(
        [&] {
            supervisor::DaemonConfig cfg = commit_test_cfg(sock);
            cfg.blank_dir = blank_root;
            cfg.mkfs_runner = mock;
            return cfg;
        }(),
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

            // The blocked create runs on its own thread (its reply only
            // arrives once the mock is released).
            nlohmann::json stale_reply;
            std::thread stale([&] {
                try {
                    stale_reply = rpc_json({{"cmd", "create"},
                                            {"id", "x1"},
                                            {"blank", {{"size", kFakeVsize},
                                                       {"mkfs", "ext4"}}}});
                } catch (const std::exception&) {
                    stale_reply = nlohmann::json{{"ok", false}};
                }
            });
            for (int i = 0; i < 500 && mock->calls.load() == 0; ++i) {
                std::this_thread::sleep_for(10ms);
            }
            check(mock->calls.load() >= 1, "stale create never reached mkfs");

            // Underneath it: destroy the id and create it again (mode 2,
            // no mkfs — this one must survive).
            check(rpc_json({{"cmd", "destroy"}, {"id", "x1"}})
                      .value("ok", false),
                  "destroy of the in-mkfs device failed");
            const auto fresh = rpc_json({{"cmd", "create"},
                                         {"id", "x1"},
                                         {"blank", {{"size", kFakeVsize}}}});
            check(fresh.value("ok", false), "re-create under the same id failed");
            const int fresh_pid = fresh.value("pid", -1);

            // Release the stale create's mkfs: it fails and its cleanup
            // runs.
            mock->release = true;
            stale.join();
            check(stale_reply.value("ok", true) == false,
                  "stale create should have failed");
            check(stale_reply.contains("error") &&
                      stale_reply["error"].get<std::string>().find("mkfs") !=
                          std::string::npos,
                  "stale create error text mismatch");

            // The NEW device is untouched: same pid, STILL READY (not
            // stopped), still served. The state assertion is what catches
            // a cleanup that terminates the newer device without removing
            // its entry.
            const auto st = rpc_json({{"cmd", "status"}, {"id", "x1"}});
            check(st.value("ok", false),
                  "newer device was removed by the stale failure");
            check(st.value("pid", -1) == fresh_pid,
                  "newer device pid changed (stale cleanup hit it)");
            check(st.value("state", "") == "ready",
                  "newer device was stopped by the stale cleanup");
            check(rpc_json({{"cmd", "hello"}}).value("ok", false),
                  "daemon unusable after the stale failure");
        });
    REQUIRE(failures == 0);
}

TEST_CASE("supervisor: commit is refused while a mode-3 create is still in mkfs",
          "[supervisor]") {
    // Review finding: the entry is published in `children_` before the
    // mkfs step runs, and the unsealable-upper rule used to key on mkfs
    // having FINISHED — so a commit landing inside that window sealed a
    // supervisor-formatted (non-deterministic) upper, violating the
    // ADR-0014 boundary. The rule now keys on the intent recorded when the
    // entry is created, so no ordering lets that seal through.
    test::TempDir dir;
    const std::string sock = dir / "supervisor.sock";
    const std::string blank_root = dir / "blank";

    auto mock = std::make_shared<MockMkfs>();
    mock->release = false;  // hold the create inside its mkfs step
    auto guard = block_daemon_signals();

    const int failures = run_daemon_case(
        [&] {
            supervisor::DaemonConfig cfg = commit_test_cfg(sock);
            cfg.blank_dir = blank_root;
            cfg.mkfs_runner = mock;
            return cfg;
        }(),
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

            // The mode-3 create blocks inside mkfs on its own thread; its
            // reply only arrives once the mock is released.
            nlohmann::json create_reply;
            std::thread blocked([&] {
                try {
                    create_reply =
                        rpc_json({{"cmd", "create"},
                                  {"id", "h1"},
                                  {"blank", {{"size", kFakeVsize},
                                             {"mkfs", "ext4"}}}});
                } catch (const std::exception&) {
                    create_reply = nlohmann::json{{"ok", false}};
                }
            });
            for (int i = 0; i < 500 && mock->calls.load() == 0; ++i) {
                std::this_thread::sleep_for(10ms);
            }
            check(mock->calls.load() >= 1, "create never reached mkfs");

            // The window: the device is created, ready and reachable, but
            // its mkfs has not finished. A commit here must be refused, and
            // the device must survive it (a commit's first act is a stop).
            const auto refused = rpc_json({{"cmd", "commit"}, {"id", "h1"}});
            check(refused.value("ok", true) == false,
                  "commit inside the mkfs window was not refused");
            check(refused.contains("error") &&
                      refused["error"].get<std::string>().find("host mkfs") !=
                          std::string::npos,
                  "in-window refusal text mismatch");
            const auto st = rpc_json({{"cmd", "status"}, {"id", "h1"}});
            check(st.value("ok", false) && st.value("state", "") == "ready",
                  "the refused commit stopped the device being formatted");

            // Release mkfs: the create completes normally and the device
            // stays a mode-3 device (never sealable, by design).
            mock->release = true;
            blocked.join();
            check(create_reply.value("ok", false),
                  "mode-3 create failed after release");
            const auto after = rpc_json({{"cmd", "commit"}, {"id", "h1"}});
            check(after.value("ok", true) == false,
                  "post-mkfs commit on a mode-3 device was not refused");
            check(rpc_json({{"cmd", "hello"}}).value("ok", false),
                  "daemon unusable after the window commit");
        });
    REQUIRE(failures == 0);
}

TEST_CASE("supervisor: the reaper collects helpers a runner had to abandon",
          "[supervisor]") {
    // Review finding (round 8): the bounded post-SIGKILL reap of a mode-3
    // mkfs helper must not park a detached task or a blocking thread on the
    // straggler — the daemon's SIGCHLD reaper collects the pid instead, via
    // MkfsRunner::take_orphan_pids(), so the helper cannot stay a zombie
    // and shutdown stays drainable. A helper process that dies while
    // "abandoned" is the observable: after the reaper swept, the TEST's own
    // waitpid must answer ECHILD (someone else reaped it) — a pid returned
    // here means the zombie survived.
    test::TempDir dir;
    const std::string sock = dir / "supervisor.sock";

    auto mock = std::make_shared<MockMkfs>();
    auto guard = block_daemon_signals();

    // An already-reaped pid lands in the same queue first: the reaper's
    // waitpid answers ECHILD for it, which must be terminal (drop it) —
    // otherwise the list would grow forever and re-issue the syscall on
    // every SIGCHLD wake. Covering both entries in one run pins that the
    // dead pid neither wedges the sweep nor stops the live one being
    // reaped.
    const pid_t dead = ::fork();
    REQUIRE(dead >= 0);
    if (dead == 0) _exit(0);
    int dead_status = 0;
    REQUIRE(::waitpid(dead, &dead_status, 0) == dead);
    mock->add_orphan(dead);

    const pid_t helper = ::fork();
    REQUIRE(helper >= 0);
    if (helper == 0) {
        ::usleep(300 * 1000);  // dies well inside the observation window
        _exit(0);
    }
    mock->add_orphan(helper);

    const int failures = run_daemon_case(
        [&] {
            supervisor::DaemonConfig cfg = commit_test_cfg(sock);
            cfg.mkfs_runner = mock;
            return cfg;
        }(),
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
            check(rpc_json({{"cmd", "hello"}}).value("ok", false),
                  "daemon not serving");
            // Give the helper time to die and the reaper to sweep it before
            // asking: whoever waits first owns the status, so the check must
            // not race the reaper.
            std::this_thread::sleep_for(1200ms);
            int wstatus = 0;
            errno = 0;
            const pid_t r = ::waitpid(helper, &wstatus, WNOHANG);
            // -1/ECHILD = reaped by the daemon (the intent); the pid itself
            // means the zombie was still there for us to collect.
            check(r < 0 && errno == ECHILD,
                  "abandoned helper was not reaped by the daemon");
        });
    REQUIRE(failures == 0);
}
