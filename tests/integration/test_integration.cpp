// Integration tests: the full read pipeline (registry mock -> layer store
// -> tar adapter -> zfile -> lsmt merge), image assembly via open_image
// (remote layers served through the LayerStore, ADR-0011), DART
// optional-accelerator fallback, and background fill through image
// assembly. No kernel dependencies (ublk E2E lives in test_ublk_e2e.cpp
// and self-skips). See docs/testing.md.
#include "common/errors.hpp"
#include "common/bytes.hpp"
#include "common/sha256.hpp"
#include "format/lsmt.hpp"
#include "format/trace.hpp"
#include "format/writer.hpp"
#include "format/zfile.hpp"
#include "image/image_file.hpp"
#include "source/dart.hpp"
#include "source/layer_store.hpp"
#include "source/registry.hpp"
#include "source/tar_offset.hpp"

#include "../support.hpp"

#include <elio/http/http_server.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/sync/mutex.hpp>
#include <elio/time/timer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

using namespace obd;
using obd::test::TempDir;
namespace http = elio::http;

namespace {

/// Minimal Range-capable blob server: every GET under /v2/* serves the one
/// hosted blob. The listener port is OS-assigned (ephemeral, issue #15):
/// never a fixed port, so concurrent suite runs on one machine cannot
/// collide.
class BlobServer {
public:
    explicit BlobServer(std::vector<uint8_t> blob)
        : blob_(std::move(blob)),
          port_lease_(), port_(port_lease_.port()) {
        http::router r;
        r.add_route(http::method::GET, "/v2/*",
                    [this](http::context& ctx) { return handler(ctx); });
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
                co_await server_->listen(elio::net::socket_address(
                    elio::net::ipv4_address("127.0.0.1", port_)));
                const int listen_errno = errno;
                if (!stop_requested_.load(std::memory_order_acquire) &&
                    !server_->is_running()) {
                    test::require_retryable_http_listen_return(
                        "BlobServer listen", listen_errno);
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
    /// The OS-assigned loopback port this server listens on.
    uint16_t port() const noexcept { return port_; }
    /// GETs serving more than the 1-byte size probe (Range bytes=0-0):
    /// the observable "went to the remote" counter for cache assertions.
    uint64_t data_gets() const {
        return data_gets_.load(std::memory_order_relaxed);
    }
    /// Distinct 64 KiB extents (the LayerStore granularity) ever served —
    /// each LayerStore remote fetch is exactly one extent.
    size_t served_extent_count() const {
        std::lock_guard lk(extents_mu_);
        return served_extents_.size();
    }
    std::string repo_base() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v2";
    }
    std::string url(const std::string& name) const {
        return repo_base() + "/" + name;
    }

private:
    elio::coro::task<http::response> handler(http::context& ctx) {
        const std::string_view range = ctx.req().header("Range");
        uint64_t first = 0, last = blob_.size() - 1;
        bool partial = false;
        if (range.starts_with("bytes=")) {
            const auto dash = range.find('-', 6);
            first = std::stoull(std::string(range.substr(6, dash - 6)));
            last = std::min<uint64_t>(
                std::stoull(std::string(range.substr(dash + 1))),
                blob_.size() - 1);
            partial = true;
        }
        if (first >= blob_.size() || first > last) {
            http::response resp(http::status::range_not_satisfiable);
            resp.set_header("Content-Length", "0");
            co_return resp;
        }
        if (last > first) {  // more than the 1-byte size probe
            data_gets_.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lk(extents_mu_);
            // Record every covered extent: bulk paths (background fill)
            // read several extents in one coalesced range read.
            for (uint64_t e = first / (64 * 1024);
                 e <= last / (64 * 1024); ++e) {
                served_extents_.insert(e);
            }
        }
        http::response resp(
            partial ? http::status::partial_content : http::status::ok,
            std::string_view(
                reinterpret_cast<const char*>(blob_.data() + first),
                last - first + 1));
        if (partial) {
            resp.set_header("Content-Range",
                            "bytes " + std::to_string(first) + "-" +
                                std::to_string(last) + "/" +
                                std::to_string(blob_.size()));
        }
        co_return resp;
    }
    std::vector<uint8_t> blob_;
    test::ReservedTcpPort port_lease_;
    uint16_t port_;
    std::unique_ptr<http::server> server_;
    std::function<void(uint16_t)> before_listen_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<uint64_t> data_gets_{0};
    mutable std::mutex extents_mu_;
    std::set<uint64_t> served_extents_;
};

/// RAII stop: an exception mid-test must not leave the accept loop pending
/// (a pending detached task hangs scheduler shutdown). Works with any
/// server exposing stop().
struct BlobGuard {
    template <typename Server>
    explicit BlobGuard(Server& server) : stop_([&server] { server.stop(); }) {}
    ~BlobGuard() { stop_(); }
    std::function<void()> stop_;
};

/// A loopback address on which NOTHING listens for the whole lifetime of
/// this object: the probe socket stays bound (never listening), so no
/// other process can bind the port and every connect() to it is answered
/// with ECONNREFUSED. This is the "unreachable proxy" fixture for the DART
/// fallback tests — a fixed port (previously 19999) was itself a
/// cross-machine collision source, since any unrelated process could be
/// legitimately listening there.
class UnusedPort {
public:
    UnusedPort() {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            throw std::system_error(errno, std::system_category(), "socket");
        }
        sockaddr_in sa {};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) !=
            0) {
            const int e = errno;
            ::close(fd_);
            fd_ = -1;
            throw std::system_error(e, std::system_category(), "bind");
        }
        socklen_t len = sizeof(sa);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&sa), &len) != 0) {
            const int e = errno;
            ::close(fd_);
            fd_ = -1;
            throw std::system_error(e, std::system_category(), "getsockname");
        }
        port_ = ntohs(sa.sin_port);
    }
    ~UnusedPort() {
        if (fd_ >= 0) ::close(fd_);
    }
    UnusedPort(const UnusedPort&) = delete;
    UnusedPort& operator=(const UnusedPort&) = delete;
    uint16_t port() const noexcept { return port_; }
    /// "host:port" form for the DART address ("...:port/dart").
    std::string address() const {
        return "127.0.0.1:" + std::to_string(port_);
    }

private:
    int fd_ = -1;
    uint16_t port_ = 0;
};

std::string sha256_hex_of(const std::vector<uint8_t>& data) {
    common::Sha256 h;
    h.update(data.data(), data.size());
    return h.final_hex();
}

/// Builds raw -> LSMT -> ZFile in `dir` and returns the zfile blob bytes.
std::vector<uint8_t> make_zfile_blob(TempDir& dir,
                                     const std::vector<uint8_t>& raw) {
    const std::string rawp = test::write_file(dir / "raw.img", raw);
    const std::string lsmts = dir / "layer.lsmt";
    const std::string zfiles = dir / "layer.zfile";
    const int fd = ::open(rawp.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    format::write_lsmt_single_layer(fd, raw.size(), lsmts, {});
    ::close(fd);
    const int fd2 = ::open(lsmts.c_str(), O_RDONLY);
    REQUIRE(fd2 >= 0);
    struct stat st {};
    REQUIRE(::fstat(fd2, &st) == 0);
    format::write_zfile(fd2, static_cast<uint64_t>(st.st_size), zfiles, {});
    ::close(fd2);
    std::vector<uint8_t> blob;
    const int fd3 = ::open(zfiles.c_str(), O_RDONLY);
    REQUIRE(fd3 >= 0);
    REQUIRE(::fstat(fd3, &st) == 0);
    blob.resize(static_cast<size_t>(st.st_size));
    REQUIRE(::read(fd3, blob.data(), blob.size()) ==
            static_cast<ssize_t>(blob.size()));
    ::close(fd3);
    return blob;
}

/// overlaybd semantics for a remote image: blobs are addressed as
/// repoBlobUrl + "/" + digest; lower.file is a LOCAL file only.
nlohmann::json remote_image_config(const std::string& repo_base,
                                   const std::string& digest_hex,
                                   uint64_t size) {
    nlohmann::json cfgj;
    cfgj["repoBlobUrl"] = repo_base;
    cfgj["lowers"] = nlohmann::json::array({nlohmann::json{
        {"digest", "sha256:" + digest_hex}, {"size", size}}});
    return cfgj;
}

/// Same as remote_image_config plus the per-layer directory the LayerStore
/// persists into.
nlohmann::json remote_image_config_with_dir(const std::string& repo_base,
                                            const std::string& digest_hex,
                                            uint64_t size,
                                            const std::string& layer_dir) {
    nlohmann::json cfgj = remote_image_config(repo_base, digest_hex, size);
    cfgj["lowers"][0]["dir"] = layer_dir;
    return cfgj;
}

/// Present-extent count in the sidecar of the (single) LayerStore staging
/// pair in `layer_dir`; -1 while no sidecar exists yet. The sidecar layout
/// is documented in docs/source.md: an 80-byte header, then 8-byte
/// {crc32, flags} records with the present bit in the flags byte.
long sidecar_present_count(const std::string& layer_dir) {
    for (const auto& entry :
         std::filesystem::directory_iterator(layer_dir)) {
        const std::string name = entry.path().filename().string();
        if (!name.starts_with(".bitmap.")) continue;
        const int fd = ::open(entry.path().c_str(), O_RDONLY);
        if (fd < 0) return -1;
        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            return -1;
        }
        long present = 0;
        uint8_t rec[8];
        for (off_t off = 80; off + 8 <= st.st_size; off += 8) {
            if (::pread(fd, rec, sizeof rec, off) !=
                static_cast<ssize_t>(sizeof rec)) {
                break;
            }
            if (rec[4] & 1) ++present;  // flags u32 LE, bit 0 = present
        }
        ::close(fd);
        return present;
    }
    return -1;
}

std::vector<std::string> names_with_prefix(const std::string& dir,
                                           const std::string& prefix) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.compare(0, prefix.size(), prefix) == 0) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}


/// Range-capable server hosting MULTIPLE blobs, addressed by the URL path
/// component after /v2/ (the layer digest). Tracks per-blob data GETs and
/// served 64 KiB extents so warm-up traffic can be attributed to the
/// right layer (ADR-0013 trace replay test). Optional per-data-GET
/// latency injection (set_latency) simulates a slow source for the
/// ADR-0012 admission-funnel tests; set_serialized additionally funnels
/// all data service through one coroutine mutex, simulating a source
/// with capacity 1 (queued requests wait one service time each).
class BlobMapServer {
public:
    explicit BlobMapServer(std::map<std::string, std::vector<uint8_t>> blobs)
        : blobs_(std::move(blobs)),
          port_lease_(), port_(port_lease_.port()) {
        for (const auto& [name, blob] : blobs_) {
            stats_[name] = std::make_unique<Stats>();
        }
        http::router r;
        r.add_route(http::method::GET, "/v2/*",
                    [this](http::context& ctx) { return handler(ctx); });
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
                co_await server_->listen(elio::net::socket_address(
                    elio::net::ipv4_address("127.0.0.1", port_)));
                const int listen_errno = errno;
                if (!stop_requested_.load(std::memory_order_acquire) &&
                    !server_->is_running()) {
                    test::require_retryable_http_listen_return(
                        "BlobMapServer listen", listen_errno);
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
    /// The OS-assigned loopback port this server listens on.
    uint16_t port() const noexcept { return port_; }
    std::string repo_base() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v2";
    }
    uint64_t data_gets(const std::string& name) const {
        return stats_.at(name)->data_gets.load(std::memory_order_relaxed);
    }
    bool served_extent(const std::string& name, uint64_t extent) const {
        const auto* st = stats_.at(name).get();
        std::lock_guard lk(st->extents_mu);
        return st->extents.count(extent) != 0;
    }
    /// Ordered data GETs across ALL blobs as (blob name, range first
    /// byte) — pins cross-blob request ORDER (the
    /// warm-up-before-trace-load test).
    std::vector<std::pair<std::string, uint64_t>> data_get_log() const {
        std::lock_guard lk(log_mu_);
        return log_;
    }
    /// Per-data-GET service latency (0 = none). Size probes stay fast.
    void set_latency(std::chrono::milliseconds d) {
        latency_ms_.store(d.count(), std::memory_order_relaxed);
    }
    /// When true, all data service is serialized through one coroutine
    /// mutex: a queued request waits behind one full service time per
    /// ahead-of-it request (a QoS-less source with capacity 1).
    void set_serialized(bool on) {
        serialized_.store(on, std::memory_order_relaxed);
    }
    /// Test hook: block data GETs after they have been counted. Size probes
    /// stay unblocked, so image assembly can complete before a live reader
    /// is intentionally parked.
    void block_data_gets() {
        blocked_data_gets_.store(0, std::memory_order_relaxed);
        data_blocked_.store(true, std::memory_order_release);
    }
    void release_data_gets() {
        data_blocked_.store(false, std::memory_order_release);
    }
    uint64_t blocked_data_gets() const {
        return blocked_data_gets_.load(std::memory_order_acquire);
    }

private:
    struct Stats {
        std::atomic<uint64_t> data_gets{0};
        mutable std::mutex extents_mu;
        std::set<uint64_t> extents;
    };

    elio::coro::task<http::response> handler(http::context& ctx) {
        const std::string name(ctx.req().path().substr(4));  // "/v2/" + name
        const auto it = blobs_.find(name);
        if (it == blobs_.end()) {
            http::response resp(http::status::not_found);
            resp.set_header("Content-Length", "0");
            co_return resp;
        }
        const auto& blob = it->second;
        auto* st = stats_.at(name).get();
        const std::string_view range = ctx.req().header("Range");
        uint64_t first = 0, last = blob.size() - 1;
        bool partial = false;
        if (range.starts_with("bytes=")) {
            const auto dash = range.find('-', 6);
            first = std::stoull(std::string(range.substr(6, dash - 6)));
            last = std::min<uint64_t>(
                std::stoull(std::string(range.substr(dash + 1))),
                blob.size() - 1);
            partial = true;
        }
        if (first >= blob.size() || first > last) {
            http::response resp(http::status::range_not_satisfiable);
            resp.set_header("Content-Length", "0");
            co_return resp;
        }
        if (last > first) {  // more than the 1-byte size probe
            st->data_gets.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard lk(st->extents_mu);
                for (uint64_t e = first / (64 * 1024); e * 64 * 1024 <= last;
                     ++e) {
                    st->extents.insert(e);
                }
            }
            {
                std::lock_guard lk(log_mu_);
                log_.emplace_back(name, first);
            }
            if (data_blocked_.load(std::memory_order_acquire)) {
                blocked_data_gets_.fetch_add(1, std::memory_order_acq_rel);
                while (data_blocked_.load(std::memory_order_acquire)) {
                    co_await elio::time::sleep_for(
                        std::chrono::milliseconds(1));
                }
            }
            const int64_t lat = latency_ms_.load(std::memory_order_relaxed);
            if (lat > 0) {
                if (serialized_.load(std::memory_order_relaxed)) {
                    co_await service_mu_.lock();
                    co_await elio::time::sleep_for(
                        std::chrono::milliseconds(lat));
                    service_mu_.unlock();
                } else {
                    co_await elio::time::sleep_for(
                        std::chrono::milliseconds(lat));
                }
            }
        }
        http::response resp(
            partial ? http::status::partial_content : http::status::ok,
            std::string_view(
                reinterpret_cast<const char*>(blob.data() + first),
                last - first + 1));
        if (partial) {
            resp.set_header("Content-Range",
                            "bytes " + std::to_string(first) + "-" +
                                std::to_string(last) + "/" +
                                std::to_string(blob.size()));
        }
        co_return resp;
    }

    std::map<std::string, std::vector<uint8_t>> blobs_;
    test::ReservedTcpPort port_lease_;
    uint16_t port_;
    std::unique_ptr<http::server> server_;
    std::function<void(uint16_t)> before_listen_;
    std::atomic<bool> stop_requested_{false};
    std::map<std::string, std::unique_ptr<Stats>> stats_;
    mutable std::mutex log_mu_;
    std::vector<std::pair<std::string, uint64_t>> log_;
    std::atomic<int64_t> latency_ms_{0};
    std::atomic<bool> serialized_{false};
    std::atomic<bool> data_blocked_{false};
    std::atomic<uint64_t> blocked_data_gets_{0};
    elio::sync::mutex service_mu_;
};

/// Wraps a payload as a single-member ustar blob (the shape overlaybd
/// registry layers take; the acceleration layer's member is named `trace`
/// upstream — our TarOffsetSource keys on the wrapper, not the name).
std::vector<uint8_t> tar_wrap(const std::vector<uint8_t>& payload) {
    auto out = test::make_tar_header(payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    out.resize((out.size() + 511) / 512 * 512, 0);
    return out;
}

#ifdef OBD_TEST_HOOKS
class TraceBlobLoadBudgetGuard {
public:
    explicit TraceBlobLoadBudgetGuard(std::chrono::milliseconds budget)
        : old_(image::test_hooks::trace_blob_load_budget_for_test()) {
        image::test_hooks::set_trace_blob_load_budget_for_test(budget);
    }
    ~TraceBlobLoadBudgetGuard() {
        image::test_hooks::set_trace_blob_load_budget_for_test(old_);
    }

    TraceBlobLoadBudgetGuard(const TraceBlobLoadBudgetGuard&) = delete;
    TraceBlobLoadBudgetGuard& operator=(const TraceBlobLoadBudgetGuard&) =
        delete;

private:
    std::chrono::milliseconds old_;
};
#endif

}  // namespace

TEST_CASE("integration: layered stack stages over a mock registry",
          "[integration]") {
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 32, 41);
    const auto blob = make_zfile_blob(dir, raw);
    const std::string layer_dir = dir / "layer_staging";
    std::filesystem::create_directories(layer_dir);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobServer server(blob);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        const bool server_running =
            co_await test::wait_server_running(server);
        REQUIRE(server_running);
        auto client = std::make_shared<source::RegistryClient>(
            nullptr, source::RegistryClientConfig{});
        auto reg =
            co_await source::RegistrySource::open(client, server.url("b"));
        REQUIRE(reg->size() == blob.size());
        auto store = co_await source::LayerStore::open(
            std::move(reg), layer_dir, sha256_hex_of(blob));
        std::vector<uint8_t> buf(1000);
        const ssize_t got = co_await store->pread(buf.data(), buf.size(), 100);
        REQUIRE(got == 1000);
        auto untarred =
            co_await source::TarOffsetSource::open(std::move(store));
        const bool is_zfile = co_await format::is_zfile(*untarred);
        REQUIRE(is_zfile);
        auto view =
            co_await format::ZFileSource::open(std::move(untarred), true);
        auto layer = co_await format::LsmtLayer::open(std::move(view));
        REQUIRE(layer->virtual_size() == raw.size());
        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
        layers.push_back(std::move(layer));
        auto merged = co_await format::MergedLsmt::open(std::move(layers));
        std::vector<uint8_t> all(raw.size());
        const ssize_t rd = co_await merged->pread(all.data(), all.size(), 0);
        REQUIRE(rd == static_cast<ssize_t>(raw.size()));
        REQUIRE(all == raw);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: cancelled connect probe does not break later io",
          "[integration]") {
    auto blob = test::pattern_bytes(32 * 1024, 95);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobServer server(blob);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        const bool server_running =
            co_await test::wait_server_running(server);
        REQUIRE(server_running);
        // Unreachable probe (nothing listening): must time out fast. The
        // port is held by a bound-but-never-listening socket for the whole
        // test, so no unrelated process can occupy it (issue #15).
        UnusedPort dead_end;
        const auto addr =
            source::parse_dart_address(dead_end.address() + "/dart");
        REQUIRE(addr.has_value());
        const bool reachable = co_await source::dart_proxy_reachable(*addr);
        REQUIRE(!reachable);
        // Subsequent IO on this scheduler must still work.
        auto client = std::make_shared<source::RegistryClient>(
            nullptr, source::RegistryClientConfig{});
        auto src =
            co_await source::RegistrySource::open(client, server.url("b"));
        std::vector<uint8_t> buf(1024);
        const ssize_t got = co_await src->pread(buf.data(), buf.size(), 0);
        REQUIRE(got == 1024);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: enabled-but-unreachable DART falls back to direct reads",
          "[integration]") {
    // DART is an optional accelerator (overlaybd p2pConfig semantics): when
    // the proxy does not answer, the image must still load directly.
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 32, 91);
    const auto blob = make_zfile_blob(dir, raw);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobServer server(blob);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        const bool server_running =
            co_await test::wait_server_running(server);
        REQUIRE(server_running);

        const auto cfgj = remote_image_config(
            server.repo_base(), sha256_hex_of(blob), blob.size());
        const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
        // p2pConfig points at a port that nothing can listen on for this
        // whole test (bound, never listening — issue #15), so the proxy is
        // deterministically unreachable and reads must fall back direct.
        UnusedPort dead_end;
        image::GlobalConfig global;
        global.p2p_enable = true;
        global.p2p_address = dead_end.address() + "/dart";
        auto opened = co_await image::open_image(cfg, global);
        std::vector<uint8_t> buf(raw.size());
        const ssize_t r = co_await opened.root->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(raw.size()));
        REQUIRE(buf == raw);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: registry pipeline serves a zfile-compressed image",
          "[integration]") {
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 96, 51);
    const auto blob = make_zfile_blob(dir, raw);
    const std::string digest_hex = sha256_hex_of(blob);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobServer server(blob);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        const bool server_running =
            co_await test::wait_server_running(server);
        REQUIRE(server_running);

        const auto cfgj =
            remote_image_config(server.repo_base(), digest_hex, blob.size());
        const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
        const image::GlobalConfig global;
        auto opened = co_await image::open_image(cfg, global);
        REQUIRE(opened.layer_count == 1);
        REQUIRE(opened.virtual_size == raw.size());
        std::vector<uint8_t> buf(raw.size());
        const ssize_t r = co_await opened.root->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(raw.size()));
        REQUIRE(buf == raw);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: background fill completes a layer through image assembly",
          "[integration]") {
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 256, 73);
    const auto blob = make_zfile_blob(dir, raw);
    // Several 64 KiB extents, so fill has meaningful work after the reads.
    REQUIRE(blob.size() > 2 * 64 * 1024);
    const std::string digest_hex = sha256_hex_of(blob);
    const std::string layer_dir = dir / "layer_fill";
    constexpr size_t kPrefix = 8192;
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobServer server(blob);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        const bool server_running =
            co_await test::wait_server_running(server);
        REQUIRE(server_running);

        auto cfgj = remote_image_config_with_dir(server.repo_base(),
                                                 digest_hex, blob.size(),
                                                 layer_dir);
        cfgj["download"] =
            nlohmann::json{{"enable", true}, {"delay", 0}, {"delayExtra", 0}};
        const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
        const image::GlobalConfig global;

        // Run 1: reads work immediately; the background fill warms every
        // remaining extent and drives the store to overlaybd.commit.
        {
            auto opened = co_await image::open_image(cfg, global);
            std::vector<uint8_t> buf(kPrefix);
            const ssize_t r =
                co_await opened.root->pread(buf.data(), buf.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(kPrefix));
            REQUIRE(buf == std::vector<uint8_t>(raw.begin(),
                                                raw.begin() + kPrefix));
            bool committed = false;
            for (int i = 0; i < 400 && !committed; ++i) {
                committed = std::filesystem::exists(layer_dir +
                                                    "/overlaybd.commit");
                if (!committed) {
                    co_await elio::time::sleep_for(
                        std::chrono::milliseconds(25));
                }
            }
            REQUIRE(committed);
            // Fill fetched the extents the prefix read never touched.
            REQUIRE(server.served_extent_count() ==
                    (blob.size() + 64 * 1024 - 1) / (64 * 1024));
            // Park the fill before the chain is destroyed (LayerStore
            // lifetime contract).
            co_await image::park_image_fills(opened);
        }
        const uint64_t served_gets = server.data_gets();

        // Run 2: the commit binds via the local probe — zero remote data
        // reads, byte-exact content.
        {
            auto opened = co_await image::open_image(cfg, global);
            REQUIRE(opened.virtual_size == raw.size());
            std::vector<uint8_t> buf(raw.size());
            const ssize_t r = co_await opened.root->pread(buf.data(),
                                                          buf.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(raw.size()));
            REQUIRE(buf == raw);
        }
        REQUIRE(server.data_gets() == served_gets);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: open_image parks fills when later lower fails assembly",
          "[integration]") {
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 256, 113);
    const auto blob = make_zfile_blob(dir, raw);
    const std::string good_digest = "sha256:" + sha256_hex_of(blob);
    const std::string layer_dir = dir / "layer_partial_unwind";

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobMapServer server({{good_digest, blob}});
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        const bool server_running =
            co_await test::wait_server_running(server);
        REQUIRE(server_running);

        nlohmann::json cfgj;
        cfgj["repoBlobUrl"] = server.repo_base();
        cfgj["lowers"] = nlohmann::json::array({
            nlohmann::json{{"digest", good_digest},
                           {"size", blob.size()},
                           {"dir", layer_dir}},
            nlohmann::json{{"digest", "sha256:abcd"},
                           {"size", 64 * 1024},
                           {"dir", dir / "bad_lower"}},
        });
        cfgj["download"] = nlohmann::json{
            {"enable", true}, {"delay", 60}, {"delayExtra", 0}};
        const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
        image::GlobalConfig global;
        global.prefetch_enable = false;

        source::test_hooks::reset_unparked_layer_store_destructions_for_test();
        bool threw = false;
        int thrown_errno = 0;
        std::optional<image::OpenedImage> opened;
        try {
            opened.emplace(co_await image::open_image(cfg, global));
        } catch (const obd::error& e) {
            threw = true;
            thrown_errno = e.errno_value();
        }
        if (opened) co_await image::park_image_fills(*opened);

        REQUIRE(threw);
        REQUIRE(thrown_errno == EINVAL);
        REQUIRE(source::test_hooks::
                    unparked_layer_store_destructions_for_test() == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: unwritable layer dir degrades to remote-only reads",
          "[integration]") {
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 32, 83);
    const auto blob = make_zfile_blob(dir, raw);
    const std::string digest_hex = sha256_hex_of(blob);
    // A regular file where the layer dir's parent should be: the dir can
    // never be created or opened (ENOTDIR) — the image must still boot
    // (ADR-0016), served remote-only.
    test::write_file(dir / "blocker", {1, 2, 3});
    const std::string layer_dir =
        std::string(dir / "blocker") + "/layer";
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobServer server(blob);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        const bool server_running =
            co_await test::wait_server_running(server);
        REQUIRE(server_running);

        const auto cfgj = remote_image_config_with_dir(
            server.repo_base(), digest_hex, blob.size(), layer_dir);
        const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
        const image::GlobalConfig global;
        auto opened = co_await image::open_image(cfg, global);
        REQUIRE(opened.virtual_size == raw.size());
        std::vector<uint8_t> buf(raw.size());
        const ssize_t r = co_await opened.root->pread(buf.data(), buf.size(),
                                                      0);
        REQUIRE(r == static_cast<ssize_t>(raw.size()));
        REQUIRE(buf == raw);
        REQUIRE(server.data_gets() > 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
    // No persistence state leaked anywhere under the temp dir.
    bool staged = false;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(dir.str())) {
        if (entry.path().filename().string().starts_with(".download.")) {
            staged = true;
        }
    }
    REQUIRE(!staged);
}

TEST_CASE("integration: image assembly serves remote reads through the layer store",
          "[integration]") {
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 64, 57);
    const auto blob = make_zfile_blob(dir, raw);
    const std::string digest_hex = sha256_hex_of(blob);
    // Deliberately NOT pre-created: assembly creates a missing layer dir.
    const std::string layer_dir = dir / "layer_cold";
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobServer server(blob);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        const bool server_running =
            co_await test::wait_server_running(server);
        REQUIRE(server_running);

        const auto cfgj = remote_image_config_with_dir(
            server.repo_base(), digest_hex, blob.size(), layer_dir);
        const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
        const image::GlobalConfig global;
        auto opened = co_await image::open_image(cfg, global);
        REQUIRE(opened.layer_count == 1);
        REQUIRE(opened.virtual_size == raw.size());
        std::vector<uint8_t> buf(raw.size());
        const ssize_t r = co_await opened.root->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(raw.size()));
        REQUIRE(buf == raw);
        REQUIRE(server.data_gets() > 0);
        // Read-through persistence: the layer dir holds a staging pair (or,
        // for a fully fetched small blob, already the commit file).
        bool persisted = false;
        for (const auto& entry :
             std::filesystem::directory_iterator(layer_dir)) {
            const std::string name = entry.path().filename().string();
            if (name.starts_with(".download.") ||
                name == "overlaybd.commit") {
                persisted = true;
            }
        }
        REQUIRE(persisted);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: layer store restart serves warmed extents without remote reads",
          "[integration]") {
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 256, 63);
    const auto blob = make_zfile_blob(dir, raw);
    // Several 64 KiB extents, so a prefix read warms only part of the blob.
    REQUIRE(blob.size() > 2 * 64 * 1024);
    const std::string digest_hex = sha256_hex_of(blob);
    const std::string layer_dir = dir / "layer_warm";
    std::filesystem::create_directories(layer_dir);
    constexpr size_t kPrefix = 8192;
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobServer server(blob);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        const bool server_running =
            co_await test::wait_server_running(server);
        REQUIRE(server_running);

        const auto cfgj = remote_image_config_with_dir(
            server.repo_base(), digest_hex, blob.size(), layer_dir);
        const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
        // Structural warm-up windows at 0 (ADR-0012): this test pins
        // read-through persistence accounting across a restart — the
        // default 1 MiB head/tail windows would warm this small blob
        // whole at the first open and drive the store to completion.
        image::GlobalConfig global;
        global.prefetch_head_kb = 0;
        global.prefetch_tail_kb = 0;

        // Run 1 (cold): open and read only a prefix of the image, then wait
        // until every remotely-served extent has been persisted — teardown
        // drops queued writes, so the store must drain before it closes.
        {
            auto opened = co_await image::open_image(cfg, global);
            std::vector<uint8_t> buf(kPrefix);
            const ssize_t r =
                co_await opened.root->pread(buf.data(), buf.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(kPrefix));
            REQUIRE(buf == std::vector<uint8_t>(raw.begin(),
                                                raw.begin() + kPrefix));
            const size_t served = server.served_extent_count();
            bool drained = false;
            for (int i = 0; i < 400 && !drained; ++i) {
                drained = sidecar_present_count(layer_dir) ==
                          static_cast<long>(served);
                if (!drained) {
                    co_await elio::time::sleep_for(
                        std::chrono::milliseconds(25));
                }
            }
            REQUIRE(drained);
        }
        // Genuinely partial: no completion, no commit file.
        REQUIRE(server.served_extent_count() <
                (blob.size() + 64 * 1024 - 1) / (64 * 1024));
        REQUIRE(!std::filesystem::exists(layer_dir + "/overlaybd.commit"));
        const uint64_t served_gets = server.data_gets();

        // Run 2 (restart): the same reads come from the resumed staging
        // pair — the mock observes no further remote data reads.
        {
            auto opened = co_await image::open_image(cfg, global);
            REQUIRE(opened.virtual_size == raw.size());
            std::vector<uint8_t> buf(kPrefix);
            const ssize_t r =
                co_await opened.root->pread(buf.data(), buf.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(kPrefix));
            REQUIRE(buf == std::vector<uint8_t>(raw.begin(),
                                                raw.begin() + kPrefix));
        }
        REQUIRE(server.data_gets() == served_gets);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: completed layer store commit binds read-only without remote reads",
          "[integration]") {
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 32, 67);
    const auto blob = make_zfile_blob(dir, raw);
    // One extent: a single remote fetch fills the whole store and drives
    // it to the sha256-verified overlaybd.commit rename.
    REQUIRE(blob.size() < 64 * 1024);
    const std::string digest_hex = sha256_hex_of(blob);
    const std::string layer_dir = dir / "layer_commit";
    std::filesystem::create_directories(layer_dir);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobServer server(blob);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        const bool server_running =
            co_await test::wait_server_running(server);
        REQUIRE(server_running);

        const auto cfgj = remote_image_config_with_dir(
            server.repo_base(), digest_hex, blob.size(), layer_dir);
        const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
        const image::GlobalConfig global;

        // Run 1: read the whole image; the store completes and renames its
        // staging file to overlaybd.commit while it is still alive.
        {
            auto opened = co_await image::open_image(cfg, global);
            std::vector<uint8_t> buf(raw.size());
            const ssize_t r =
                co_await opened.root->pread(buf.data(), buf.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(raw.size()));
            REQUIRE(buf == raw);
            bool committed = false;
            for (int i = 0; i < 400 && !committed; ++i) {
                committed = std::filesystem::exists(layer_dir +
                                                    "/overlaybd.commit");
                if (!committed) {
                    co_await elio::time::sleep_for(
                        std::chrono::milliseconds(25));
                }
            }
            REQUIRE(committed);
        }
        server.stop();
        co_await elio::time::sleep_for(std::chrono::milliseconds(25));
        test::write_file(layer_dir + "/.download.deadbeefdeadbeef",
                         std::vector<uint8_t>(1, 0));
        test::write_file(layer_dir + "/.bitmap.deadbeefdeadbeef",
                         std::vector<uint8_t>(80, 0));
        REQUIRE(names_with_prefix(layer_dir, ".download.").size() == 1);
        REQUIRE(names_with_prefix(layer_dir, ".bitmap.").size() == 1);
        const uint64_t served_gets = server.data_gets();

        // Run 2: the commit marker binds the layer locally (the local
        // probe) — zero remote data reads, byte-exact content, and the
        // stale staging pair beside overlaybd.commit is swept through the
        // real open_image assembly path.
        {
            auto opened = co_await image::open_image(cfg, global);
            REQUIRE(opened.virtual_size == raw.size());
            std::vector<uint8_t> buf(raw.size());
            const ssize_t r =
                co_await opened.root->pread(buf.data(), buf.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(raw.size()));
            REQUIRE(buf == raw);
        }
        REQUIRE(server.data_gets() == served_gets);
        REQUIRE(names_with_prefix(layer_dir, ".download.").empty());
        REQUIRE(names_with_prefix(layer_dir, ".bitmap.").empty());
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: trace layer replays warm-up through the layer store",
          "[integration]") {
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 384, 71);
    const auto data_payload = make_zfile_blob(dir, raw);
    // The third record's range must fit in the payload.
    REQUIRE(data_payload.size() > 131072 + 4096);
    const auto data_blob = tar_wrap(data_payload);

    // Trace records in PAYLOAD byte space — the byte space upstream's
    // PrefetchFile records (the layer blob file below decompression; the
    // tar wrapper is invisible to it). With the 512B tar header the
    // records land on underlying extents 0, 1, 2:
    //   [0, 4096)       -> underlying [512, 4608)        extent 0
    //   [65024, +512)   -> underlying [65536, 66048)     extent 1 ONLY
    //   [131072, +4096) -> underlying [131584, 135680)   extent 2
    // The second record pins the tar-base translation against the key
    // mutation: translated it touches ONLY extent 1 (fresh — assembly
    // probes read the zfile header in extent 0 and the trailer/index in
    // the last extents), while an untranslated populate would touch ONLY
    // extent 0 (65024..65536), which probe traffic already covers. With
    // the translation dropped, extent 1 is never served and the
    // assertion below fails.
    format::trace::TraceWriter tw;
    REQUIRE(tw.append({'R', 0, 4096, 0}));
    REQUIRE(tw.append({'R', 0, 512, 65024}));
    REQUIRE(tw.append({'R', 0, 4096, 131072}));
    const auto trace_span = tw.finalize();
    const auto trace_blob =
        tar_wrap({trace_span.begin(), trace_span.end()});

    const std::string data_digest = "sha256:" + sha256_hex_of(data_blob);
    const std::string accel_digest = "sha256:" + sha256_hex_of(trace_blob);
    const std::string layer_dir = dir / "layer_traced";
    std::filesystem::create_directories(layer_dir);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobMapServer server(
            {{data_digest, data_blob}, {accel_digest, trace_blob}});
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        const bool server_running =
            co_await test::wait_server_running(server);
        REQUIRE(server_running);

        nlohmann::json cfgj;
        cfgj["repoBlobUrl"] = server.repo_base();
        cfgj["accelerationLayer"] = true;
        cfgj["lowers"] = nlohmann::json::array(
            {nlohmann::json{{"digest", data_digest},
                            {"size", data_blob.size()},
                            {"dir", layer_dir}},
             nlohmann::json{{"digest", accel_digest},
                            {"size", trace_blob.size()}}});
        const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
        // Structural warm-up windows at 0 (ADR-0012): this test pins the
        // REPLAY's extent attribution (an extent only the replay can
        // reach) — the default 1 MiB head/tail windows would warm this
        // small fixture blob whole and make the assertions below
        // attribution-blind.
        image::GlobalConfig global;
        global.prefetch_head_kb = 0;
        global.prefetch_tail_kb = 0;
        auto opened = co_await image::open_image(cfg, global);
        // The trace layer is set aside: one data layer, full content.
        REQUIRE(opened.layer_count == 1);
        REQUIRE(opened.virtual_size == raw.size());
        // The trace was recognized and replayed end to end.
        REQUIRE(opened.trace.trace_present);
        REQUIRE(opened.trace.records_total == 3);
        REQUIRE(opened.trace.records_replayed == 3);
        REQUIRE(opened.trace.bytes_warmed == 2 * 4096 + 512);
        // The acceleration layer blob itself was fetched (small, direct).
        REQUIRE(server.data_gets(accel_digest) >= 1);
        // Warm-up reached the data layer through LayerStore::populate with
        // the tar-base translation: extents 1 and 2 are fetched ONLY by
        // the replay (assembly probes touch the zfile header in extent 0
        // and the trailer/index in the last extents), and extent 1 in
        // particular is reachable only WITH the +512 translation (see the
        // record layout above).
        REQUIRE(server.served_extent(data_digest, 0));
        REQUIRE(server.served_extent(data_digest, 1));
        REQUIRE(server.served_extent(data_digest, 2));

        // Device reads never see trace bytes.
        std::vector<uint8_t> buf(raw.size());
        const ssize_t r = co_await opened.root->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(raw.size()));
        REQUIRE(buf == raw);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: trace replay warms the lower addressed by layer index",
          "[integration]") {
    // Two remote dir-configured data layers: a layer_index off-by-one or
    // a reversed warm_targets vector would warm the WRONG blob — pinned
    // here by a record for layer 1 whose extent nothing else touches.
    TempDir dir;
    // Large enough that extent 1 is a MIDDLE extent (assembly probes
    // touch extent 0 and the last extents only).
    const auto raw0 = test::pattern_bytes(512 * 384, 73);
    const auto raw1 = test::pattern_bytes(512 * 384, 79);
    const auto blob0 = make_zfile_blob(dir, raw0);
    const auto blob1 = make_zfile_blob(dir, raw1);
    REQUIRE(blob0.size() > 2 * 64 * 1024);
    REQUIRE(blob1.size() > 2 * 64 * 1024);
    // Plain (non-tar) blobs: record offsets map 1:1 to extent space.
    format::trace::TraceWriter tw;
    REQUIRE(tw.append({'R', 1, 4096, 65536}));  // layer 1, extent 1
    REQUIRE(tw.append({'R', 0, 4096, 0}));      // layer 0, extent 0
    const auto trace_span = tw.finalize();
    const auto trace_blob =
        tar_wrap({trace_span.begin(), trace_span.end()});

    const std::string digest0 = "sha256:" + sha256_hex_of(blob0);
    const std::string digest1 = "sha256:" + sha256_hex_of(blob1);
    const std::string accel_digest = "sha256:" + sha256_hex_of(trace_blob);
    const std::string layer_dir0 = dir / "layer0";
    const std::string layer_dir1 = dir / "layer1";
    std::filesystem::create_directories(layer_dir0);
    std::filesystem::create_directories(layer_dir1);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobMapServer server(
            {{digest0, blob0}, {digest1, blob1}, {accel_digest, trace_blob}});
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        const bool server_running =
            co_await test::wait_server_running(server);
        REQUIRE(server_running);

        nlohmann::json cfgj;
        cfgj["repoBlobUrl"] = server.repo_base();
        cfgj["accelerationLayer"] = true;
        cfgj["lowers"] = nlohmann::json::array(
            {nlohmann::json{{"digest", digest0},
                            {"size", blob0.size()},
                            {"dir", layer_dir0}},
             nlohmann::json{{"digest", digest1},
                            {"size", blob1.size()},
                            {"dir", layer_dir1}},
             nlohmann::json{{"digest", accel_digest},
                            {"size", trace_blob.size()}}});
        const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
        // Structural warm-up windows at 0 (ADR-0012): this test pins
        // trace-replay attribution — which exact extents the REPLAY
        // fetches — so the default 1 MiB head/tail windows (which would
        // warm these small blobs whole) must stay out of the way.
        image::GlobalConfig global;
        global.prefetch_head_kb = 0;
        global.prefetch_tail_kb = 0;
        auto opened = co_await image::open_image(cfg, global);

        REQUIRE(opened.layer_count == 2);
        REQUIRE(opened.trace.trace_present);
        REQUIRE(opened.trace.records_replayed == 2);
        // layer_index 1 must warm ONLY layer 1's blob: extent 1 is
        // unreachable by assembly probes (header in extent 0, trailer and
        // index in the last extents), so it is served exactly when the
        // right lower is populated — and must NOT appear on layer 0.
        REQUIRE(server.served_extent(digest1, 1));
        REQUIRE(!server.served_extent(digest0, 1));

        // The merged view is the two data layers: the top one wins.
        REQUIRE(opened.virtual_size == raw1.size());
        std::vector<uint8_t> buf(raw1.size());
        const ssize_t r = co_await opened.root->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(raw1.size()));
        REQUIRE(buf == raw1);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: structural warm-up fetches head and tail extents at bring-up",
          "[integration]") {
    // ADR-0012's cold-start floor: with warm-up enabled, open_image alone
    // (NO device read) persists the head/tail window extents of each
    // lower; with `prefetch.enable = false` the same extents stay cold.
    // The two phases run against DIFFERENT blobs on one mock server so
    // per-blob extent attribution stays phase-local. Windows are 256 KiB
    // per side (4 extents) to keep the fixture small; the blob is large
    // enough that a middle extent sits outside both windows and outside
    // the open-time probes (which touch the tar/zfile headers in extent
    // 0 and the zfile trailer/jump table plus the LSMT index in the
    // last ~2 payload extents).
    TempDir dir;
    const auto raw_off = test::pattern_bytes(512 * 384 * 6, 91);
    const auto raw_on = test::pattern_bytes(512 * 384 * 6, 92);
    const auto payload_off = make_zfile_blob(dir, raw_off);
    const auto payload_on = make_zfile_blob(dir, raw_on);
    const auto blob_off = tar_wrap(payload_off);
    const auto blob_on = tar_wrap(payload_on);
    constexpr uint64_t kWindow = 256 * 1024;
    constexpr uint64_t kExtent = 64 * 1024;
    REQUIRE(payload_off.size() > 3 * kWindow);
    REQUIRE(payload_on.size() > 3 * kWindow);

    const std::string digest_off = "sha256:" + sha256_hex_of(blob_off);
    const std::string digest_on = "sha256:" + sha256_hex_of(blob_on);
    const std::string dir_off = dir / "layer_off";
    const std::string dir_on = dir / "layer_on";
    std::filesystem::create_directories(dir_off);
    std::filesystem::create_directories(dir_on);

    // ustar base offset 512: the head window [0, 256 KiB) of the view
    // maps to underlying extents 0..4; extent 2 is reachable ONLY by the
    // head warm-up. The tail window [size-256 KiB, size) of the view
    // maps to underlying [size-256 KiB+512, size+512); its first extent
    // sits ~4 extents back from the payload end, beyond the probes'
    // reach. The middle extent must stay cold in BOTH phases (bounded
    // windows — warm-up must not flood the whole blob).
    const uint64_t head_probe_extent = 2;
    const uint64_t tail_extent_off =
        (payload_off.size() + 512 - kWindow) / kExtent;
    const uint64_t tail_extent_on =
        (payload_on.size() + 512 - kWindow) / kExtent;
    const uint64_t mid_extent_off = (payload_off.size() / 2 + 512) / kExtent;
    const uint64_t mid_extent_on = (payload_on.size() / 2 + 512) / kExtent;
    REQUIRE(mid_extent_off > head_probe_extent);
    REQUIRE(mid_extent_off < tail_extent_off);
    REQUIRE(mid_extent_on > 4);  // head warm-up reaches extent 4 (below)
    REQUIRE(mid_extent_on < tail_extent_on);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobMapServer server(
            {{digest_off, blob_off}, {digest_on, blob_on}});
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        const bool server_running =
            co_await test::wait_server_running(server);
        REQUIRE(server_running);

        auto make_cfg = [&](const std::string& digest, uint64_t size,
                            const std::string& layer_dir) {
            nlohmann::json cfgj;
            cfgj["repoBlobUrl"] = server.repo_base();
            cfgj["lowers"] = nlohmann::json::array(
                {nlohmann::json{{"digest", digest},
                                {"size", size},
                                {"dir", layer_dir}}});
            return image::ImageConfig::from_json_text(cfgj.dump(), {});
        };

        // Phase 1 — warm-up disabled: after bring-up, the head/tail
        // window extents were NOT fetched (only the open-time probes
        // ran), yet the device serves byte-exactly on demand.
        {
            const auto cfg = make_cfg(digest_off, blob_off.size(), dir_off);
            image::GlobalConfig global;
            global.prefetch_enable = false;
            auto opened = co_await image::open_image(cfg, global);
            REQUIRE(opened.warmup.windows_populated == 0);
            REQUIRE(!server.served_extent(digest_off, head_probe_extent));
            REQUIRE(!server.served_extent(digest_off, tail_extent_off));
            REQUIRE(!server.served_extent(digest_off, mid_extent_off));
            std::vector<uint8_t> buf(raw_off.size());
            const ssize_t r =
                co_await opened.root->pread(buf.data(), buf.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(raw_off.size()));
            REQUIRE(buf == raw_off);
        }

        // Phase 2 — warm-up enabled: after bring-up and BEFORE any
        // device read, the head window's extents (extent 2, beyond the
        // header probes) and the tail window's first extent (beyond the
        // trailer/index probes) are already fetched through the
        // LayerStore; the middle extent is not.
        {
            const auto cfg = make_cfg(digest_on, blob_on.size(), dir_on);
            image::GlobalConfig global;
            global.prefetch_head_kb = 256;
            global.prefetch_tail_kb = 256;
            auto opened = co_await image::open_image(cfg, global);
            REQUIRE(opened.warmup.layers_total == 1);
            REQUIRE(opened.warmup.layers_warmed == 1);
            REQUIRE(opened.warmup.windows_populated == 2);
            REQUIRE(opened.warmup.windows_failed == 0);
            REQUIRE(server.served_extent(digest_on, head_probe_extent));
            // The windows are computed in the tar VIEW byte space: view
            // [0, 256 KiB) translates through the +512 tar base to
            // underlying [512, 262656), reaching 512 bytes into extent
            // 4 — blob-space windows would stop at extent 3. Extent 4 is
            // touched by nothing else (probes: extent 0 and the last ~2
            // payload extents; the tail window starts far above it), so
            // this pins the view-space choice, not just warm-up
            // presence.
            REQUIRE(server.served_extent(digest_on, 4));
            REQUIRE(server.served_extent(digest_on, tail_extent_on));
            REQUIRE(!server.served_extent(digest_on, mid_extent_on));
            // The warmed extents answer device reads without new remote
            // traffic, and the full image reads back byte-exactly.
            std::vector<uint8_t> buf(raw_on.size());
            const ssize_t r =
                co_await opened.root->pread(buf.data(), buf.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(raw_on.size()));
            REQUIRE(buf == raw_on);
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: structural warm-up runs before the trace blob load",
          "[integration]") {
    // ADR-0012 "the floor first": the acceleration layer's trace blob
    // load/replay must not delay the structural warm-up — a slow or
    // unhealthy trace layer's fetch time sits outside both warm-up
    // budgets, so the floor runs BEFORE the first trace-blob byte is
    // fetched. Pinned via the mock's ordered request log: BOTH warm-up
    // windows of the data blob — the warm-up-only head extent 2 (beyond
    // the open-time probes' extent 0) AND the tail window's first
    // extent (beyond the probes' last ~2 payload extents) — are served
    // before the FIRST data GET of the trace blob. Requiring both
    // windows closes the loophole a head→trace→tail regression order
    // would otherwise slip through. Under the pre-fix order (trace
    // load ahead of layer construction) the trace blob's GETs would
    // lead the log instead.
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 384 * 6, 95);
    const auto data_payload = make_zfile_blob(dir, raw);
    REQUIRE(data_payload.size() > 3 * 256 * 1024);
    const auto data_blob = tar_wrap(data_payload);
    // ustar base offset 512 (same arithmetic as the extent-4 pin): the
    // tail window [size-256 KiB, size) of the view maps to underlying
    // [size-256 KiB+512, size+512); its first extent sits ~4 extents
    // back from the payload end, past the probes' reach.
    const uint64_t tail_extent =
        (data_payload.size() + 512 - 256 * 1024) / (64 * 1024);
    // Distinct from the head pin (extent 2) and from the traced middle
    // extent (640 KiB) below, so each pin attributes to its own cause.
    REQUIRE(tail_extent > 640 * 1024 / (64 * 1024));
    // One replay record into a MIDDLE extent (outside both 256 KiB
    // windows), so replay traffic is distinguishable from warm-up
    // traffic — and proves replay still works after the reorder.
    format::trace::TraceWriter tw;
    REQUIRE(tw.append({'R', 0, 4096, 640 * 1024}));
    const auto trace_span = tw.finalize();
    const auto trace_blob =
        tar_wrap({trace_span.begin(), trace_span.end()});

    const std::string data_digest = "sha256:" + sha256_hex_of(data_blob);
    const std::string accel_digest = "sha256:" + sha256_hex_of(trace_blob);
    const std::string layer_dir = dir / "layer_data";
    std::filesystem::create_directories(layer_dir);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobMapServer server(
            {{data_digest, data_blob}, {accel_digest, trace_blob}});
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        const bool server_running =
            co_await test::wait_server_running(server);
        REQUIRE(server_running);

        nlohmann::json cfgj;
        cfgj["repoBlobUrl"] = server.repo_base();
        cfgj["accelerationLayer"] = true;
        cfgj["lowers"] = nlohmann::json::array(
            {nlohmann::json{{"digest", data_digest},
                            {"size", data_blob.size()},
                            {"dir", layer_dir}},
             nlohmann::json{{"digest", accel_digest},
                            {"size", trace_blob.size()}}});
        const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
        image::GlobalConfig global;
        global.prefetch_head_kb = 256;
        global.prefetch_tail_kb = 256;
        auto opened = co_await image::open_image(cfg, global);

        // Both warm-up kinds ran end to end.
        REQUIRE(opened.warmup.windows_populated == 2);
        REQUIRE(opened.trace.trace_present);
        REQUIRE(opened.trace.records_replayed == 1);

        // The order pin: BOTH of the data blob's warm-up-only extents
        // (head window, tail window) were served BEFORE the trace
        // blob's first data GET — a head→trace→tail order fails red on
        // the tail check.
        const auto log = server.data_get_log();
        size_t first_accel = log.size();
        for (size_t i = 0; i < log.size(); ++i) {
            if (log[i].first == accel_digest) {
                first_accel = i;
                break;
            }
        }
        REQUIRE(first_accel < log.size());
        bool head_extent_before = false;
        bool tail_extent_before = false;
        for (size_t i = 0; i < first_accel; ++i) {
            if (log[i].first != data_digest) continue;
            if (log[i].second / (64 * 1024) == 2) head_extent_before = true;
            if (log[i].second / (64 * 1024) == tail_extent) {
                tail_extent_before = true;
            }
        }
        REQUIRE(head_extent_before);
        REQUIRE(tail_extent_before);

        // The replay warmed the traced middle extent, and the device
        // reads byte-exactly.
        REQUIRE(server.served_extent(data_digest, 640 * 1024 / (64 * 1024)));
        std::vector<uint8_t> buf(raw.size());
        const ssize_t r = co_await opened.root->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(raw.size()));
        REQUIRE(buf == raw);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

#ifdef OBD_TEST_HOOKS
TEST_CASE("integration: trace blob load budget skips slow trace and keeps reads",
          "[integration]") {
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 32, 96);
    const std::string layer = dir / "data.lsmt";
    {
        const std::string src = test::write_file(dir / "data.raw", raw);
        const int fd = ::open(src.c_str(), O_RDONLY);
        REQUIRE(fd >= 0);
        format::write_lsmt_single_layer(fd, raw.size(), layer, {});
        ::close(fd);
    }

    format::trace::TraceWriter tw;
    REQUIRE(tw.append({'R', 0, 4096, 0}));
    const auto trace_span = tw.finalize();
    const auto trace_blob =
        tar_wrap({trace_span.begin(), trace_span.end()});
    const std::string accel_digest = "sha256:" + sha256_hex_of(trace_blob);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobMapServer server({{accel_digest, trace_blob}});
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        const bool server_running =
            co_await test::wait_server_running(server);
        REQUIRE(server_running);

        nlohmann::json cfgj;
        cfgj["repoBlobUrl"] = server.repo_base();
        cfgj["accelerationLayer"] = true;
        cfgj["lowers"] = nlohmann::json::array(
            {nlohmann::json{{"digest", "sha256:data"}, {"file", layer}},
             nlohmann::json{{"digest", accel_digest},
                            {"size", trace_blob.size()}}});
        const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});

        image::GlobalConfig global;
        global.prefetch_head_kb = 0;
        global.prefetch_tail_kb = 0;

        {
            TraceBlobLoadBudgetGuard budget(std::chrono::milliseconds(500));
            auto opened = co_await image::open_image(cfg, global);
            REQUIRE(opened.trace.trace_present);
            REQUIRE(opened.trace.records_replayed == 1);
            std::vector<uint8_t> buf(raw.size());
            const ssize_t r =
                co_await opened.root->pread(buf.data(), buf.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(raw.size()));
            REQUIRE(buf == raw);
        }

        server.set_latency(std::chrono::milliseconds(300));
        const uint64_t data_gets_before_slow =
            server.data_gets(accel_digest);
        {
            TraceBlobLoadBudgetGuard budget(std::chrono::milliseconds(20));
            const auto start = std::chrono::steady_clock::now();
            auto opened = co_await image::open_image(cfg, global);
            const auto elapsed =
                std::chrono::steady_clock::now() - start;
            REQUIRE(elapsed < std::chrono::milliseconds(200));
            REQUIRE(!opened.trace.trace_present);
            REQUIRE(opened.trace.records_replayed == 0);

            std::vector<uint8_t> buf(raw.size());
            const ssize_t r =
                co_await opened.root->pread(buf.data(), buf.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(raw.size()));
            REQUIRE(buf == raw);
        }

        co_await elio::time::sleep_for(std::chrono::milliseconds(650));
        REQUIRE(server.data_gets(accel_digest) > data_gets_before_slow);
        co_return 0;
    });
    REQUIRE(rc == 0);
}
#endif

TEST_CASE("integration: admission funnel bounds on-demand latency under scavenger load",
          "[integration]") {
    // ADR-0012 acceptance: with a prefetch storm (Prefetch class) and the
    // background fill (Fill class) competing for a serialized, slow
    // source (capacity 1, 25 ms service time), every guest-blocking
    // on-demand read still completes within a bounded time: the funnel
    // admits on-demand unconditionally and caps the scavenger queue at
    // the AIMD window, so an on-demand request never waits behind more
    // than window_max scavenger service times.
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 8192, 87);  // 4 MiB
    const auto blob = make_zfile_blob(dir, raw);
    REQUIRE(blob.size() > 8 * 64 * 1024);  // many extents of competition
    const std::string digest = "sha256:" + sha256_hex_of(blob);
    const std::string layer_dir = dir / "layer_funnel";
    std::filesystem::create_directories(layer_dir);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobMapServer server({{digest, blob}});
        server.set_latency(std::chrono::milliseconds(25));
        server.set_serialized(true);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        std::optional<image::OpenedImage> opened;
        std::atomic<bool> storm_stop{false};
        std::atomic<int> storm_done{0};
        std::vector<elio::coro::join_handle<void>> storm_tasks;
        int storm_spawned = 0;
        bool storm_joined = false;
        bool cleaned = false;
        auto join_storm = [&]() -> elio::coro::task<void> {
            if (storm_joined) co_return;
            storm_stop.store(true, std::memory_order_relaxed);
            storm_joined = true;
            std::exception_ptr first_failure;
            for (auto& task : storm_tasks) {
                try {
                    co_await task;
                } catch (...) {
                    if (!first_failure) first_failure = std::current_exception();
                }
            }
            if (first_failure) std::rethrow_exception(first_failure);
            co_return;
        };
        auto cleanup = [&]() -> elio::coro::task<void> {
            if (cleaned) co_return;
            cleaned = true;
            co_await join_storm();
            if (opened) co_await image::park_image_fills(*opened);
            co_return;
        };

        std::exception_ptr failure;
        try {
            const bool server_running =
                co_await test::wait_server_running(server);
            REQUIRE(server_running);

            auto cfgj = remote_image_config_with_dir(
                server.repo_base(), sha256_hex_of(blob), blob.size(), layer_dir);
            cfgj["download"] = nlohmann::json{
                {"enable", true}, {"delay", 0}, {"delayExtra", 0}};
            const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
            const image::GlobalConfig global;
            opened.emplace(co_await image::open_image(cfg, global));
            REQUIRE(opened->layer_stores.size() == 1);
            source::LayerStore* store = opened->layer_stores[0];

            // Prefetch storm: six coroutines warming cold ranges of the same
            // store through populate() — scavenger pressure at the funnel on
            // top of the background fill. The 1 ms sleep every iteration is
            // LOAD-BEARING, not a pacing nicety: once the store is Complete
            // (the structural warm-up fetches most extents during bring-up,
            // so the fill frequently finishes mid-test), populate() becomes
            // a no-op that returns WITHOUT any suspension point — a storm
            // coroutine would then spin on its scheduler worker forever,
            // never observe storm_stop, and starve the other coroutines on
            // few-worker machines (the CI hang, issue #35).
            for (int i = 0; i < 6; ++i) {
                ++storm_spawned;
                storm_tasks.push_back(
                    elio::spawn([&, store, i]() -> elio::coro::task<void> {
                        const uint64_t total = store->size();
                        uint64_t off = static_cast<uint64_t>(i) * 256 * 1024;
                        while (!storm_stop.load(std::memory_order_relaxed)) {
                            const ssize_t pr = co_await store->populate(
                                off % total, 256 * 1024);
                            if (pr < 0) break;
                            off += 6 * 256 * 1024;
                            co_await elio::time::sleep_for(
                                std::chrono::milliseconds(1));
                        }
                        storm_done.fetch_add(1, std::memory_order_relaxed);
                    }));
            }

            // On-demand phase: sequential guest reads of cold extents, each
            // individually timed. With a capacity-1 source at 25 ms and the
            // window ceiling at 32, the worst case is ~32 queued scavenger
            // service times (~0.8 s); 2 s leaves scheduling slack while
            // still failing any run where on-demand traffic queues behind
            // unbounded scavenger load.
            constexpr int kReads = 12;
            std::vector<uint8_t> buf(16 * 1024);
            for (int i = 0; i < kReads; ++i) {
                // Sector-aligned (A3), spread across the 4 MiB image.
                const uint64_t off = static_cast<uint64_t>(i) * 256 * 1024;
                const auto t0 = std::chrono::steady_clock::now();
                const ssize_t r = co_await opened->root->pread(
                    buf.data(), buf.size(), off);
                const auto elapsed = std::chrono::steady_clock::now() - t0;
                REQUIRE(r == static_cast<ssize_t>(buf.size()));
                REQUIRE(buf == std::vector<uint8_t>(
                                   raw.begin() + static_cast<ptrdiff_t>(off),
                                   raw.begin() + static_cast<ptrdiff_t>(
                                                     off + buf.size())));
                REQUIRE(elapsed < std::chrono::seconds(2));
            }

            // The storm really was throttled at the funnel (not merely
            // absent): at least one scavenger request queued.
            REQUIRE(opened->funnel->scavenger_waits() > 0);

            storm_stop.store(true, std::memory_order_relaxed);
            for (int i = 0; i < 5000 && storm_done.load() < storm_spawned;
                 ++i) {
                co_await elio::time::sleep_for(std::chrono::milliseconds(1));
            }
            REQUIRE(storm_done.load() == storm_spawned);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await test::finish_with_async_cleanup(failure, cleanup);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: admission funnel collapses scavenger traffic under on-demand contention",
          "[integration]") {
    // ADR-0012 acceptance, cross-store: both lowers' LayerStores share
    // the one per-device funnel. While eight concurrent on-demand
    // readers stream cold extents of the TOP layer (keeping
    // inflight_on_demand > 0 almost continuously), the bottom layer's
    // background fill — a pure scavenger, its extents are shadowed and
    // never read — makes essentially no progress; once the contention
    // stops, the fill flows again. Both phases ride a 20 ms injected
    // source latency so a handful of admissions is clearly separable
    // from unrestricted fill progress.
    TempDir dir;
    const auto raw0 = test::pattern_bytes(512 * 16384, 93);  // 8 MiB
    const auto raw1 = test::pattern_bytes(512 * 16384, 97);
    const auto blob0 = make_zfile_blob(dir, raw0);
    const auto blob1 = make_zfile_blob(dir, raw1);
    const std::string digest0 = "sha256:" + sha256_hex_of(blob0);
    const std::string digest1 = "sha256:" + sha256_hex_of(blob1);
    const std::string layer_dir0 = dir / "layer_bottom";
    const std::string layer_dir1 = dir / "layer_top";
    std::filesystem::create_directories(layer_dir0);
    std::filesystem::create_directories(layer_dir1);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobMapServer server({{digest0, blob0}, {digest1, blob1}});
        server.set_latency(std::chrono::milliseconds(20));
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        std::optional<image::OpenedImage> opened;
        std::atomic<bool> readers_stop{false};
        std::atomic<int> readers_done{0};
        std::vector<elio::coro::join_handle<void>> reader_tasks;
        bool readers_joined = false;
        bool cleaned = false;
        auto join_readers = [&]() -> elio::coro::task<void> {
            if (readers_joined) co_return;
            readers_stop.store(true, std::memory_order_relaxed);
            readers_joined = true;
            std::exception_ptr first_failure;
            for (auto& task : reader_tasks) {
                try {
                    co_await task;
                } catch (...) {
                    if (!first_failure) first_failure = std::current_exception();
                }
            }
            if (first_failure) std::rethrow_exception(first_failure);
            co_return;
        };
        auto cleanup = [&]() -> elio::coro::task<void> {
            if (cleaned) co_return;
            cleaned = true;
            co_await join_readers();
            if (opened) co_await image::park_image_fills(*opened);
            co_return;
        };

        std::exception_ptr failure;
        try {
            const bool server_running =
                co_await test::wait_server_running(server);
            REQUIRE(server_running);

            nlohmann::json cfgj;
            cfgj["repoBlobUrl"] = server.repo_base();
            cfgj["lowers"] = nlohmann::json::array(
                {nlohmann::json{{"digest", digest0},
                                {"size", blob0.size()},
                                {"dir", layer_dir0}},
                 nlohmann::json{{"digest", digest1},
                                {"size", blob1.size()},
                                {"dir", layer_dir1}}});
            cfgj["download"] = nlohmann::json{
                {"enable", true}, {"delay", 0}, {"delayExtra", 0}};
            const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
            // Structural warm-up windows at 0 (ADR-0012): the phase-1
            // on-demand fetch counts below assume every read extent is cold;
            // pre-warmed head/tail extents would shrink them.
            image::GlobalConfig global;
            global.prefetch_head_kb = 0;
            global.prefetch_tail_kb = 0;
            opened.emplace(co_await image::open_image(cfg, global));
            REQUIRE(opened->layer_stores.size() == 2);
            source::LayerStore* top = opened->layer_stores[1];

            const uint64_t extents =
                (top->size() + 64 * 1024 - 1) / (64 * 1024);
            REQUIRE(extents >= 64);  // enough cold extents for a real storm

            // Phase 1: eight concurrent on-demand readers streaming cold
            // extents of the top layer's store, partitioned by stride.
            const uint64_t d0_before = server.data_gets(digest0);
            const uint64_t d1_before = server.data_gets(digest1);
            for (int i = 0; i < 8; ++i) {
                reader_tasks.push_back(
                    elio::spawn([&, top, extents, i]() -> elio::coro::task<void> {
                        std::vector<uint8_t> buf(64 * 1024);
                        for (uint64_t e = static_cast<uint64_t>(i);
                             e < extents &&
                             !readers_stop.load(std::memory_order_relaxed);
                             e += 8) {
                            const ssize_t r = co_await top->pread(
                                buf.data(), buf.size(), e * 64 * 1024);
                            if (r <= 0) break;
                        }
                        readers_done.fetch_add(1, std::memory_order_relaxed);
                    }));
            }
            for (int i = 0; i < 30000 && readers_done.load() < 8; ++i) {
                co_await elio::time::sleep_for(std::chrono::milliseconds(1));
            }
            REQUIRE(readers_done.load() == 8);
            co_await join_readers();

            // The contention was real (dozens of on-demand fetches)...
            const uint64_t d1_during =
                server.data_gets(digest1) - d1_before;
            REQUIRE(d1_during >= 48);
            // ...and the shadowed bottom layer's fill collapsed under it:
            // unrestricted it would have fetched roughly one extent per
            // 20 ms for the whole phase.
            const uint64_t d0_during =
                server.data_gets(digest0) - d0_before;
            REQUIRE(d0_during <= 6);

            // Phase 2: contention over — the scavenger flows again.
            bool recovered = false;
            for (int i = 0; i < 15000 && !recovered; ++i) {
                recovered = server.data_gets(digest0) >=
                            d0_before + d0_during + 15;
                if (!recovered) {
                    co_await elio::time::sleep_for(
                        std::chrono::milliseconds(1));
                }
            }
            REQUIRE(recovered);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await test::finish_with_async_cleanup(failure, cleanup);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: issue28 hidden cleanup probe preserves assertion failure",
          "[integration][.][issue28-harness]") {
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 2048, 109);  // 1 MiB
    const auto blob = make_zfile_blob(dir, raw);
    const std::string digest = "sha256:" + sha256_hex_of(blob);
    const std::string layer_dir = dir / "layer_issue28_probe";
    std::filesystem::create_directories(layer_dir);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobMapServer server({{digest, blob}});
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        std::optional<image::OpenedImage> opened;
        std::vector<elio::coro::join_handle<void>> readers;
        std::atomic<int> readers_started{0};
        bool readers_joined = false;
        bool cleaned = false;
        auto join_readers = [&]() -> elio::coro::task<void> {
            if (readers_joined) co_return;
            server.release_data_gets();
            readers_joined = true;
            std::exception_ptr first_failure;
            for (auto& reader : readers) {
                try {
                    co_await reader;
                } catch (...) {
                    if (!first_failure) first_failure = std::current_exception();
                }
            }
            if (first_failure) std::rethrow_exception(first_failure);
            co_return;
        };
        auto cleanup = [&]() -> elio::coro::task<void> {
            if (cleaned) co_return;
            cleaned = true;
            co_await join_readers();
            if (opened) co_await image::park_image_fills(*opened);
            co_return;
        };

        std::exception_ptr failure;
        try {
            const bool server_running =
                co_await test::wait_server_running(server);
            REQUIRE(server_running);

            auto cfgj = remote_image_config_with_dir(
                server.repo_base(), sha256_hex_of(blob), blob.size(), layer_dir);
            cfgj["download"] = nlohmann::json{
                {"enable", true}, {"delay", 5}, {"delayExtra", 0}};
            const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
            image::GlobalConfig global;
            global.prefetch_head_kb = 0;
            global.prefetch_tail_kb = 0;
            opened.emplace(co_await image::open_image(cfg, global));
            REQUIRE(opened->layer_stores.size() == 1);
            source::LayerStore* store = opened->layer_stores[0];
            server.block_data_gets();

            for (int i = 0; i < 2; ++i) {
                readers.push_back(
                    elio::spawn([&, store, i]() -> elio::coro::task<void> {
                        readers_started.fetch_add(1, std::memory_order_relaxed);
                        std::vector<uint8_t> buf(64 * 1024);
                        const uint64_t off = static_cast<uint64_t>(i) *
                                             64 * 1024;
                        const ssize_t r = co_await store->pread(
                            buf.data(), buf.size(), off);
                        (void)r;
                    }));
            }
            for (int i = 0; i < 5000 &&
                            (readers_started.load() < 2 ||
                             server.blocked_data_gets() < 1);
                 ++i) {
                co_await elio::time::sleep_for(std::chrono::milliseconds(1));
            }
            INFO("issue28 intentional failure marker: live readers are blocked");
            REQUIRE(readers_started.load() == 2);
            REQUIRE(server.blocked_data_gets() >= 1);
            REQUIRE(false);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await test::finish_with_async_cleanup(failure, cleanup);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: issue28 hidden sibling runs after failed cleanup probe",
          "[integration][.][issue28-harness]") {
    std::cout << "issue28 sibling executed after cleanup probe" << std::endl;
    REQUIRE(true);
}

TEST_CASE("integration: concurrent mock servers bind distinct ephemeral ports",
          "[integration]") {
    // Issue #15 regression tripwire: mock blob servers must never
    // hard-code listener ports (the fixed 1919x/1920x range collided
    // across sibling worktrees/CI runs on one machine). Two servers live
    // at once on this host; each must get its own OS-assigned ephemeral
    // port and serve its own bytes — impossible with a fixed port, which
    // would make the second listen fail with EADDRINUSE.
    const auto blob_a = test::pattern_bytes(64 * 1024, 111);
    const auto blob_b = test::pattern_bytes(64 * 1024, 222);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobServer server_a(blob_a);
        BlobServer server_b(blob_b);
        std::optional<test::TcpPortBlocker> blocker;
        std::atomic<bool> armed{true};
        std::atomic<uint16_t> blocked_port{0};
        server_a.set_before_listen_hook([&](uint16_t port) {
            if (!armed.exchange(false)) return;
            blocker.emplace(port);
            blocked_port.store(port, std::memory_order_release);
        });
        elio::go([&server_a]() -> elio::coro::task<void> {
            co_await server_a.run();
        });
        elio::go([&server_b]() -> elio::coro::task<void> {
            co_await server_b.run();
        });
        BlobGuard guard_a{server_a};
        BlobGuard guard_b{server_b};
        const bool server_a_running =
            co_await test::wait_server_running(server_a);
        REQUIRE(server_a_running);
        const bool server_b_running =
            co_await test::wait_server_running(server_b);
        REQUIRE(server_b_running);
        REQUIRE(blocked_port.load(std::memory_order_acquire) != 0);
        REQUIRE(server_a.port() != 0);
        REQUIRE(server_b.port() != 0);
        REQUIRE(server_a.port() !=
                blocked_port.load(std::memory_order_acquire));
        REQUIRE(server_a.port() != server_b.port());

        auto client = std::make_shared<source::RegistryClient>(
            nullptr, source::RegistryClientConfig{});
        auto src_a =
            co_await source::RegistrySource::open(client, server_a.url("b"));
        auto src_b =
            co_await source::RegistrySource::open(client, server_b.url("b"));
        REQUIRE(src_a->size() == blob_a.size());
        REQUIRE(src_b->size() == blob_b.size());
        std::vector<uint8_t> buf(4096);
        const ssize_t got_a =
            co_await src_a->pread(buf.data(), buf.size(), 0);
        REQUIRE(got_a == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == std::vector<uint8_t>(blob_a.begin(),
                                            blob_a.begin() + buf.size()));
        const ssize_t got_b =
            co_await src_b->pread(buf.data(), buf.size(), 0);
        REQUIRE(got_b == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == std::vector<uint8_t>(blob_b.begin(),
                                            blob_b.begin() + buf.size()));
        blocker.reset();
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: TurboOCI target persists separately and reopens offline",
          "[integration][turboci]") {
    TempDir dir;
    constexpr size_t index_offset = 4608;
    std::vector<uint8_t> meta(index_offset + 32 + 4096, 0);
    const uint8_t magic[] = {0x4c,0x53,0x4d,0x54,0,1,2,0,
        0x65,0x7e,0x63,0xd2,0x94,0x44,8,0x4c,0xa2,0xd2,0xc8,0xec,0x4f,0xcf,0xae,0x8a};
    for (const auto base : {size_t(0), meta.size() - 4096}) {
        std::copy(std::begin(magic), std::end(magic), meta.begin() + base);
        bytes::store_u32_le(meta.data() + base + 24, 390);
        bytes::store_u32_le(meta.data() + base + 28, base == 0 ? 3 : 6);
        bytes::store_u64_le(meta.data() + base + 32, index_offset);
        bytes::store_u64_le(meta.data() + base + 40, 2);
        bytes::store_u64_le(meta.data() + base + 48, 1024);
        meta[base + 132] = meta[base + 133] = 1;
    }
    std::fill(meta.begin() + 4096, meta.begin() + index_offset, 0x4d);
    bytes::store_u64_le(meta.data() + index_offset, 0x0004000000000000ULL);
    bytes::store_u64_le(meta.data() + index_offset + 8, 8);
    bytes::store_u64_le(meta.data() + index_offset + 16, 0x0004000000000001ULL);
    bytes::store_u64_le(meta.data() + index_offset + 24, 0x0100000000000001ULL);
    auto target = std::vector<uint8_t>(1536, 0x48);
    std::fill(target.begin() + 512, target.begin() + 1024, 0x54);
    const auto metadata_path = test::write_file(dir / "ext4.fs.meta", meta);

    const auto fixture = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "fixtures/turboci";
    std::ifstream gzip_input(fixture / "original.tar.gz", std::ios::binary);
    const std::vector<uint8_t> gzip((std::istreambuf_iterator<char>(gzip_input)), {});
    REQUIRE(gzip.size() > 1024);
    const auto gzip_digest = sha256_hex_of(gzip);
    const auto digest = sha256_hex_of(target);
    const std::string layer_dir = dir / "cache";
    const std::string committed = layer_dir + "/targets/" + digest + "/overlaybd.commit";
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobMapServer server({{"sha256:" + digest, target},
                              {"sha256:" + gzip_digest, gzip}});
        elio::go([&server]() -> elio::coro::task<void> { co_await server.run(); });
        BlobGuard guard{server};
        const bool running = co_await test::wait_server_running(server);
        REQUIRE(running);
        nlohmann::json j = {{"repoBlobUrl", server.repo_base()},
            {"download", {{"enable", true}, {"delay", 0}, {"delayExtra", 0}}},
            {"lowers", nlohmann::json::array({{{"file", metadata_path},
                {"dir", layer_dir}, {"targetDigest", "sha256:" + digest}}})}};
        image::GlobalConfig global;
        global.prefetch_enable = false;
        // A malformed index must park the already-started target fill.
        {
            const auto bad_index = test::write_file(dir / "bad-gzip.meta",
                                                    std::vector<uint8_t>(333, 0));
            auto bad = j;
            bad["lowers"][0]["dir"] = dir / "bad-cache";
            bad["lowers"][0]["gzipIndex"] = bad_index;
            bad["lowers"][0]["targetDigest"] = "sha256:" + gzip_digest;
            bad["download"]["delay"] = 60;
            const auto bad_cfg = image::ImageConfig::from_json_text(bad.dump(), {});
            std::string rejection;
            source::test_hooks::reset_unparked_layer_store_destructions_for_test();
            try {
                auto unexpected = co_await image::open_image(bad_cfg, global);
                co_await image::park_image_fills(unexpected);
            } catch (const error& e) {
                rejection = e.what();
            }
            REQUIRE(rejection.find("invalid ddgzidx v1 index") != std::string::npos);
            REQUIRE(source::test_hooks::unparked_layer_store_destructions_for_test() == 0);
        }
        {
            const auto cfg = image::ImageConfig::from_json_text(j.dump(), {});
            auto opened = co_await image::open_image(cfg, global);
            REQUIRE(opened.layer_stores.size() == 1);
            std::vector<uint8_t> data(1024);
            const auto n = co_await opened.root->pread(data.data(), data.size(), 0);
            REQUIRE(n == 1024);
            REQUIRE(std::all_of(data.begin() + 512, data.end(), [](auto c) { return c == 0x54; }));
            bool complete = false;
            for (int i = 0; i < 400 && !complete; ++i) {
                complete = std::filesystem::exists(committed);
                if (!complete) co_await elio::time::sleep_for(std::chrono::milliseconds(25));
            }
            co_await image::park_image_fills(opened);
            REQUIRE(complete);
        }
        REQUIRE_FALSE(std::filesystem::exists(layer_dir + "/overlaybd.commit"));
        const auto target_cache_dir = std::filesystem::path(committed).parent_path().string();
        test::write_file(target_cache_dir + "/.download.deadbeefdeadbeef",
                         std::vector<uint8_t>(1, 0));
        test::write_file(target_cache_dir + "/.bitmap.deadbeefdeadbeef",
                         std::vector<uint8_t>(80, 0));
        REQUIRE(names_with_prefix(target_cache_dir, ".download.").size() == 1);
        REQUIRE(names_with_prefix(target_cache_dir, ".bitmap.").size() == 1);
        // No registry URL proves committed targets can reopen entirely offline.
        j.erase("repoBlobUrl");
        global.prefetch_enable = true;
        const auto cfg = image::ImageConfig::from_json_text(j.dump(), {});
        auto reopened = co_await image::open_image(cfg, global);
        REQUIRE(reopened.layer_stores.empty());
        REQUIRE(names_with_prefix(target_cache_dir, ".download.").empty());
        REQUIRE(names_with_prefix(target_cache_dir, ".bitmap.").empty());
        REQUIRE(reopened.warmup.layers_total == 1);
        REQUIRE(reopened.warmup.layers_warmed == 1);
        REQUIRE(reopened.warmup.windows_populated == 2);
        std::vector<uint8_t> data(1024);
        const auto n = co_await reopened.root->pread(data.data(), data.size(), 0);
        REQUIRE(n == 1024);
        REQUIRE(std::all_of(data.begin(), data.begin() + 512, [](auto c) { return c == 0x4d; }));
        REQUIRE(std::all_of(data.begin() + 512, data.end(), [](auto c) { return c == 0x54; }));
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: malformed remote TurboOCI metadata parks its store",
          "[integration][turboci]") {
    TempDir dir;
    // Long enough for all format probes, but an invalid LSMT header. The
    // consuming open_warp must not destroy the store before assembly parks it.
    const std::vector<uint8_t> malformed(128 * 1024, 0x51);
    const auto target_path = test::write_file(dir / "original.tar",
                                             std::vector<uint8_t>(1536, 0));
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobServer server(malformed);
        elio::go([&server]() -> elio::coro::task<void> { co_await server.run(); });
        BlobGuard guard{server};
        const bool running = co_await test::wait_server_running(server);
        REQUIRE(running);
        auto j = remote_image_config_with_dir(server.repo_base(),
            sha256_hex_of(malformed), malformed.size(), dir / "metadata-cache");
        j["lowers"][0]["targetFile"] = target_path;
        j["download"] = {{"enable", true}, {"delay", 60}, {"delayExtra", 0}};
        const auto cfg = image::ImageConfig::from_json_text(j.dump(), {});
        image::GlobalConfig global;
        global.prefetch_enable = false;
        source::test_hooks::reset_unparked_layer_store_destructions_for_test();
        bool rejected = false;
        std::optional<image::OpenedImage> opened;
        try {
            opened.emplace(co_await image::open_image(cfg, global));
        } catch (const error&) {
            rejected = true;
        }
        if (opened) co_await image::park_image_fills(*opened);
        REQUIRE(rejected);
        REQUIRE(source::test_hooks::unparked_layer_store_destructions_for_test() == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: TurboOCI target encoding requires matching gzip index",
          "[integration][turboci]") {
    TempDir dir;
    constexpr size_t index_offset = 4608;
    std::vector<uint8_t> meta(index_offset + 32 + 4096, 0);
    const uint8_t magic[] = {0x4c,0x53,0x4d,0x54,0,1,2,0,
        0x65,0x7e,0x63,0xd2,0x94,0x44,8,0x4c,0xa2,0xd2,0xc8,0xec,0x4f,0xcf,0xae,0x8a};
    for (const auto base : {size_t(0), meta.size() - 4096}) {
        std::copy(std::begin(magic), std::end(magic), meta.begin() + base);
        bytes::store_u32_le(meta.data() + base + 24, 390);
        bytes::store_u32_le(meta.data() + base + 28, base == 0 ? 3 : 6);
        bytes::store_u64_le(meta.data() + base + 32, index_offset);
        bytes::store_u64_le(meta.data() + base + 40, 2);
        bytes::store_u64_le(meta.data() + base + 48, 1024);
        meta[base + 132] = meta[base + 133] = 1;
    }
    std::fill(meta.begin() + 4096, meta.begin() + index_offset, 0x4d);
    bytes::store_u64_le(meta.data() + index_offset, 0x0004000000000000ULL);
    bytes::store_u64_le(meta.data() + index_offset + 8, 8);
    bytes::store_u64_le(meta.data() + index_offset + 16, 0x0004000000000001ULL);
    bytes::store_u64_le(meta.data() + index_offset + 24, 0x0100000000000001ULL);
    auto target = std::vector<uint8_t>(1536, 0x48);
    std::fill(target.begin() + 512, target.begin() + 1024, 0x54);
    const auto metadata_path = test::write_file(dir / "ext4.fs.meta", meta);

    const auto fixture = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "fixtures/turboci";
    std::ifstream input(fixture / "original.tar.gz", std::ios::binary);
    const std::vector<uint8_t> gzip((std::istreambuf_iterator<char>(input)), {});
    REQUIRE(gzip.size() > 1024); // Warp target ranges also fit compressed size.
    const auto raw_path = test::write_file(dir / "raw.tar", target);
    const auto digest = sha256_hex_of(gzip);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobServer server(gzip);
        elio::go([&server]() -> elio::coro::task<void> { co_await server.run(); });
        BlobGuard guard{server};
        const bool running = co_await test::wait_server_running(server);
        REQUIRE(running);
        image::GlobalConfig global;
        global.prefetch_enable = false;
        for (int mode = 0; mode < 4; ++mode) {
            nlohmann::json lower = {{"file", metadata_path}};
            nlohmann::json j = {{"repoBlobUrl", server.repo_base()},
                {"download", {{"enable", true}, {"delay", 60}, {"delayExtra", 0}}}};
            if (mode == 0) {
                lower["targetFile"] = (fixture / "original.tar.gz").string();
            } else if (mode == 1) {
                lower["targetDigest"] = "sha256:" + digest;
                lower["dir"] = dir / "remote-cache";
            } else if (mode == 2) {
                const std::string cache = dir / "committed-cache";
                const auto cache_dir = cache + "/targets/" + digest;
                std::filesystem::create_directories(cache_dir);
                test::write_file(cache_dir + "/overlaybd.commit", gzip);
                lower["targetDigest"] = "sha256:" + digest;
                lower["dir"] = cache;
                j.erase("repoBlobUrl");
            } else {
                lower["targetFile"] = raw_path;
                lower["gzipIndex"] = (fixture / "gzip.meta").string();
            }
            j["lowers"] = nlohmann::json::array({lower});
            const auto cfg = image::ImageConfig::from_json_text(j.dump(), {});
            std::string rejection;
            source::test_hooks::reset_unparked_layer_store_destructions_for_test();
            std::optional<image::OpenedImage> opened;
            try {
                opened.emplace(co_await image::open_image(cfg, global));
            } catch (const error& e) {
                rejection = e.what();
            }
            if (opened) co_await image::park_image_fills(*opened);
            const std::string expected = mode == 3 ?
                "gzipIndex requires a gzip target" : "gzip target requires gzipIndex";
            INFO("target mode " << mode << ": " << rejection);
            REQUIRE(rejection.find(expected) != std::string::npos);
            REQUIRE(source::test_hooks::unparked_layer_store_destructions_for_test() == 0);
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
}
