// Integration test: the D3 grow-only resize chain (ADR-0014 dev_size
// model) over the real control socket — supervisor resize/create-headroom/
// commit-re-baseline against the signalfd-based fake obd-device
// (tests/fake_device_main.cpp), which — like the real obd-device — serves
// the device command channel and executes resize grow-only without a
// kernel. No ublk/kernel facility is needed (the privileged ublk grow is
// covered by the self-skipping E2E; see tests/integration/test_ublk_e2e.cpp).
#include "format/lsmt.hpp"
#include "format/lsmt_rw.hpp"
#include "source/local_file.hpp"
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
#include <cstdio>
#include <cstring>
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

/// Must match tests/fake_device_main.cpp (the fake's declared layer size
/// and the payload it writes before it can be checkpointed).
constexpr uint64_t kFakeVsize = 512 * 64;      // 32 KiB
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
/// exception.
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
/// daemon. Catch2 assertions happen here, on the calling thread.
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

supervisor::DaemonConfig resize_test_cfg(const std::string& sock) {
    supervisor::DaemonConfig cfg;
    cfg.socket_path = sock;
    cfg.global_config = "";
    cfg.device_bin = OBD_TEST_FAKE_DEVICE_BIN;
    cfg.ready_timeout_sec = 5;
    cfg.stop_timeout_sec = 5;
    cfg.max_recovery_attempts = 0;
    return cfg;
}

std::string write_config(const test::TempDir& dir, const std::string& name,
                         const nlohmann::json& j) {
    const std::string text = j.dump();
    return test::write_file(dir / name,
                            std::vector<uint8_t>(text.begin(), text.end()));
}

/// Re-opens the (sealed) upper and asserts its declared virtual size and
/// the fake's payload (data region starts at sector 8).
void require_sealed_layer(const std::string& upper, uint64_t expected_vsize) {
    REQUIRE(test::run_coro([&]() -> elio::coro::task<int> {
        auto ro = co_await source::LocalFileSource::open(upper);
        source::BlobSourcePtr base = std::move(ro);
        auto layer = co_await format::LsmtLayer::open(std::move(base));
        REQUIRE(layer->virtual_size() == expected_vsize);
        REQUIRE(layer->segments().size() == 1);
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

TEST_CASE("supervisor: resize grows a device and rejects shrink or no-op cleanly",
          "[supervisor]") {
    test::TempDir dir;
    const std::string sock = dir / "supervisor.sock";
    const std::string upper_dir = dir / "upper";
    const std::string cfg_lsmt = write_config(
        dir, "config-lsmt.json",
        {{"upper", {{"dir", upper_dir}, {"type", "lsmt"}}}});

    auto guard = block_daemon_signals();

    const int failures = run_daemon_case(
        resize_test_cfg(sock),
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

            // Unknown id and malformed fields are clean errors; the
            // daemon keeps serving.
            const auto ghost = rpc_json({{"cmd", "resize"},
                                         {"id", "ghost"},
                                         {"size", 65536}});
            check(ghost.value("ok", true) == false &&
                      ghost["error"].get<std::string>().find("no such device") !=
                          std::string::npos,
                  "resize on unknown id not a clean error");
            const auto bad_type = rpc_json({{"cmd", "resize"},
                                            {"id", "d1"},
                                            {"size", "big"}});
            check(bad_type.value("ok", true) == false &&
                      bad_type["error"].get<std::string>().find(
                          "non-negative integer") != std::string::npos,
                  "resize string size not a parse error");
            const auto bad_neg = rpc_json({{"cmd", "resize"},
                                           {"id", "d1"},
                                           {"size", -1}});
            check(bad_neg.value("ok", true) == false,
                  "resize negative size not an error");

            // The fake serves the device with the declared 32 KiB size.
            check(rpc_json({{"cmd", "create"},
                            {"id", "d1"},
                            {"config", cfg_lsmt}})
                      .value("ok", false),
                  "create d1 failed");

            // No-op (equal to current): grow-only rejection.
            const auto equal =
                rpc_json({{"cmd", "resize"}, {"id", "d1"},
                          {"size", kFakeVsize}});
            check(equal.value("ok", true) == false &&
                      equal["error"].get<std::string>().find("grow-only") !=
                          std::string::npos,
                  "resize to the current size not rejected grow-only");

            // Misaligned sizes are rejected supervisor-side.
            const auto mis =
                rpc_json({{"cmd", "resize"}, {"id", "d1"}, {"size", 1000}});
            check(mis.value("ok", true) == false &&
                      mis["error"].get<std::string>().find(
                          "multiple of 512") != std::string::npos,
                  "resize misaligned size not rejected");

            // A real grow: reply carries the new size and the id.
            const auto grow = rpc_json({{"cmd", "resize"},
                                        {"id", "d1"},
                                        {"size", kFakeVsize * 2}});
            check(grow.value("ok", false) == true,
                  "resize grow failed");
            check(grow.value("size", 0) == kFakeVsize * 2,
                  "resize reply size mismatch");
            check(grow.value("id", "") == "d1", "resize reply id mismatch");

            // Shrink back (and any size <= current after the grow): still
            // grow-only rejections.
            const auto shrink =
                rpc_json({{"cmd", "resize"}, {"id", "d1"},
                          {"size", kFakeVsize}});
            check(shrink.value("ok", true) == false &&
                      shrink["error"].get<std::string>().find("grow-only") !=
                          std::string::npos,
                  "resize shrink not rejected grow-only");
            const auto middle =
                rpc_json({{"cmd", "resize"}, {"id", "d1"},
                          {"size", kFakeVsize * 3 / 2}});
            check(middle.value("ok", true) == false &&
                      middle["error"].get<std::string>().find("grow-only") !=
                          std::string::npos,
                  "resize to below the grown size not rejected");

            // The daemon is still alive and serving after all of it.
            check(rpc_json({{"cmd", "hello"}}).value("ok", false) == true,
                  "daemon unusable after resize commands");
        });
    REQUIRE(failures == 0);
}

TEST_CASE("supervisor: create virtual_size headroom override is validated grow-only",
          "[supervisor]") {
    test::TempDir dir;
    const std::string sock = dir / "supervisor.sock";
    const std::string upper_dir = dir / "upper";
    const std::string cfg_lsmt = write_config(
        dir, "config-lsmt.json",
        {{"upper", {{"dir", upper_dir}, {"type", "lsmt"}}}});

    auto guard = block_daemon_signals();

    const int failures = run_daemon_case(
        resize_test_cfg(sock),
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

            // Wrong type / misaligned override: clean supervisor errors.
            const auto bad_type = rpc_json({{"cmd", "create"},
                                            {"id", "dtype"},
                                            {"config", cfg_lsmt},
                                            {"virtual_size", "big"}});
            check(bad_type.value("ok", true) == false &&
                      bad_type["error"].get<std::string>().find(
                          "non-negative integer") != std::string::npos,
                  "create virtual_size string not a parse error");
            const auto bad_align = rpc_json({{"cmd", "create"},
                                             {"id", "dalign"},
                                             {"config", cfg_lsmt},
                                             {"virtual_size", 1000}});
            check(bad_align.value("ok", true) == false &&
                      bad_align["error"].get<std::string>().find(
                          "multiple of 512") != std::string::npos,
                  "create virtual_size misaligned not rejected");

            // A headroom override smaller than the image's declared size
            // is a shrink: the device (which knows the assembled size)
            // rejects it and create fails with the rule's message.
            const auto small = rpc_json({{"cmd", "create"},
                                         {"id", "dsmall"},
                                         {"config", cfg_lsmt},
                                         {"virtual_size", kFakeVsize / 2}});
            check(small.value("ok", true) == false &&
                      small["error"].get<std::string>().find("grow-only") !=
                          std::string::npos,
                  "create headroom shrink not rejected");
            check(small["error"].get<std::string>().find(
                      "smaller than the image") != std::string::npos,
                  "create headroom shrink error text mismatch");

            // Sanctioned headroom: an override larger than the image size
            // creates fine.
            check(rpc_json({{"cmd", "create"},
                            {"id", "dhead"},
                            {"config", cfg_lsmt},
                            {"virtual_size", kFakeVsize * 3}})
                      .value("ok", false),
                  "create with headroom override failed");
            // ... and the grown device stays grow-only: a resize to the
            // image size (a shrink of the created device) is rejected.
            const auto shrink = rpc_json({{"cmd", "resize"},
                                          {"id", "dhead"},
                                          {"size", kFakeVsize}});
            check(shrink.value("ok", true) == false &&
                      shrink["error"].get<std::string>().find("grow-only") !=
                          std::string::npos,
                  "resize of headroom device below its created size not "
                  "rejected");
        });
    REQUIRE(failures == 0);
}

TEST_CASE("supervisor: commit virtual_size re-baselines the sealed layer grow-only",
          "[supervisor]") {
    test::TempDir dir;
    const std::string sock = dir / "supervisor.sock";
    const std::string upper_dir = dir / "upper";
    const std::string upper = upper_dir + "/overlaybd.rw";
    const std::string cfg_lsmt = write_config(
        dir, "config-lsmt.json",
        {{"upper", {{"dir", upper_dir}, {"type", "lsmt"}}}});

    auto guard = block_daemon_signals();

    const int failures = run_daemon_case(
        resize_test_cfg(sock),
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

            // Wrong type / misaligned / negative override: clean errors.
            const auto bad_type = rpc_json({{"cmd", "commit"},
                                            {"id", "d1"},
                                            {"virtual_size", "big"}});
            check(bad_type.value("ok", true) == false &&
                      bad_type["error"].get<std::string>().find(
                          "non-negative integer") != std::string::npos,
                  "commit virtual_size string not a parse error");

            check(rpc_json({{"cmd", "create"},
                            {"id", "d1"},
                            {"config", cfg_lsmt}})
                      .value("ok", false),
                  "create d1 failed");

            // Misaligned override: rejected before the device is stopped.
            const auto bad_align = rpc_json({{"cmd", "commit"},
                                             {"id", "d1"},
                                             {"virtual_size", 1000}});
            check(bad_align.value("ok", true) == false &&
                      bad_align["error"].get<std::string>().find(
                          "multiple of 512") != std::string::npos,
                  "commit virtual_size misaligned not rejected");

            // Below the layer's declared size: grow-only rejection from
            // the seal path with a precise reason.
            const auto below = rpc_json({{"cmd", "commit"},
                                         {"id", "d1"},
                                         {"virtual_size", kFakeVsize / 2}});
            check(below.value("ok", true) == false &&
                      below["error"].get<std::string>().find("declared size") !=
                          std::string::npos,
                  "commit virtual_size below declared size not rejected");
            check(below["error"].get<std::string>().find("grow-only") !=
                      std::string::npos,
                  "commit virtual_size below declared size error text "
                  "mismatch");

            // A re-baseline >= the declared size (and content extent)
            // seals with the override in the header.
            const uint64_t rebased = kFakeVsize * 3;
            const auto ok = rpc_json({{"cmd", "commit"},
                                      {"id", "d1"},
                                      {"virtual_size", rebased}});
            check(ok.value("ok", false) == true,
                  "commit with virtual_size re-baseline failed");
            check(ok.contains("path") && ok.contains("sha256"),
                  "commit reply missing path/sha256");
        });
    REQUIRE(failures == 0);

    // The sealed layer re-opens with the re-baselined virtual size and
    // the fake's payload intact (asserted on the calling thread).
    require_sealed_layer(upper, kFakeVsize * 3);
}
