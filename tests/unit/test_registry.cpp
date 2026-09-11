// Unit tests: registry client against a mock HTTP registry (Elio server).
// Covers Range reads, size probes, bearer-token auth, redirect caching and
// the DART prefix passthrough (ADR-0005).
#include "source/registry.hpp"

#include "../support.hpp"

#include <elio/http/http_server.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/sync/event.hpp>
#include <elio/time/timer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cerrno>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <system_error>
#include <utility>

using namespace obd;
namespace http = elio::http;

namespace {



/// A mock OCI registry speaking the subset our client needs:
///   /v2/blobs/<x>     — plain Range-capable blob
///   /v2/auth/<x>      — 401 + Bearer challenge, then token-gated
///   /token            — issues {"token":...} for Basic u:p; counts hits,
///                       optional expires_in, per-exchange serial tokens
///   /v2/redir/<x>     — 302 to /cdn/blob (anonymous)
///   /cdn/blob         — Range-capable, no auth
///   /dart/<upstream>  — DART-style prefix passthrough echo endpoint
class MockRegistry {
public:
    explicit MockRegistry(std::vector<uint8_t> blob)
        : blob_(std::move(blob)),
          port_lease_(), port_(port_lease_.port()) {
        http::router r;
        r.add_route(http::method::GET, "/v2/*",
                    [this](http::context& ctx) { return blob_handler(ctx); });
        r.add_route(http::method::GET, "/token",
                    [this](http::context& ctx) { return token_handler(ctx); });
        r.add_route(http::method::GET, "/cdn/*",
                    [this](http::context& ctx) { return cdn_handler(ctx); });
        r.add_route(http::method::GET, "/dart/*",
                    [this](http::context& ctx) { return dart_handler(ctx); });
        server_ = std::make_unique<http::server>(std::move(r));
    }

    elio::coro::task<void> run() {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            port_lease_.reset();
            port_ = port_lease_.port();
            port_lease_.release();
            if (before_listen_) before_listen_(port_);
            try {
                errno = 0;
                co_await server_->listen(
                    elio::net::socket_address(
                        elio::net::ipv4_address("127.0.0.1", port_)));
                const int listen_errno = errno;
                if (!stop_requested_.load(std::memory_order_acquire) &&
                    !server_->is_running()) {
                    test::require_retryable_http_listen_return(
                        "MockRegistry listen", listen_errno);
                }
            } catch (const std::system_error& e) {
                if (e.code().value() != EADDRINUSE) throw;
            }
            if (!stop_requested_.load(std::memory_order_acquire)) {
                co_await elio::time::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }
    void stop() {
        stop_requested_.store(true, std::memory_order_release);
        server_->stop();
    }
    bool is_running() const noexcept { return server_->is_running(); }
    void set_before_listen_hook(std::function<void(uint16_t)> hook) {
        before_listen_ = std::move(hook);
    }
    bool drained() const { return server_->active_connections() == 0; }
    /// The OS-assigned loopback port this mock listens on (ephemeral:
    /// picked at construction via port 0, issue #15).
    uint16_t port() const noexcept { return port_; }
    /// Origin for building test URLs ("http://127.0.0.1:<port>").
    std::string origin() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    /// Registry base ("http://127.0.0.1:<port>/v2").
    std::string repo_base() const { return origin() + "/v2"; }

private:
    static std::optional<std::pair<uint64_t, uint64_t>> parse_range(
        std::string_view v) {
        if (!v.starts_with("bytes=")) return std::nullopt;
        v.remove_prefix(6);
        const auto dash = v.find('-');
        if (dash == std::string_view::npos) return std::nullopt;
        return std::pair{std::stoull(std::string(v.substr(0, dash))),
                         std::stoull(std::string(v.substr(dash + 1)))};
    }

    elio::coro::task<http::response> serve_range(
        const http::request& req) {
        const auto range = parse_range(req.header("Range"));
        if (!range) {
            co_return http::response(
                http::status::ok,
                std::string_view(
                    reinterpret_cast<const char*>(blob_.data()),
                    blob_.size()));
        }
        const uint64_t first = range->first;
        const uint64_t last = std::min<uint64_t>(range->second,
                                                 blob_.size() - 1);
        if (first >= blob_.size() || first > last) {
            http::response resp(http::status::range_not_satisfiable);
            resp.set_header("Content-Length", "0");
            resp.set_header("Content-Range",
                            "bytes */" + std::to_string(blob_.size()));
            co_return resp;
        }
        http::response resp(
            http::status::partial_content,
            std::string_view(
                reinterpret_cast<const char*>(blob_.data() + first),
                last - first + 1));
        resp.set_header("Content-Range", "bytes " + std::to_string(first) +
                                             "-" + std::to_string(last) +
                                             "/" +
                                             std::to_string(blob_.size()));
        co_return resp;
    }

    elio::coro::task<http::response> blob_handler(http::context& ctx) {
        const std::string path(ctx.req().path());
        if (path.starts_with("/v2/auth/")) {
            const std::string_view auth = ctx.req().header("Authorization");
            bool accepted = false;
            if (auth.starts_with("Bearer ")) {
                const std::string_view presented = auth.substr(7);
                if (serial_tokens_) {
                    // "<prefix>-<serial>"; accept only serials above the
                    // configured watermark (models server-side expiry of
                    // the older, already-issued tokens).
                    const std::string pfx = token_prefix_ + "-";
                    if (presented.starts_with(pfx)) {
                        try {
                            const long serial = std::stol(std::string(
                                presented.substr(pfx.size())));
                            accepted =
                                serial > accept_serial_above_.load();
                        } catch (const std::exception&) {
                        }
                    }
                } else {
                    accepted = presented == token_prefix_;
                }
                if (accepted && reject_data_auth_ &&
                    ctx.req().header("Range") != "bytes=0-0") {
                    // Models a registry that accepts the token on the
                    // 0-0 probes but rejects it on real data GETs.
                    accepted = false;
                }
            }
            if (!accepted) {
                http::response resp(http::status::unauthorized);
                resp.set_header("Content-Length", "0");
                resp.set_header(
                    "WWW-Authenticate",
                    "Bearer realm=\"http://127.0.0.1:" +
                        std::to_string(port_) +
                        "/token\",service=\"test\",scope=\"repository:x\"");
                co_return resp;
            }
            co_return co_await serve_range(ctx.req());
        }
        if (path.starts_with("/v2/redir/")) {
            http::response resp(http::status::found);
            resp.set_header("Content-Length", "0");
            resp.set_header("Location", "http://127.0.0.1:" +
                                            std::to_string(port_) +
                                            "/cdn/blob");
            co_return resp;
        }
        co_return co_await serve_range(ctx.req());
    }

    elio::coro::task<http::response> token_handler(http::context& ctx) {
        const std::string_view auth = ctx.req().header("Authorization");
        if (auth != "Basic dTpw") {  // u:p
            http::response resp(http::status::unauthorized);
            resp.set_header("Content-Length", "0");
            co_return resp;
        }
        const int serial = ++token_hits_;
        if (token_delay_ms_ > 0) {
            // Widen the race window so concurrent re-auths genuinely
            // overlap (single-flight tests count this endpoint's hits).
            co_await elio::time::sleep_for(
                std::chrono::milliseconds(token_delay_ms_.load()));
        }
        if (token_endpoint_rejects_) {
            // Models a token endpoint that is down / rejecting the Basic
            // credentials: every exchange fails.
            http::response resp(http::status::unauthorized);
            resp.set_header("Content-Length", "0");
            co_return resp;
        }
        {
            std::lock_guard<std::mutex> g(body_mu_);
            if (!token_body_override_.empty()) {
                // Hostile-input tests: raw response body, verbatim.
                co_return http::response(http::status::ok,
                                         token_body_override_,
                                         "application/json");
            }
        }
        std::string token = token_prefix_;
        if (serial_tokens_) token += "-" + std::to_string(serial);
        std::string body = "{\"token\":\"" + token + "\"";
        if (expires_in_ >= 0) {
            body += ",\"expires_in\":" + std::to_string(expires_in_.load());
        }
        body += "}";
        co_return http::response(http::status::ok, body, "application/json");
    }

    elio::coro::task<http::response> cdn_handler(http::context& ctx) {
        co_return co_await serve_range(ctx.req());
    }

    elio::coro::task<http::response> dart_handler(http::context& ctx) {
        // The DART prefix contract: the remainder of the path is the full
        // upstream URL, scheme included. Our mock answers it directly.
        last_dart_path_ = std::string(ctx.req().path());
        co_return co_await serve_range(ctx.req());
    }

    std::vector<uint8_t> blob_;
    test::ReservedTcpPort port_lease_;
    uint16_t port_;
    std::unique_ptr<http::server> server_;
    std::function<void(uint16_t)> before_listen_;
    std::atomic<bool> stop_requested_{false};
    std::mutex body_mu_;
    std::string token_body_override_;  // under body_mu_
public:
    std::string last_dart_path_;
    // Auth knobs; set before the traffic they should affect. Defaults
    // reproduce the original static-token mock ("sekrit", no expires_in).
    std::string token_prefix_ = "sekrit";
    std::atomic<bool> serial_tokens_{false};   // issue "<prefix>-<n>"
    std::atomic<long> accept_serial_above_{-1};  // serial acceptance floor
    std::atomic<bool> reject_data_auth_{false};  // 401 non-probe auth'd GETs
    std::atomic<bool> token_endpoint_rejects_{false};  // fail all exchanges
    std::atomic<int> expires_in_{-1};            // <0: omit the field
    std::atomic<int> token_delay_ms_{0};
    std::atomic<int> token_hits_{0};             // token endpoint exchanges

    /// Makes /token return `body` verbatim (empty restores normal issue).
    void set_token_body_override(std::string body) {
        std::lock_guard<std::mutex> g(body_mu_);
        token_body_override_ = std::move(body);
    }
};

struct MockGuard {
    MockRegistry& mock;
    ~MockGuard() { mock.stop(); }
};

elio::coro::task<void> wait_drained(MockRegistry& mock) {
    for (int i = 0; i < 200 && !mock.drained(); ++i) {
        co_await elio::time::sleep_for(std::chrono::milliseconds(5));
    }
}

/// Countdown latch for joining N elio::go children from a coroutine.
struct JoinLatch {
    explicit JoinLatch(int n) : pending(n) {}
    std::atomic<int> pending;
    elio::sync::event done;
};

source::CredentialStorePtr test_creds() {
    auto store = std::make_shared<source::CredentialStore>();
    store->add("127.0.0.1", {"u", "p"});
    return store;
}

}  // namespace

TEST_CASE("source: mock registry refreshes port after bind collision",
          "[source]") {
    const int rc = test::run_coro([]() -> elio::coro::task<int> {
        auto blob = test::pattern_bytes(64 * 1024, 20);
        MockRegistry mock(blob);
        std::optional<test::TcpPortBlocker> blocker;
        std::atomic<bool> armed{true};
        std::atomic<uint16_t> blocked_port{0};
        mock.set_before_listen_hook([&](uint16_t port) {
            if (!armed.exchange(false)) return;
            blocker.emplace(port);
            blocked_port.store(port, std::memory_order_release);
        });
        elio::go([&mock]() -> elio::coro::task<void> {
            co_await mock.run();
        });
        MockGuard guard{mock};
        const bool mock_running = co_await test::wait_server_running(mock);
        REQUIRE(mock_running);
        REQUIRE(blocked_port.load(std::memory_order_acquire) != 0);
        REQUIRE(mock.port() != blocked_port.load(std::memory_order_acquire));

        auto client =
            std::make_shared<source::RegistryClient>(
                nullptr, source::RegistryClientConfig{});
        const std::string url = mock.repo_base() + "/blobs/x";
        auto src = co_await source::RegistrySource::open(client, url);
        std::vector<uint8_t> buf(4096);
        const ssize_t r = co_await src->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == std::vector<uint8_t>(blob.begin(),
                                            blob.begin() + buf.size()));
        blocker.reset();
        co_await wait_drained(mock);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: registry range reads and size probe", "[source]") {
    const int rc = test::run_coro([]() -> elio::coro::task<int> {
        auto blob = test::pattern_bytes(300 * 1024, 21);
        MockRegistry mock(blob);
        elio::go([&mock]() -> elio::coro::task<void> {
            co_await mock.run();
        });
        MockGuard guard{mock};
        const bool mock_running = co_await test::wait_server_running(mock);
        REQUIRE(mock_running);

        auto client =
            std::make_shared<source::RegistryClient>(nullptr,
                                                     source::RegistryClientConfig{});
        const std::string url = mock.repo_base() + "/blobs/x";
        auto src = co_await source::RegistrySource::open(client, url);
        REQUIRE(src->size() == blob.size());
        std::vector<uint8_t> buf(100 * 1024);
        const ssize_t r =
            co_await src->pread(buf.data(), buf.size(), 12345);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == std::vector<uint8_t>(blob.begin() + 12345,
                                            blob.begin() + 12345 + buf.size()));
        co_await wait_drained(mock);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: registry bearer auth flow via token endpoint",
          "[source]") {
    const int rc = test::run_coro([]() -> elio::coro::task<int> {
        auto blob = test::pattern_bytes(64 * 1024, 22);
        MockRegistry mock(blob);
        elio::go([&mock]() -> elio::coro::task<void> {
            co_await mock.run();
        });
        MockGuard guard{mock};
        const bool mock_running = co_await test::wait_server_running(mock);
        REQUIRE(mock_running);

        auto client = std::make_shared<source::RegistryClient>(
            test_creds(), source::RegistryClientConfig{});
        const std::string url = mock.repo_base() + "/auth/x";
        auto src = co_await source::RegistrySource::open(client, url);
        REQUIRE(src->size() == blob.size());
        std::vector<uint8_t> buf(4096);
        const ssize_t r = co_await src->pread(buf.data(), buf.size(), 512);
        REQUIRE(r == 4096);
        REQUIRE(buf == std::vector<uint8_t>(blob.begin() + 512,
                                            blob.begin() + 512 + 4096));
        co_await wait_drained(mock);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: registry redirect mode drops auth on the CDN URL",
          "[source]") {
    const int rc = test::run_coro([]() -> elio::coro::task<int> {
        auto blob = test::pattern_bytes(64 * 1024, 23);
        MockRegistry mock(blob);
        elio::go([&mock]() -> elio::coro::task<void> {
            co_await mock.run();
        });
        MockGuard guard{mock};
        const bool mock_running = co_await test::wait_server_running(mock);
        REQUIRE(mock_running);

        const std::string url = mock.repo_base() + "/redir/x";
        auto client = std::make_shared<source::RegistryClient>(
            nullptr, source::RegistryClientConfig{});
        auto src = co_await source::RegistrySource::open(client, url);
        std::vector<uint8_t> buf(1024);
        const ssize_t r = co_await src->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == 1024);
        co_await wait_drained(mock);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: DART prefix passthrough preserves the embedded URL",
          "[source]") {
    const int rc = test::run_coro([]() -> elio::coro::task<int> {
        auto blob = test::pattern_bytes(64 * 1024, 24);
        MockRegistry mock(blob);
        elio::go([&mock]() -> elio::coro::task<void> {
            co_await mock.run();
        });
        MockGuard guard{mock};
        const bool mock_running = co_await test::wait_server_running(mock);
        REQUIRE(mock_running);

        source::RegistryClientConfig cfg;
        cfg.accelerate_base = mock.origin() + "/dart";
        auto client =
            std::make_shared<source::RegistryClient>(nullptr, cfg);
        const std::string url = mock.repo_base() + "/blobs/x";
        auto src = co_await source::RegistrySource::open(client, url);
        std::vector<uint8_t> buf(2048);
        const ssize_t r = co_await src->pread(buf.data(), buf.size(), 100);
        REQUIRE(r == 2048);
        REQUIRE(buf == std::vector<uint8_t>(blob.begin() + 100,
                                            blob.begin() + 100 + 2048));
        // The mock saw the full passthrough path, double slashes intact.
        REQUIRE(mock.last_dart_path_ == "/dart/" + url);
        co_await wait_drained(mock);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: registry concurrent 401s share one token refresh",
          "[source]") {
    const int rc = test::run_coro([]() -> elio::coro::task<int> {
        auto blob = test::pattern_bytes(256 * 1024, 25);
        MockRegistry mock(blob);
        mock.serial_tokens_ = true;
        mock.expires_in_ = 100;      // refreshed tokens live 80 s: no re-auth
        mock.token_delay_ms_ = 30;   // make the concurrent 401s overlap
        elio::go([&mock]() -> elio::coro::task<void> {
            co_await mock.run();
        });
        MockGuard guard{mock};
        const bool mock_running = co_await test::wait_server_running(mock);
        REQUIRE(mock_running);

        auto client = std::make_shared<source::RegistryClient>(
            test_creds(), source::RegistryClientConfig{});
        const std::string url = mock.repo_base() + "/auth/x";
        auto src = co_await source::RegistrySource::open(client, url);
        REQUIRE(mock.token_hits_.load() == 1);

        // Server-side expiry: the token cached at open (serial 1) is now
        // rejected; every in-flight read takes a 401 and must re-auth.
        mock.accept_serial_above_ = 1;

        constexpr int kReaders = 8;
        constexpr size_t kCount = 4096;
        auto join = std::make_shared<JoinLatch>(kReaders);
        std::vector<ssize_t> results(kReaders, -1);
        std::vector<std::vector<uint8_t>> bufs(
            kReaders, std::vector<uint8_t>(kCount));
        for (int i = 0; i < kReaders; ++i) {
            elio::go([&, i]() -> elio::coro::task<void> {
                results[i] = co_await src->pread(bufs[i].data(), kCount,
                                                 i * 8192);
                if (join->pending.fetch_sub(1) == 1) join->done.set();
                co_return;
            });
        }
        co_await join->done.wait();

        for (int i = 0; i < kReaders; ++i) {
            REQUIRE(results[i] == static_cast<ssize_t>(kCount));
            REQUIRE(bufs[i] ==
                    std::vector<uint8_t>(blob.begin() + i * 8192,
                                         blob.begin() + i * 8192 + kCount));
        }
        // One exchange at open plus exactly ONE coalesced refresh for all
        // eight concurrent 401s (un-coalesced this would be 1 + 8).
        REQUIRE(mock.token_hits_.load() == 2);
        co_await wait_drained(mock);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: registry 401 retry budget is bounded when re-auth keeps failing", "[source]") {
    const int rc = test::run_coro([]() -> elio::coro::task<int> {
        auto blob = test::pattern_bytes(64 * 1024, 26);
        MockRegistry mock(blob);
        // The mock accepts every fresh token on the 0-0 probes but rejects
        // it on data GETs: re-auth never helps, so the request must fail
        // within its retry budget instead of looping on token exchanges.
        mock.reject_data_auth_ = true;
        elio::go([&mock]() -> elio::coro::task<void> {
            co_await mock.run();
        });
        MockGuard guard{mock};
        const bool mock_running = co_await test::wait_server_running(mock);
        REQUIRE(mock_running);

        auto client = std::make_shared<source::RegistryClient>(
            test_creds(), source::RegistryClientConfig{});
        const std::string url = mock.repo_base() + "/auth/x";
        auto src = co_await source::RegistrySource::open(client, url);
        REQUIRE(mock.token_hits_.load() == 1);

        std::vector<uint8_t> buf(4096);
        const ssize_t r = co_await src->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == -EPERM);
        // One exchange per data-request attempt (the generation rule
        // forbids retrying with the just-rejected token), then -EPERM:
        // bounded retries, no livelock.
        REQUIRE(mock.token_hits_.load() == 3);
        co_await wait_drained(mock);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: registry re-auths after expires_in lifetime elapses",
          "[source]") {
    const int rc = test::run_coro([]() -> elio::coro::task<int> {
        auto blob = test::pattern_bytes(64 * 1024, 27);
        MockRegistry mock(blob);
        mock.expires_in_ = 1;  // cache lifetime: 80% of 1 s = 800 ms
        elio::go([&mock]() -> elio::coro::task<void> {
            co_await mock.run();
        });
        MockGuard guard{mock};
        const bool mock_running = co_await test::wait_server_running(mock);
        REQUIRE(mock_running);

        auto client = std::make_shared<source::RegistryClient>(
            test_creds(), source::RegistryClientConfig{});
        const std::string base = mock.repo_base() + "/auth/";
        auto src_a = co_await source::RegistrySource::open(client,
                                                           base + "a");
        REQUIRE(mock.token_hits_.load() == 1);
        // A new URL resolves against the same token key; within the 800 ms
        // lifetime the cached token is reused.
        auto src_b = co_await source::RegistrySource::open(client,
                                                           base + "b");
        REQUIRE(mock.token_hits_.load() == 1);

        co_await elio::time::sleep_for(std::chrono::milliseconds(1200));
        // Past 800 ms the token is stale: the next resolution re-auths.
        auto src_c = co_await source::RegistrySource::open(client,
                                                           base + "c");
        REQUIRE(mock.token_hits_.load() == 2);

        std::vector<uint8_t> buf(1024);
        // NB: co_await inside REQUIRE gets evaluated multiple times by the
        // Catch2 decomposition — always hoist side-effecting awaits out.
        const ssize_t read_c = co_await src_c->pread(buf.data(), buf.size(), 0);
        REQUIRE(read_c == 1024);
        REQUIRE(buf == std::vector<uint8_t>(blob.begin(),
                                            blob.begin() + 1024));
        REQUIRE(mock.token_hits_.load() == 2);
        co_await wait_drained(mock);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: registry self-mode URL cache follows bearer expiry",
          "[source]") {
    const int rc = test::run_coro([]() -> elio::coro::task<int> {
        auto blob = test::pattern_bytes(64 * 1024, 29);
        MockRegistry mock(blob);
        mock.expires_in_ = 1;  // cache lifetime: 80% of 1 s = 800 ms
        elio::go([&mock]() -> elio::coro::task<void> {
            co_await mock.run();
        });
        MockGuard guard{mock};
        const bool mock_running = co_await test::wait_server_running(mock);
        REQUIRE(mock_running);

        auto client = std::make_shared<source::RegistryClient>(
            test_creds(), source::RegistryClientConfig{});
        const std::string url = mock.repo_base() + "/auth/x";
        auto src = co_await source::RegistrySource::open(client, url);
        REQUIRE(mock.token_hits_.load() == 1);

        std::vector<uint8_t> buf(1024);
        const ssize_t read_before =
            co_await src->pread(buf.data(), buf.size(), 0);
        REQUIRE(read_before == 1024);
        REQUIRE(mock.token_hits_.load() == 1);

        co_await elio::time::sleep_for(std::chrono::milliseconds(1200));
        // Same URL and same already-open source: the URL-info cache must not
        // keep a Self-mode Bearer Authorization header past the token's
        // proactive expiry. The mock still accepts the old token, so this
        // proves refresh happened before any rejected data request.
        const ssize_t read_after =
            co_await src->pread(buf.data(), buf.size(), 4096);
        REQUIRE(read_after == 1024);
        REQUIRE(mock.token_hits_.load() == 2);
        co_await wait_drained(mock);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: registry keeps the cached token within expires_in lifetime", "[source]") {
    const int rc = test::run_coro([]() -> elio::coro::task<int> {
        auto blob = test::pattern_bytes(64 * 1024, 28);
        MockRegistry mock(blob);
        mock.expires_in_ = 100;  // cache lifetime: 80 s
        elio::go([&mock]() -> elio::coro::task<void> {
            co_await mock.run();
        });
        MockGuard guard{mock};
        const bool mock_running = co_await test::wait_server_running(mock);
        REQUIRE(mock_running);

        auto client = std::make_shared<source::RegistryClient>(
            test_creds(), source::RegistryClientConfig{});
        const std::string base = mock.repo_base() + "/auth/";
        auto src_a = co_await source::RegistrySource::open(client,
                                                           base + "a");
        co_await elio::time::sleep_for(std::chrono::milliseconds(300));
        // Far inside the 80 s lifetime: further resolutions and reads
        // never touch the token endpoint again.
        auto src_b = co_await source::RegistrySource::open(client,
                                                           base + "b");
        std::vector<uint8_t> buf(1024);
        const ssize_t read_a = co_await src_a->pread(buf.data(), buf.size(), 7);
        REQUIRE(read_a == 1024);
        const ssize_t read_b = co_await src_b->pread(buf.data(), buf.size(), 9);
        REQUIRE(read_b == 1024);
        REQUIRE(mock.token_hits_.load() == 1);
        co_await wait_drained(mock);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: registry failed token refresh reaches all concurrent waiters", "[source]") {
    const int rc = test::run_coro([]() -> elio::coro::task<int> {
        auto blob = test::pattern_bytes(128 * 1024, 29);
        MockRegistry mock(blob);
        mock.serial_tokens_ = true;
        mock.expires_in_ = 0;      // every resolution needs a fresh exchange
        mock.token_delay_ms_ = 30; // make the concurrent 401s overlap
        elio::go([&mock]() -> elio::coro::task<void> {
            co_await mock.run();
        });
        MockGuard guard{mock};
        const bool mock_running = co_await test::wait_server_running(mock);
        REQUIRE(mock_running);

        auto client = std::make_shared<source::RegistryClient>(
            test_creds(), source::RegistryClientConfig{});
        const std::string url = mock.repo_base() + "/auth/x";
        auto src = co_await source::RegistrySource::open(client, url);
        REQUIRE(mock.token_hits_.load() == 1);

        // The cached token is now stale server-side AND the token endpoint
        // rejects every exchange: the coalesced refresh must fail for all
        // waiters, with exactly one exchange attempted.
        mock.accept_serial_above_ = 1;
        mock.token_endpoint_rejects_ = true;

        constexpr int kReaders = 8;
        constexpr size_t kCount = 2048;
        auto join = std::make_shared<JoinLatch>(kReaders);
        std::vector<ssize_t> results(kReaders, 0);
        std::vector<std::vector<uint8_t>> bufs(
            kReaders, std::vector<uint8_t>(kCount));
        for (int i = 0; i < kReaders; ++i) {
            elio::go([&, i]() -> elio::coro::task<void> {
                results[i] = co_await src->pread(bufs[i].data(), kCount,
                                                 i * 4096);
                if (join->pending.fetch_sub(1) == 1) join->done.set();
                co_return;
            });
        }
        co_await join->done.wait();

        // Every waiter receives the leader's error — no hang, no success.
        for (int i = 0; i < kReaders; ++i) {
            REQUIRE(results[i] == -EPERM);
        }
        REQUIRE(mock.token_hits_.load() == 2);

        // A failed flight must not poison the key: once the endpoint
        // recovers, the next request starts a fresh flight and succeeds.
        mock.token_endpoint_rejects_ = false;
        std::vector<uint8_t> buf(2048);
        const ssize_t r = co_await src->pread(buf.data(), buf.size(), 512);
        REQUIRE(r == 2048);
        REQUIRE(buf == std::vector<uint8_t>(blob.begin() + 512,
                                            blob.begin() + 512 + 2048));
        REQUIRE(mock.token_hits_.load() == 3);
        co_await wait_drained(mock);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: registry survives hostile token endpoint fields", "[source]") {
    const int rc = test::run_coro([]() -> elio::coro::task<int> {
        auto blob = test::pattern_bytes(64 * 1024, 30);
        MockRegistry mock(blob);
        elio::go([&mock]() -> elio::coro::task<void> {
            co_await mock.run();
        });
        MockGuard guard{mock};
        const bool mock_running = co_await test::wait_server_running(mock);
        REQUIRE(mock_running);

        auto client = std::make_shared<source::RegistryClient>(
            test_creds(), source::RegistryClientConfig{});
        const std::string base = mock.repo_base() + "/auth/";

        // A float expires_in far outside int64 range (1e100): converting
        // it would be undefined behavior; it must be ignored and the
        // token cached with the fixed fallback lifetime instead.
        mock.set_token_body_override(
            R"({"token":"sekrit","expires_in":1e100})");
        auto src_a = co_await source::RegistrySource::open(client,
                                                           base + "a");
        REQUIRE(mock.token_hits_.load() == 1);
        auto src_b = co_await source::RegistrySource::open(client,
                                                           base + "b");
        std::vector<uint8_t> buf(1024);
        const ssize_t read_b = co_await src_b->pread(buf.data(), buf.size(), 0);
        REQUIRE(read_b == 1024);
        REQUIRE(buf == std::vector<uint8_t>(blob.begin(),
                                            blob.begin() + 1024));
        REQUIRE(mock.token_hits_.load() == 1);

        // A non-string token field maps to format_error → -EINVAL through
        // the -errno discipline; it must never escape as a raw exception
        // (single-flight would fan it out to every waiter).
        mock.set_token_body_override(R"({"token":123})");
        auto client2 = std::make_shared<source::RegistryClient>(
            test_creds(), source::RegistryClientConfig{});
        const int64_t bad = co_await client2->get_length(base + "c");
        REQUIRE(bad == -EINVAL);
        REQUIRE(mock.token_hits_.load() == 2);

        // A partially-numeric string expires_in must NOT earn the declared
        // lifetime (std::stoll alone would parse the "1" prefix of
        // "1junk"): it takes the 30 s fallback, so a resolution 1.2 s
        // later still reuses the token — a bogus 80%-of-1-s lifetime
        // would have expired after 800 ms.
        mock.set_token_body_override(
            R"({"token":"sekrit","expires_in":"1junk"})");
        auto client3 = std::make_shared<source::RegistryClient>(
            test_creds(), source::RegistryClientConfig{});
        auto src_junk = co_await source::RegistrySource::open(client3,
                                                              base + "d");
        REQUIRE(mock.token_hits_.load() == 3);
        co_await elio::time::sleep_for(std::chrono::milliseconds(1200));
        auto src_junk2 = co_await source::RegistrySource::open(client3,
                                                               base + "e");
        REQUIRE(mock.token_hits_.load() == 3);

        // A fully-numeric string expires_in IS honored ("1" → 800 ms):
        // the same 1.2 s spacing forces a fresh exchange.
        mock.set_token_body_override(
            R"({"token":"sekrit","expires_in":"1"})");
        auto client4 = std::make_shared<source::RegistryClient>(
            test_creds(), source::RegistryClientConfig{});
        auto src_str = co_await source::RegistrySource::open(client4,
                                                             base + "f");
        REQUIRE(mock.token_hits_.load() == 4);
        co_await elio::time::sleep_for(std::chrono::milliseconds(1200));
        auto src_str2 = co_await source::RegistrySource::open(client4,
                                                              base + "g");
        REQUIRE(mock.token_hits_.load() == 5);
        co_await wait_drained(mock);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: registry token cache lifetime derives from expires_in",
          "[source]") {
    using namespace std::chrono;
    using source::detail::token_cache_lifetime;
    // 80% of the declared lifetime (proactive refresh margin).
    REQUIRE(token_cache_lifetime(100) == seconds(80));
    REQUIRE(token_cache_lifetime(10) == seconds(8));
    // An immediately-expiring token is cached as already expired.
    REQUIRE(token_cache_lifetime(0) == seconds(0));
    // Absent or negative (garbage) values fall back to the fixed 30 s.
    REQUIRE(token_cache_lifetime(std::nullopt) == seconds(30));
    REQUIRE(token_cache_lifetime(-5) == seconds(30));
    // Absurd declared lifetimes are capped at 7 days instead of pinning
    // the token forever or overflowing the arithmetic.
    constexpr auto kCap = milliseconds(7LL * 24 * 3600 * 800);
    REQUIRE(token_cache_lifetime(int64_t{1} << 62) == kCap);
    REQUIRE(token_cache_lifetime(std::numeric_limits<int64_t>::max()) ==
            kCap);
    REQUIRE(token_cache_lifetime(7LL * 24 * 3600) == kCap);
    REQUIRE(token_cache_lifetime(7LL * 24 * 3600 + 1) == kCap);
    REQUIRE(token_cache_lifetime(7LL * 24 * 3600 - 1) < kCap);
}
