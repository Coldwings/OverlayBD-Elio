// Unit tests: registry client against a mock HTTP registry (Elio server).
// Covers Range reads, size probes, bearer-token auth, redirect caching and
// the DART prefix passthrough (ADR-0005).
#include "source/registry.hpp"

#include "../support.hpp"

#include <elio/http/http_server.hpp>
#include <elio/net/tcp.hpp>
#include <elio/net/stream.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/sync/event.hpp>
#include <elio/time/timer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <cstdio>
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

/// A raw TCP peer for framing and connection-retirement checks. Unlike the
/// HTTP server mock, it can deliberately truncate a chunk trailer or leave
/// an error body unread. Each accepted connection serves sequential requests.
class RawRegistry {
public:
    struct Reply {
        std::string wire;
        size_t fragment_size = 4096;
        bool close_after = false;
        elio::sync::event* late_release = nullptr;
        std::string late_wire = {};
    };
    using Plan = std::function<Reply(const std::string&)>;

    explicit RawRegistry(Plan plan, bool automatic_probes = true)
        : plan_(std::move(plan)), automatic_probes_(automatic_probes) {
        listener_ = elio::net::tcp_listener::bind(
            elio::net::ipv4_address("127.0.0.1", 0));
        if (!listener_) throw std::system_error(errno, std::generic_category(),
                                                 "raw registry bind");
    }
    std::string url(std::string_view path = "/blob") const {
        return "http://127.0.0.1:" +
            std::to_string(listener_->local_address().port()) + std::string(path);
    }
    void stop() { stopped_.cancel(); }
    size_t connections() const noexcept { return connections_.load(); }
    size_t requests() const noexcept { return requests_.load(); }
    size_t data_requests() const noexcept { return data_requests_.load(); }

    elio::coro::task<void> run() {
        auto token = stopped_.get_token();
        while (!token.is_cancelled()) {
            auto accepted = co_await listener_->accept(token);
            if (!accepted) co_return;
            elio::net::stream stream(std::move(*accepted));
            bool counted = false;
            std::string pending;
            for (;;) {
                while (pending.find("\r\n\r\n") == std::string::npos) {
                    std::array<char, 1024> buffer;
                    auto read = co_await stream.read(buffer.data(), buffer.size(), token);
                    if (read.result <= 0) break;
                    pending.append(buffer.data(), static_cast<size_t>(read.result));
                    if (pending.size() > 16384) throw std::runtime_error("oversized test request");
                }
                auto end = pending.find("\r\n\r\n");
                if (end == std::string::npos) break;
                const std::string request = pending.substr(0, end + 4);
                pending.erase(0, end + 4);
                if (!counted) { ++connections_; counted = true; }
                ++requests_;
                const bool probe = request.find("Range: bytes=0-0\r\n") != std::string::npos;
                if (probe && automatic_probes_) {
                    auto written = co_await stream.write_all(
                        "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-0/1048576\r\n"
                        "Content-Length: 1\r\n\r\np", token);
                    if (written.result < 0) break;
                    continue;
                }
                if (!probe) ++data_requests_;
                const Reply reply = plan_(request);
                bool failed = false;
                for (size_t pos = 0; pos < reply.wire.size(); pos += reply.fragment_size) {
                    auto written = co_await stream.write_all(
                        std::string_view(reply.wire).substr(pos, reply.fragment_size), token);
                    if (written.result < 0) { failed = true; break; }
                    co_await elio::time::sleep_for(std::chrono::microseconds(10), token);
                }
                if (!failed && reply.late_release) {
                    const auto released = co_await reply.late_release->wait(token);
                    if (released == elio::coro::cancel_result::completed) {
                        const auto written = co_await stream.write_all(reply.late_wire, token);
                        failed = written.result < 0;
                    }
                }
                if (failed || reply.close_after) break;
            }
        }
    }

private:
    Plan plan_;
    bool automatic_probes_;
    std::optional<elio::net::tcp_listener> listener_;
    elio::coro::cancel_source stopped_;
    std::atomic<size_t> connections_{0};
    std::atomic<size_t> requests_{0};
    std::atomic<size_t> data_requests_{0};
};

template <typename Consumer>
elio::coro::task<void> with_raw_registry(RawRegistry& peer, Consumer consumer) {
    auto server = elio::spawn(peer.run());
    std::exception_ptr failure;
    try { co_await consumer(); } catch (...) { failure = std::current_exception(); }
    peer.stop();
    try {
        co_await server;
    } catch (...) {
        if (!failure) failure = std::current_exception();
    }
    co_await server.wait_destroyed_async();
    if (failure) std::rethrow_exception(failure);
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

TEST_CASE("source: registry streaming consumes fragmented range framing", "[source][registry-streaming]") {
    const bool chunked = GENERATE(false, true);
    test::run_coro([chunked]() -> elio::coro::task<void> {
        const auto bytes = test::pattern_bytes(65539, 74);
        const std::string payload(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        RawRegistry peer([&](const std::string&) {
            std::string wire = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 16-65554/1048576\r\n";
            if (chunked) {
                wire += "Transfer-Encoding: chunked\r\n\r\n";
                for (size_t pos = 0; pos < payload.size(); pos += 7003) {
                    const auto chunk = std::string_view(payload).substr(pos, 7003);
                    char length[32];
                    std::snprintf(length, sizeof(length), "%zx", chunk.size());
                    wire += std::string(length) + "\r\n" + std::string(chunk) + "\r\n";
                }
                wire += "0\r\nX-End: complete\r\n\r\n";
            } else {
                wire += "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n" + payload;
            }
            return RawRegistry::Reply{std::move(wire), 4093};
        });
        co_await with_raw_registry(peer, [&]() -> elio::coro::task<void> {
            source::RegistryClient client(nullptr, {});
            std::vector<uint8_t> guarded(bytes.size() + 2, 0xd5);
            for (int i = 0; i < 2; ++i) {
                const auto read = co_await client.get_data(peer.url(), guarded.data() + 1, 16, bytes.size());
                REQUIRE(read == static_cast<ssize_t>(bytes.size()));
                REQUIRE(std::equal(bytes.begin(), bytes.end(), guarded.begin() + 1));
                REQUIRE(guarded.front() == 0xd5);
                REQUIRE(guarded.back() == 0xd5);
            }
            REQUIRE(peer.connections() == 2);  // retired probe + reused payload connection
            REQUIRE(peer.data_requests() == 2);
        });
    });
}

TEST_CASE("source: registry streaming drains ignored range before connection reuse", "[source][registry-streaming]") {
    test::run_coro([]() -> elio::coro::task<void> {
        const std::string payload(65539, 'r');
        RawRegistry peer([&](const std::string&) {
            return RawRegistry::Reply{"HTTP/1.1 200 OK\r\nContent-Length: " +
                std::to_string(payload.size()) + "\r\n\r\n" + payload, 4093};
        });
        co_await with_raw_registry(peer, [&]() -> elio::coro::task<void> {
            source::RegistryClient client(nullptr, {});
            std::array<char, 128> output;
            for (int i = 0; i < 2; ++i) {
                output.fill('x');
                const auto read = co_await client.get_data(peer.url(), output.data(), 0, output.size());
                REQUIRE(read == static_cast<ssize_t>(output.size()));
                REQUIRE(std::all_of(output.begin(), output.end(), [](char c) { return c == 'r'; }));
            }
            REQUIRE(peer.connections() == 2);  // discovery retires only its own connection
        });
    });
}

TEST_CASE("source: registry streaming detects short bodies and missing final framing", "[source][registry-streaming]") {
    const int failure = GENERATE(0, 1, 2);
    test::run_coro([failure]() -> elio::coro::task<void> {
        RawRegistry peer([failure](const std::string& request) {
            const bool healthy = request.starts_with("GET /healthy ");
            const std::string head = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 16-23/1048576\r\n";
            if (healthy) return RawRegistry::Reply{head + "Content-Length: 8\r\n\r\nhealthy!"};
            if (failure == 0) return RawRegistry::Reply{head + "Content-Length: 3\r\n\r\nabc"};
            if (failure == 1) return RawRegistry::Reply{head + "Content-Length: 8\r\n\r\nabc", 4096, true};
            // All requested bytes arrive, but the required zero chunk/trailer
            // never does: copying eight bytes cannot establish success.
            return RawRegistry::Reply{head + "Transfer-Encoding: chunked\r\n\r\n8\r\nabcdefgh\r\n", 4096, true};
        });
        co_await with_raw_registry(peer, [&]() -> elio::coro::task<void> {
            source::RegistryClient client(nullptr, {});
            std::array<char, 8> output;
            const auto bad = co_await client.get_data(peer.url("/bad"), output.data(), 16, output.size());
            REQUIRE(bad == (failure == 0 ? -EIO : -EBADMSG));
            REQUIRE(peer.data_requests() == (failure == 0 ? 1 : 3));
            const auto good = co_await client.get_data(peer.url("/healthy"), output.data(), 16, output.size());
            REQUIRE(good == 8);
            REQUIRE(std::string(output.data(), output.size()) == "healthy!");
            REQUIRE(peer.connections() == (failure == 0 ? 3 : 6));
        });
    });
}

TEST_CASE("source: registry streaming keeps decoded body size bounded", "[source][registry-streaming]") {
    const bool chunked = GENERATE(false, true);
    const bool excessive = GENERATE(false, true);
    test::run_coro([chunked, excessive]() -> elio::coro::task<void> {
        RawRegistry peer([chunked, excessive](const std::string&) {
            const std::string head = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 16-23/1048576\r\n";
            if (chunked) return RawRegistry::Reply{head + "Transfer-Encoding: chunked\r\n\r\n" +
                (excessive ? "9\r\nabcdefghi\r\n" : "8\r\nabcdefgh\r\n") + "0\r\n\r\n"};
            return RawRegistry::Reply{head + (excessive ? "Content-Length: 9\r\n\r\nabcdefghi" :
                "Content-Length: 8\r\n\r\nabcdefgh")};
        });
        co_await with_raw_registry(peer, [&]() -> elio::coro::task<void> {
            source::RegistryClientConfig config;
            config.max_response_size = 8;
            source::RegistryClient client(nullptr, config);
            std::array<char, 10> output;
            output.fill('x');
            const auto read = co_await client.get_data(peer.url(), output.data() + 1, 16, 8);
            REQUIRE(read == (excessive ? -EMSGSIZE : 8));
            REQUIRE(output.front() == 'x');
            REQUIRE(output.back() == 'x');
            if (!excessive) REQUIRE(std::string(output.data() + 1, 8) == "abcdefgh");
            REQUIRE(peer.data_requests() == (excessive ? 3 : 1));
        });
    });
}

TEST_CASE("source: registry streaming rejects status before unread error body", "[source][registry-streaming]") {
    const int status = GENERATE(200, 416, 429);
    test::run_coro([status]() -> elio::coro::task<void> {
        RawRegistry peer([status](const std::string& request) {
            if (request.starts_with("GET /healthy ")) return RawRegistry::Reply{
                "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 16-23/1048576\r\n"
                "Content-Length: 8\r\n\r\nhealthy!"};
            // No body follows these headers. A buffered implementation would
            // wait for its read timeout rather than return the status result.
            return RawRegistry::Reply{"HTTP/1.1 " + std::to_string(status) +
                " Rejected\r\nContent-Length: 1048576\r\n\r\n"};
        });
        co_await with_raw_registry(peer, [&]() -> elio::coro::task<void> {
            source::RegistryClientConfig config;
            config.read_timeout = std::chrono::seconds(5);
            source::RegistryClient client(nullptr, config);
            std::array<char, 8> output;
            output.fill('x');
            const auto start = std::chrono::steady_clock::now();
            const auto bad = co_await client.get_data(peer.url("/bad"), output.data(), 16, output.size());
            REQUIRE(bad == (status == 416 ? -ERANGE : status == 429 ? -EBUSY : -EIO));
            REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
            REQUIRE(std::all_of(output.begin(), output.end(), [](char c) { return c == 'x'; }));
            const auto good = co_await client.get_data(peer.url("/healthy"), output.data(), 16, output.size());
            REQUIRE(good == 8);
            REQUIRE(std::string(output.data(), output.size()) == "healthy!");
            REQUIRE(peer.connections() == (status == 416 ? 4 : 6));
        });
    });
}

TEST_CASE("source: registry cancellation retires a blocked payload buffer before return", "[source][registry-streaming]") {
    elio::runtime::run_config config;
    config.num_threads = 1;  // observe buffer progress without cross-worker data races
    elio::run([]() -> elio::coro::task<void> {
        elio::sync::event late_release;
        RawRegistry peer([&](const std::string& request) {
            const std::string head = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 16-23/1048576\r\n"
                "Content-Length: 8\r\n\r\n";
            if (request.starts_with("GET /healthy ")) return RawRegistry::Reply{head + "healthy!"};
            return RawRegistry::Reply{head + "abcd", 4096, true, &late_release, "efgh"};
        });
        co_await with_raw_registry(peer, [&]() -> elio::coro::task<void> {
            source::RegistryClientConfig client_config;
            client_config.read_timeout = std::chrono::seconds(5);
            source::RegistryClient client(nullptr, client_config);
            const std::string url = peer.url("/cancel");
            std::array<char, 8> loan;
            loan.fill('x');
            auto reading = elio::spawn(client.get_data(url, loan.data(), 16, loan.size()));
            std::exception_ptr failure;
            try {
                const auto progress_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (std::string_view(loan.data(), 4) != "abcd" &&
                       !reading.is_ready() && std::chrono::steady_clock::now() < progress_deadline) {
                    co_await elio::time::sleep_for(std::chrono::milliseconds(1));
                }
                REQUIRE(std::string_view(loan.data(), 4) == "abcd");
                REQUIRE_FALSE(reading.is_ready());
                const auto cancelled_at = std::chrono::steady_clock::now();
                reading.request_cancel();
                const auto cancelled = co_await reading;
                REQUIRE(cancelled == -ECANCELED);
                REQUIRE(std::chrono::steady_clock::now() - cancelled_at < std::chrono::seconds(2));
                REQUIRE(peer.data_requests() == 1);  // cancellation never retries the request

                loan.fill('r');  // caller reuses storage immediately after awaited return
                late_release.set();  // now the old peer attempts its delayed payload suffix
                std::array<char, 8> healthy;
                const auto next = co_await client.get_data(peer.url("/healthy"), healthy.data(), 16, healthy.size());
                REQUIRE(next == 8);
                REQUIRE(std::string(healthy.data(), healthy.size()) == "healthy!");
                REQUIRE(std::all_of(loan.begin(), loan.end(), [](char c) { return c == 'r'; }));
                REQUIRE(peer.connections() == 4);  // probes + retired cancelled exchange + healthy payload
                REQUIRE(peer.data_requests() == 2);
            } catch (...) {
                failure = std::current_exception();
            }
            // Preserve the borrowed buffer even if a test assertion fails.
            reading.request_cancel();
            late_release.set();
            co_await reading.wait_destroyed_async();
            if (failure) std::rethrow_exception(failure);
        });
    }, config);
}

TEST_CASE("source: cancelled registry auth leader preserves the shared token exchange", "[source][registry-streaming]") {
    test::run_coro([]() -> elio::coro::task<void> {
        auto blob = test::pattern_bytes(4096, 91);
        MockRegistry mock(blob);
        mock.token_delay_ms_ = 80;
        elio::go([&mock]() -> elio::coro::task<void> { co_await mock.run(); });
        MockGuard guard{mock};
        const bool running = co_await test::wait_server_running(mock);
        REQUIRE(running);
        source::RegistryClient client(test_creds(), {});
        const std::string url = mock.repo_base() + "/auth/x";
        std::array<uint8_t, 128> leader_buffer;
        std::array<uint8_t, 128> follower_buffer;
        auto leader = elio::spawn(client.get_data(url, leader_buffer.data(), 16, leader_buffer.size()));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (mock.token_hits_.load() == 0 && !leader.is_ready() &&
               std::chrono::steady_clock::now() < deadline) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        const bool exchange_started = mock.token_hits_.load() == 1;
        auto follower = elio::spawn(client.get_data(url, follower_buffer.data(), 16, follower_buffer.size()));
        leader.request_cancel();
        const auto leader_result = co_await leader;
        const auto follower_result = co_await follower;
        co_await leader.wait_destroyed_async();
        co_await follower.wait_destroyed_async();
        REQUIRE(exchange_started);
        REQUIRE(leader_result == -ECANCELED);
        REQUIRE(follower_result == static_cast<ssize_t>(follower_buffer.size()));
        REQUIRE(std::equal(follower_buffer.begin(), follower_buffer.end(), blob.begin() + 16));
        REQUIRE(mock.token_hits_.load() == 1);
        co_await wait_drained(mock);
    });
}

TEST_CASE("source: registry discovery and range length ignore held probe bodies", "[source][registry-streaming]") {
    const bool unrepresentable = GENERATE(false, true);
    test::run_coro([unrepresentable]() -> elio::coro::task<void> {
        RawRegistry peer([unrepresentable](const std::string& request) {
            if (request.find("Range: bytes=0-0\r\n") != std::string::npos) {
                // The advertised body is larger than the configured cap and
                // never arrives. Only status/headers are relevant to discovery
                // and to a 206 size total; both retire this connection promptly.
                const std::string total = unrepresentable ? "9223372036854775808" : "1048576";
                return RawRegistry::Reply{
                    "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-0/" + total +
                    "\r\nContent-Length: 1048576\r\n\r\n"};
            }
            return RawRegistry::Reply{
                "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 16-23/1048576\r\n"
                "Content-Length: 8\r\n\r\nhealthy!"};
        }, false);
        co_await with_raw_registry(peer, [&]() -> elio::coro::task<void> {
            source::RegistryClientConfig config;
            config.max_response_size = 8;
            config.read_timeout = std::chrono::seconds(5);
            source::RegistryClient client(nullptr, config);
            const auto start = std::chrono::steady_clock::now();
            const auto size = co_await client.get_length(peer.url());
            REQUIRE(size == (unrepresentable ? -EOVERFLOW : 1048576));
            REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
            std::array<char, 8> output;
            const auto read = co_await client.get_data(peer.url(), output.data(), 16, output.size());
            REQUIRE(read == 8);
            REQUIRE(std::string(output.data(), output.size()) == "healthy!");
            REQUIRE(peer.requests() == 3);
            REQUIRE(peer.connections() == 3);
        });
    });
}

TEST_CASE("source: registry ignored range length counts decoded bytes within its cap", "[source][registry-streaming]") {
    const int framing = GENERATE(0, 1, 2, 3);
    const bool excessive = GENERATE(false, true);
    test::run_coro([framing, excessive]() -> elio::coro::task<void> {
        constexpr size_t cap = 32768;
        const std::string payload(cap + (excessive ? 1 : 0), 's');
        size_t requests = 0;
        RawRegistry peer([&](const std::string&) {
            ++requests;
            std::string head = "HTTP/1.1 200 OK\r\n";
            if (framing == 0) head += "Content-Length: " + std::to_string(payload.size()) + "\r\n";
            else if (framing == 2) head += "Connection: close\r\n";
            else head += "Transfer-Encoding: chunked\r\n";
            head += "\r\n";
            if (requests == 1) return RawRegistry::Reply{std::move(head)};  // irrelevant discovery body held
            std::string wire = std::move(head);
            if (framing == 1 || framing == 3) {
                char size[32];
                std::snprintf(size, sizeof(size), "%zx", payload.size());
                wire += std::string(size) + "\r\n" + payload + "\r\n";
                if (framing == 1) wire += "0\r\nX-End: complete\r\n\r\n";
            } else wire += payload;
            return RawRegistry::Reply{std::move(wire), 4093, framing == 2 || framing == 3};
        }, false);
        co_await with_raw_registry(peer, [&]() -> elio::coro::task<void> {
            source::RegistryClientConfig config;
            config.max_response_size = cap;
            source::RegistryClient client(nullptr, config);
            const auto size = co_await client.get_length(peer.url());
            REQUIRE(size == (excessive ? -EMSGSIZE : framing == 3 ? -EBADMSG : static_cast<int64_t>(cap)));
            REQUIRE(peer.requests() == 2);
            REQUIRE(peer.connections() == 2);
        });
    });
}

TEST_CASE("source: registry request guards and owned errors ignore ambient errno", "[source][registry-streaming]") {
    test::run_coro([]() -> elio::coro::task<void> {
        source::RegistryClient client(nullptr, {});
        std::array<char, 8> output;
        errno = EBUSY;
        const auto invalid = co_await client.get_data("not a URL", output.data(), 0, output.size());
        REQUIRE(invalid == -EINVAL);
        errno = EBUSY;
        const auto length = co_await client.get_length("not a URL");
        REQUIRE(length == -EINVAL);
        const auto empty = co_await client.get_data("not a URL", nullptr, 0, 0);
        REQUIRE(empty == 0);
        const auto missing = co_await client.get_data("not a URL", nullptr, 0, 1);
        REQUIRE(missing == -EINVAL);
        const auto overflow = co_await client.get_data("not a URL", output.data(), UINT64_MAX, 2);
        REQUIRE(overflow == -EOVERFLOW);
        const auto too_large = co_await client.get_data("not a URL", output.data(), 0, 64 * 1024 * 1024 + 1);
        REQUIRE(too_large == -EMSGSIZE);
    });
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
