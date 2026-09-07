// Unit tests: registry client against a mock HTTP registry (Elio server).
// Covers Range reads, size probes, bearer-token auth, redirect caching and
// the DART prefix passthrough (ADR-0005).
#include "source/registry.hpp"

#include "../support.hpp"

#include <elio/http/http_server.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/time/timer.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace obd;
namespace http = elio::http;

namespace {



/// A mock OCI registry speaking the subset our client needs:
///   /v2/blobs/<x>     — plain Range-capable blob
///   /v2/auth/<x>      — 401 + Bearer challenge, then token-gated
///   /token            — issues {"token":"sekrit"} for Basic u:p
///   /v2/redir/<x>     — 302 to /cdn/blob (anonymous)
///   /cdn/blob         — Range-capable, no auth
///   /dart/<upstream>  — DART-style prefix passthrough echo endpoint
class MockRegistry {
public:
    MockRegistry(std::vector<uint8_t> blob, uint16_t port)
        : blob_(std::move(blob)), port_(port) {
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
        co_await server_->listen(
            elio::net::socket_address(
                elio::net::ipv4_address("127.0.0.1", port_)));
    }
    void stop() { server_->stop(); }
    bool drained() const { return server_->active_connections() == 0; }

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
            if (auth != "Bearer sekrit") {
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
        co_return http::response(http::status::ok,
                                 R"({"token":"sekrit"})",
                                 "application/json");
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
    uint16_t port_;
    std::unique_ptr<http::server> server_;
public:
    std::string last_dart_path_;
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

source::CredentialStorePtr test_creds() {
    auto store = std::make_shared<source::CredentialStore>();
    store->add("127.0.0.1", {"u", "p"});
    return store;
}

}  // namespace

TEST_CASE("source: registry range reads and size probe", "[source]") {
    const int rc = test::run_coro([]() -> elio::coro::task<int> {
        auto blob = test::pattern_bytes(300 * 1024, 21);
        MockRegistry mock(blob, 19191);
        elio::go([&mock]() -> elio::coro::task<void> {
            co_await mock.run();
        });
        MockGuard guard{mock};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

        auto client =
            std::make_shared<source::RegistryClient>(nullptr,
                                                     source::RegistryClientConfig{});
        const std::string url = "http://127.0.0.1:19191/v2/blobs/x";
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
        MockRegistry mock(blob, 19192);
        elio::go([&mock]() -> elio::coro::task<void> {
            co_await mock.run();
        });
        MockGuard guard{mock};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

        auto client = std::make_shared<source::RegistryClient>(
            test_creds(), source::RegistryClientConfig{});
        const std::string url = "http://127.0.0.1:19192/v2/auth/x";
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
        MockRegistry mock(blob, 19193);
        elio::go([&mock]() -> elio::coro::task<void> {
            co_await mock.run();
        });
        MockGuard guard{mock};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

        const std::string url = "http://127.0.0.1:19193/v2/redir/x";
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
        MockRegistry mock(blob, 19194);
        elio::go([&mock]() -> elio::coro::task<void> {
            co_await mock.run();
        });
        MockGuard guard{mock};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

        source::RegistryClientConfig cfg;
        cfg.accelerate_base = "http://127.0.0.1:19194/dart";
        auto client =
            std::make_shared<source::RegistryClient>(nullptr, cfg);
        const std::string url = "http://127.0.0.1:19194/v2/blobs/x";
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
