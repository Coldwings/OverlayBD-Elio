// Integration tests for trace recording (ADR-0013, record path) over the
// REAL supervisor control socket: a real daemon supervises the test fake
// device, which — for configs with lowers — opens a REAL image (full
// assembly: registry -> record tap -> LayerStore -> tar -> lsmt -> merge)
// against the in-process mock registry below, and serves the real
// device-side trace command protocol (src/supervisor/device_control).
// When a recording starts, the fake runs a scripted read workload through
// the merged root, so the tap captures genuine remote reads.
//
// Every produced blob is parsed with the same codec reader
// (src/format/trace.hpp) the replay side and the C2 golden tests use.
//
// Duration semantics under test: the bound is enforced DEVICE-side, so
// expiry finalizes with no client call — including when the client
// disconnected mid-recording (a dead CLI can never leak a recording
// device).
#include "common/sha256.hpp"
#include "format/trace.hpp"
#include "format/writer.hpp"
#include "image/config.hpp"
#include "supervisor/daemon.hpp"
#include "supervisor/protocol.hpp"

#include <elio/http/http_server.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/time/timer.hpp>

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <system_error>
#include <thread>

using namespace obd;
using namespace std::chrono_literals;
namespace http = elio::http;

#ifndef OBD_TEST_FAKE_DEVICE_BIN
#define OBD_TEST_FAKE_DEVICE_BIN "obd-test-fake-device"
#endif

namespace {

/// Minimal mock registry: a blob map behind GET /v2/<digest> with Range
/// support (mirrors the integration suite's BlobMapServer, trimmed).
class TraceBlobServer {
public:
    TraceBlobServer(std::map<std::string, std::vector<uint8_t>> blobs,
                    uint16_t port)
        : blobs_(std::move(blobs)), port_(port) {
        http::router r;
        r.add_route(http::method::GET, "/v2/*",
                    [this](http::context& ctx) { return handler(ctx); });
        server_ = std::make_unique<http::server>(std::move(r));
    }
    elio::coro::task<void> run() {
        co_await server_->listen(elio::net::socket_address(
            elio::net::ipv4_address("127.0.0.1", port_)));
    }
    void stop() { server_->stop(); }
    std::string repo_base() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v2";
    }

private:
    elio::coro::task<http::response> handler(http::context& ctx) {
        const std::string name(ctx.req().path().substr(4));
        const auto it = blobs_.find(name);
        if (it == blobs_.end()) {
            http::response resp(http::status::not_found);
            resp.set_header("Content-Length", "0");
            co_return resp;
        }
        const auto& blob = it->second;
        // Empty blobs must not underflow the default `last` or be
        // served: range-not-satisfiable is the honest answer.
        if (blob.empty()) {
            http::response resp(http::status::range_not_satisfiable);
            resp.set_header("Content-Length", "0");
            co_return resp;
        }
        const std::string_view range = ctx.req().header("Range");
        uint64_t first = 0, last = blob.size() - 1;
        bool partial = false;
        if (range.starts_with("bytes=")) {
            // Malformed/partial Range headers must not throw out of the
            // mock (stoull on an unvalidated substring would).
            const auto dash = range.find('-', 6);
            if (dash == std::string_view::npos || dash == 6 ||
                dash + 1 >= range.size()) {
                http::response resp(http::status::bad_request);
                resp.set_header("Content-Length", "0");
                co_return resp;
            }
            char* endp = nullptr;
            errno = 0;
            const uint64_t a =
                std::strtoull(std::string(range.substr(6, dash - 6)).c_str(),
                              &endp, 10);
            if (errno != 0 || endp == nullptr || *endp != '\0') {
                http::response resp(http::status::bad_request);
                resp.set_header("Content-Length", "0");
                co_return resp;
            }
            const uint64_t b = std::strtoull(
                std::string(range.substr(dash + 1)).c_str(), &endp, 10);
            if (errno != 0 || endp == nullptr || *endp != '\0') {
                http::response resp(http::status::bad_request);
                resp.set_header("Content-Length", "0");
                co_return resp;
            }
            first = a;
            last = std::min<uint64_t>(b, blob.size() - 1);
            partial = true;
        }
        if (first >= blob.size() || first > last) {
            http::response resp(http::status::range_not_satisfiable);
            resp.set_header("Content-Length", "0");
            co_return resp;
        }
        http::response resp(partial ? http::status::partial_content
                                    : http::status::ok);
        resp.set_header("Content-Length", std::to_string(last - first + 1));
        if (partial) {
            resp.set_header("Content-Range",
                            "bytes " + std::to_string(first) + "-" +
                                std::to_string(last) + "/" +
                                std::to_string(blob.size()));
        }
        resp.set_body(std::string(
            reinterpret_cast<const char*>(blob.data() + first),
            last - first + 1));
        co_return resp;
    }

    std::map<std::string, std::vector<uint8_t>> blobs_;
    uint16_t port_;
    std::unique_ptr<http::server> server_;
};

/// tar-wraps a payload the way overlaybd-commit wraps layer blobs.
std::vector<uint8_t> tar_wrap(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out = test::make_tar_header(payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    const size_t pad = (512 - (payload.size() % 512)) % 512;
    out.resize(out.size() + pad, 0);
    return out;
}

/// Builds the test image blob: a plain (uncompressed) LSMT layer holding
/// 512*512 bytes of pattern content, tar-wrapped for registry transport.
/// Returns {blob, digest, payload_virtual_size}.
struct TestImage {
    std::vector<uint8_t> blob;
    std::string digest;
    uint64_t vsize = 0;
};
TestImage make_test_image(const test::TempDir& dir, uint32_t seed) {
    const auto raw = test::pattern_bytes(512 * 512, seed);
    const std::string raw_path = test::write_file(dir / "raw.img", raw);
    const std::string layer_path = dir / "layer.lsmt";
    const int fd = ::open(raw_path.c_str(), O_RDONLY);
    if (fd < 0) throw std::system_error(errno, std::generic_category());
    format::write_lsmt_single_layer(fd, raw.size(), layer_path, {});
    ::close(fd);
    const int lfd = ::open(layer_path.c_str(), O_RDONLY);
    if (lfd < 0) throw std::system_error(errno, std::generic_category());
    struct stat st {};
    if (::fstat(lfd, &st) != 0) {
        throw std::system_error(errno, std::generic_category());
    }
    std::vector<uint8_t> payload(static_cast<size_t>(st.st_size));
    // ::read may return short even for regular files; loop for the full
    // contents (a single-read assumption is a real flake source).
    size_t got = 0;
    while (got < payload.size()) {
        const ssize_t n =
            ::read(lfd, payload.data() + got, payload.size() - got);
        if (n <= 0) {
            ::close(lfd);
            throw std::system_error(EIO, std::generic_category());
        }
        got += static_cast<size_t>(n);
    }
    ::close(lfd);
    TestImage img;
    img.blob = tar_wrap(payload);
    img.digest = "sha256:" + common::Sha256::hex(img.blob.data(),
                                                 img.blob.size());
    img.vsize = raw.size();
    return img;
}

/// Minimal synchronous obdctl-style client: one JSON line per connection.
/// Runs on a helper thread, so NO Catch2 macros here (Catch2 assertion
/// state is not thread-safe); failures surface as exceptions.
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

/// Sends one line and CLOSES WITHOUT READING (CLI-death simulation).
void uds_send_and_vanish(const std::string& path, const std::string& line) {
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
    const ssize_t w = ::write(fd, line.data(), line.size());
    (void)w;
    ::close(fd);  // gone: no reply read, no clean shutdown
}

struct SigMaskGuard {
    sigset_t prev {};
    ~SigMaskGuard() { ::sigprocmask(SIG_SETMASK, &prev, nullptr); }
};

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

using CheckFn = std::function<void(bool, const char*)>;

/// Drives a real supervisor daemon plus the mock registry while
/// `client_body(sock, check)` runs on a helper thread, then SIGTERMs the
/// daemon. Returns the number of failed client checks.
template <typename F>
int run_trace_daemon_case(supervisor::DaemonConfig cfg,
                          TraceBlobServer& server, F&& client_body) {
    std::atomic<bool> client_done{false};
    std::atomic<int> failures{0};
    std::string fail_msg;
    CheckFn check = [&](bool cond, const char* what) {
        if (!cond) {
            failures.fetch_add(1);
            fail_msg = what;
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
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
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
        server.stop();
        co_await elio::time::sleep_for(20ms);
        co_return 0;
    });
    client.join();
    INFO(fail_msg);
    REQUIRE(rc == 0);
    REQUIRE(daemon_rc.load() >= 0);
    return failures.load();
}

supervisor::DaemonConfig trace_test_cfg(const std::string& sock) {
    supervisor::DaemonConfig cfg;
    cfg.socket_path = sock;
    cfg.global_config = "";
    cfg.device_bin = OBD_TEST_FAKE_DEVICE_BIN;
    cfg.ready_timeout_sec = 10;
    cfg.stop_timeout_sec = 5;
    cfg.max_recovery_attempts = 0;
    return cfg;
}

std::string write_image_config(const test::TempDir& dir,
                               const std::string& repo_base,
                               const TestImage& img,
                               const std::string& layer_dir) {
    nlohmann::json j;
    j["repoBlobUrl"] = repo_base;
    j["lowers"] = nlohmann::json::array({nlohmann::json{
        {"digest", img.digest}, {"size", img.blob.size()},
        {"dir", layer_dir}}});
    const std::string text = j.dump();
    return test::write_file(dir / "config.json",
                            std::vector<uint8_t>(text.begin(), text.end()));
}

std::vector<uint8_t> read_whole_file(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::system_error(errno, std::generic_category());
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        throw std::system_error(errno, std::generic_category());
    }
    std::vector<uint8_t> data(static_cast<size_t>(st.st_size));
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t r = ::read(fd, data.data() + done, data.size() - done);
        if (r <= 0) throw std::system_error(errno, std::generic_category());
        done += static_cast<size_t>(r);
    }
    ::close(fd);
    return data;
}

}  // namespace

TEST_CASE("integration: trace recording captures remote reads end to end",
          "[integration]") {
    test::TempDir dir;
    const auto img = make_test_image(dir, 81);
    const std::string sock = dir / "supervisor.sock";
    const std::string layer_dir = dir / "layer0";
    const std::string trace_path = dir / "out.trace";
    std::filesystem::create_directories(layer_dir);
    TraceBlobServer server({{img.digest, img.blob}}, 19207);
    const std::string cfg_path =
        write_image_config(dir, server.repo_base(), img, layer_dir);

    auto guard = block_daemon_signals();
    std::string stop_sha256;  // threaded out of the client (L5)

    const int failures = run_trace_daemon_case(
        trace_test_cfg(sock), server,
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
                            {"config", cfg_path}})
                      .value("ok", false),
                  "create d1 failed");

            const auto started = rpc_json({{"cmd", "trace_start"},
                                           {"id", "d1"},
                                           {"path", trace_path},
                                           {"duration_sec", 300}});
            check(started.value("ok", false), "trace_start failed");
            check(started.value("id", "") == "d1", "trace_start id echo");
            check(started.value("path", "") == trace_path,
                  "trace_start path echo");
            check(started.value("duration_sec", 0) == 300,
                  "trace_start duration echo");

            // The status reply carries the additive trace field.
            const auto st1 = rpc_json({{"cmd", "status"}, {"id", "d1"}});
            check(st1.value("ok", false) && st1.contains("trace") &&
                      st1["trace"].value("state", "") == "recording",
                  "status does not show recording");

            // The fake's scripted workload runs at recording start; give
            // it a bounded moment before stopping.
            std::this_thread::sleep_for(500ms);
            const auto stopped =
                rpc_json({{"cmd", "trace_stop"}, {"id", "d1"}});
            check(stopped.value("ok", false), "trace_stop failed");
            check(stopped.value("path", "") == trace_path,
                  "trace_stop path mismatch");
            check(stopped.value("sha256", "").size() == 64,
                  "trace_stop sha256 not 64 hex chars");
            stop_sha256 = stopped.value("sha256", "");
            check(stopped.value("size", 0) == 24 + 24,
                  "trace blob should hold exactly one record");
            check(stopped.value("records", 0) == 1,
                  "expected one coalesced record");
            check(stopped.value("dropped", 1) == 0, "unexpected drops");

            const auto st2 = rpc_json({{"cmd", "status"}, {"id", "d1"}});
            check(st2.value("ok", false) && st2.contains("trace") &&
                      st2["trace"].value("state", "") == "stopped" &&
                      st2["trace"].value("reason", "") == "stopped",
                  "status does not show stopped");
        });
    REQUIRE(failures == 0);

    // The blob passes the codec reader (the C2 golden path) and matches
    // the workload's remote fetches: the three middle-extent reads
    // coalesce into ONE adjacent record (the documented window).
    const auto blob = read_whole_file(trace_path);
    auto parsed = format::trace::parse(blob);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 1);
    const auto& rec = parsed->front();
    REQUIRE(rec.op == 'R');
    REQUIRE(rec.layer_index == 0);
    // Workload reads at payload offsets 64K/128K/192K map to tar-extents
    // 1..3: raw [65536, 262144) minus the 512B header = payload
    // [65024, 261632), merged by the coalescing window.
    REQUIRE(rec.offset == 65024);
    REQUIRE(rec.count == 3 * 65536);
    // The digest the stop reply reported IS the produced file's digest.
    REQUIRE(stop_sha256 == common::Sha256::hex(blob.data(), blob.size()));
}

TEST_CASE("integration: trace recording duration expiry finalizes without a client call",
          "[integration]") {
    test::TempDir dir;
    const auto img = make_test_image(dir, 83);
    const std::string sock = dir / "supervisor.sock";
    const std::string layer_dir = dir / "layer0";
    const std::string trace_path = dir / "out.trace";
    std::filesystem::create_directories(layer_dir);
    TraceBlobServer server({{img.digest, img.blob}}, 19208);
    const std::string cfg_path =
        write_image_config(dir, server.repo_base(), img, layer_dir);

    auto guard = block_daemon_signals();

    const int failures = run_trace_daemon_case(
        trace_test_cfg(sock), server,
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
                            {"config", cfg_path}})
                      .value("ok", false),
                  "create d1 failed");
            check(rpc_json({{"cmd", "trace_start"},
                            {"id", "d1"},
                            {"path", trace_path},
                            {"duration_sec", 1}})
                      .value("ok", false),
                  "trace_start failed");

            // NO trace_stop: the device-side timer must finalize, and the
            // additive status field must report it.
            std::string state, reason;
            for (int i = 0; i < 500; ++i) {
                const auto st = rpc_json({{"cmd", "status"}, {"id", "d1"}});
                if (st.value("ok", false) && st.contains("trace") &&
                    st["trace"].value("state", "") == "stopped") {
                    state = "stopped";
                    reason = st["trace"].value("reason", "");
                    break;
                }
                std::this_thread::sleep_for(20ms);
            }
            check(state == "stopped", "recording never finalized");
            check(reason == "expired", "finalize reason not 'expired'");

            // A stop after the expiry is idempotent: same stats.
            const auto again =
                rpc_json({{"cmd", "trace_stop"}, {"id", "d1"}});
            check(again.value("ok", false),
                  "stop after expiry not idempotent");
            check(again.value("records", 0) == 1,
                  "expiry stats mismatch on late stop");
        });
    REQUIRE(failures == 0);

    const auto blob = read_whole_file(trace_path);
    auto parsed = format::trace::parse(blob);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 1);
}

TEST_CASE("integration: trace recording survives client disconnect mid-record",
          "[integration]") {
    test::TempDir dir;
    const auto img = make_test_image(dir, 85);
    const std::string sock = dir / "supervisor.sock";
    const std::string layer_dir = dir / "layer0";
    const std::string trace_path = dir / "out.trace";
    std::filesystem::create_directories(layer_dir);
    TraceBlobServer server({{img.digest, img.blob}}, 19209);
    const std::string cfg_path =
        write_image_config(dir, server.repo_base(), img, layer_dir);

    auto guard = block_daemon_signals();

    const int failures = run_trace_daemon_case(
        trace_test_cfg(sock), server,
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
                            {"config", cfg_path}})
                      .value("ok", false),
                  "create d1 failed");

            // CLI-death: the command goes out, the client vanishes
            // without reading the reply. The device-side duration bound
            // must still finalize the recording.
            uds_send_and_vanish(
                s, nlohmann::json({{"cmd", "trace_start"},
                                   {"id", "d1"},
                                   {"path", trace_path},
                                   {"duration_sec", 1}})
                       .dump() +
                       "\n");

            std::string reason;
            for (int i = 0; i < 500; ++i) {
                const auto st = rpc_json({{"cmd", "status"}, {"id", "d1"}});
                if (st.value("ok", false) && st.contains("trace") &&
                    st["trace"].value("state", "") == "stopped") {
                    reason = st["trace"].value("reason", "");
                    break;
                }
                std::this_thread::sleep_for(20ms);
            }
            check(reason == "expired",
                  "dead-client recording never finalized");
            // The supervisor survived the vanished client.
            check(rpc_json({{"cmd", "hello"}}).value("ok", false),
                  "daemon unusable after client disconnect");
        });
    REQUIRE(failures == 0);

    const auto blob = read_whole_file(trace_path);
    auto parsed = format::trace::parse(blob);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 1);
}

TEST_CASE("integration: trace recording rejects bad requests cleanly",
          "[integration]") {
    test::TempDir dir;
    const auto img = make_test_image(dir, 87);
    const std::string sock = dir / "supervisor.sock";
    const std::string layer_dir = dir / "layer0";
    const std::string trace_path = dir / "out.trace";
    std::filesystem::create_directories(layer_dir);
    TraceBlobServer server({{img.digest, img.blob}}, 19210);
    const std::string cfg_path =
        write_image_config(dir, server.repo_base(), img, layer_dir);

    auto guard = block_daemon_signals();

    const int failures = run_trace_daemon_case(
        trace_test_cfg(sock), server,
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
            // Protocol-level validation (answered, not dropped).
            const auto missing = rpc_json({{"cmd", "trace_start"},
                                           {"id", "ghost"}});
            check(missing.value("ok", true) == false &&
                      missing["error"].get<std::string>().find(
                          "requires") != std::string::npos,
                  "missing fields not a protocol error");
            const auto bad_dur_type = rpc_json({{"cmd", "trace_start"},
                                                {"id", "ghost"},
                                                {"path", "/tmp/x.trace"},
                                                {"duration_sec", "60"}});
            check(bad_dur_type.value("ok", true) == false,
                  "string duration not a protocol error");
            const auto unknown = rpc_json({{"cmd", "trace_start"},
                                           {"id", "ghost"},
                                           {"path", "/tmp/x.trace"},
                                           {"duration_sec", 60}});
            check(unknown.value("ok", true) == false &&
                      unknown["error"].get<std::string>().find(
                          "no such device") != std::string::npos,
                  "unknown id not an error");
            const auto unknown_stop =
                rpc_json({{"cmd", "trace_stop"}, {"id", "ghost"}});
            check(unknown_stop.value("ok", true) == false,
                  "trace_stop on unknown id not an error");

            check(rpc_json({{"cmd", "create"},
                            {"id", "d1"},
                            {"config", cfg_path}})
                      .value("ok", false),
                  "create d1 failed");

            // Stop with no recording: clean device-propagated error.
            const auto idle_stop =
                rpc_json({{"cmd", "trace_stop"}, {"id", "d1"}});
            check(idle_stop.value("ok", true) == false &&
                      idle_stop["error"].get<std::string>().find(
                          "no trace recording in progress") !=
                          std::string::npos,
                  "idle stop error mismatch");

            // Out-of-bounds duration: the device rejects.
            const auto bad_dur = rpc_json({{"cmd", "trace_start"},
                                           {"id", "d1"},
                                           {"path", trace_path},
                                           {"duration_sec", 0}});
            check(bad_dur.value("ok", true) == false &&
                      bad_dur["error"].get<std::string>().find(
                          "duration_sec") != std::string::npos,
                  "zero duration not rejected");

            check(rpc_json({{"cmd", "trace_start"},
                            {"id", "d1"},
                            {"path", trace_path},
                            {"duration_sec", 300}})
                      .value("ok", false),
                  "trace_start failed");
            // Double start: precise error, first recording unaffected.
            const auto twice = rpc_json({{"cmd", "trace_start"},
                                         {"id", "d1"},
                                         {"path", trace_path},
                                         {"duration_sec", 300}});
            check(twice.value("ok", true) == false &&
                      twice["error"].get<std::string>().find(
                          "already in progress") != std::string::npos,
                  "double start error mismatch");
            std::this_thread::sleep_for(500ms);
            check(rpc_json({{"cmd", "trace_stop"}, {"id", "d1"}})
                      .value("ok", false),
                  "trace_stop after double start failed");
            check(rpc_json({{"cmd", "hello"}}).value("ok", false),
                  "daemon unusable after bad trace commands");
        });
    REQUIRE(failures == 0);

    auto parsed = format::trace::parse(read_whole_file(trace_path));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 1);
}

TEST_CASE("integration: trace recording crash mid-record marks the trace lost",
          "[integration]") {
    // A device dying mid-recording takes its queued records with it
    // (memory-only until finalize): the supervisor must not leave a
    // "recording" status standing — status reports the trace "lost".
    test::TempDir dir;
    const auto img = make_test_image(dir, 89);
    const std::string sock = dir / "supervisor.sock";
    const std::string layer_dir = dir / "layer0";
    const std::string trace_path = dir / "out.trace";
    std::filesystem::create_directories(layer_dir);
    TraceBlobServer server({{img.digest, img.blob}}, 19211);
    const std::string cfg_path =
        write_image_config(dir, server.repo_base(), img, layer_dir);

    auto guard = block_daemon_signals();

    const int failures = run_trace_daemon_case(
        trace_test_cfg(sock), server,
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
                            {"config", cfg_path}})
                      .value("ok", false),
                  "create d1 failed");
            check(rpc_json({{"cmd", "trace_start"},
                            {"id", "d1"},
                            {"path", trace_path},
                            {"duration_sec", 300}})
                      .value("ok", false),
                  "trace_start failed");
            const auto st1 = rpc_json({{"cmd", "status"}, {"id", "d1"}});
            check(st1.value("ok", false) && st1.contains("trace") &&
                      st1["trace"].value("state", "") == "recording",
                  "status does not show recording");
            const int pid = st1.value("pid", -1);
            if (pid <= 0) {
                check(false, "no device pid in status");
                return;
            }

            // Crash the device mid-record (SIGKILL: no shutdown
            // finalize can run).
            if (::kill(pid, SIGKILL) != 0) {
                check(false, "cannot SIGKILL the device");
                return;
            }
            std::string state, reason;
            for (int i = 0; i < 500; ++i) {
                const auto st = rpc_json({{"cmd", "status"}, {"id", "d1"}});
                if (st.value("ok", false) && st.contains("trace") &&
                    st["trace"].value("state", "") == "lost") {
                    state = "lost";
                    reason = st["trace"].value("reason", "");
                    break;
                }
                std::this_thread::sleep_for(20ms);
            }
            check(state == "lost", "trace not marked lost after crash");
            check(reason == "device_exit",
                  "lost trace reason not 'device_exit'");
            check(rpc_json({{"cmd", "hello"}}).value("ok", false),
                  "daemon unusable after device crash");
        });
    REQUIRE(failures == 0);

    // The output file was O_TRUNC-created at start but never finalized:
    // it is empty and not a valid trace blob — the "lost" status, not
    // the file, is the source of truth for a crashed recording.
    struct stat st {};
    REQUIRE(::stat(trace_path.c_str(), &st) == 0);
    REQUIRE(st.st_size == 0);
}
