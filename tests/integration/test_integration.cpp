// Integration tests: the full read pipeline (registry mock -> layer store
// -> tar adapter -> zfile -> lsmt merge), image assembly via open_image
// (remote layers served through the LayerStore, ADR-0011), DART
// optional-accelerator fallback, and background fill through image
// assembly. No kernel dependencies (ublk E2E lives in test_ublk_e2e.cpp
// and self-skips). See docs/testing.md.
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

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>

using namespace obd;
using obd::test::TempDir;
namespace http = elio::http;

namespace {

/// Minimal Range-capable blob server: every GET under /v2/* serves the one
/// hosted blob.
class BlobServer {
public:
    BlobServer(std::vector<uint8_t> blob, uint16_t port)
        : blob_(std::move(blob)), port_(port) {
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
    uint16_t port_;
    std::unique_ptr<http::server> server_;
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
    BlobMapServer(std::map<std::string, std::vector<uint8_t>> blobs,
                  uint16_t port)
        : blobs_(std::move(blobs)), port_(port) {
        for (const auto& [name, blob] : blobs_) {
            stats_[name] = std::make_unique<Stats>();
        }
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
    uint16_t port_;
    std::unique_ptr<http::server> server_;
    std::map<std::string, std::unique_ptr<Stats>> stats_;
    mutable std::mutex log_mu_;
    std::vector<std::pair<std::string, uint64_t>> log_;
    std::atomic<int64_t> latency_ms_{0};
    std::atomic<bool> serialized_{false};
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

}  // namespace

TEST_CASE("integration: layered stack stages over a mock registry",
          "[integration]") {
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 32, 41);
    const auto blob = make_zfile_blob(dir, raw);
    const std::string layer_dir = dir / "layer_staging";
    std::filesystem::create_directories(layer_dir);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobServer server(blob, 19190);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));
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
        REQUIRE(co_await format::is_zfile(*untarred));
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
        BlobServer server(blob, 19199);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));
        // Unreachable probe (nothing on 19999): must time out fast.
        const auto addr = source::parse_dart_address("127.0.0.1:19999/dart");
        REQUIRE(addr.has_value());
        REQUIRE(!co_await source::dart_proxy_reachable(*addr));
        // Subsequent IO on this scheduler must still work.
        auto client = std::make_shared<source::RegistryClient>(
            nullptr, source::RegistryClientConfig{});
        auto src =
            co_await source::RegistrySource::open(client, server.url("b"));
        std::vector<uint8_t> buf(1024);
        REQUIRE(co_await src->pread(buf.data(), buf.size(), 0) == 1024);
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
        BlobServer server(blob, 19198);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

        const auto cfgj = remote_image_config(
            server.repo_base(), sha256_hex_of(blob), blob.size());
        const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
        image::GlobalConfig global;
        global.p2p_enable = true;
        global.p2p_address = "127.0.0.1:19999/dart";  // nothing listening
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
        BlobServer server(blob, 19195);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

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
        BlobServer server(blob, 19194);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

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
        BlobServer server(blob, 19189);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

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
        BlobServer server(blob, 19191);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

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
        BlobServer server(blob, 19192);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

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
        BlobServer server(blob, 19193);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

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
        const uint64_t served_gets = server.data_gets();

        // Run 2: the commit marker binds the layer locally (the local
        // probe) — zero remote data reads, byte-exact content.
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
            {{data_digest, data_blob}, {accel_digest, trace_blob}}, 19201);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

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
            {{digest0, blob0}, {digest1, blob1}, {accel_digest, trace_blob}},
            19202);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

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
            {{digest_off, blob_off}, {digest_on, blob_on}}, 19205);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

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
    // fetched. Pinned via the mock's ordered request log: a warm-up-only
    // head extent (extent 2, beyond the open-time probes' extent 0) of
    // the data blob is served before the FIRST data GET of the trace
    // blob. Under the pre-fix order (trace load ahead of layer
    // construction) the trace blob's GETs would lead the log instead.
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 384 * 6, 95);
    const auto data_payload = make_zfile_blob(dir, raw);
    REQUIRE(data_payload.size() > 3 * 256 * 1024);
    const auto data_blob = tar_wrap(data_payload);
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
            {{data_digest, data_blob}, {accel_digest, trace_blob}}, 19206);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

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

        // The order pin: the data blob's warm-up-only head extent was
        // served BEFORE the trace blob's first data GET.
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
        for (size_t i = 0; i < first_accel; ++i) {
            if (log[i].first == data_digest &&
                log[i].second / (64 * 1024) == 2) {
                head_extent_before = true;
            }
        }
        REQUIRE(head_extent_before);

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
        BlobMapServer server({{digest, blob}}, 19203);
        server.set_latency(std::chrono::milliseconds(25));
        server.set_serialized(true);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

        auto cfgj = remote_image_config_with_dir(
            server.repo_base(), sha256_hex_of(blob), blob.size(), layer_dir);
        cfgj["download"] = nlohmann::json{
            {"enable", true}, {"delay", 0}, {"delayExtra", 0}};
        const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
        const image::GlobalConfig global;
        auto opened = co_await image::open_image(cfg, global);
        REQUIRE(opened.layer_stores.size() == 1);
        source::LayerStore* store = opened.layer_stores[0];

        // Prefetch storm: six coroutines warming cold ranges of the same
        // store through populate() — scavenger pressure at the funnel on
        // top of the background fill.
        std::atomic<bool> storm_stop{false};
        std::atomic<int> storm_done{0};
        for (int i = 0; i < 6; ++i) {
            elio::go([&, i]() -> elio::coro::task<void> {
                const uint64_t total = store->size();
                uint64_t off = static_cast<uint64_t>(i) * 256 * 1024;
                while (!storm_stop.load(std::memory_order_relaxed)) {
                    const ssize_t pr =
                        co_await store->populate(off % total, 256 * 1024);
                    if (pr < 0) break;
                    off += 6 * 256 * 1024;
                }
                storm_done.fetch_add(1, std::memory_order_relaxed);
            });
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
            const ssize_t r = co_await opened.root->pread(
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
        REQUIRE(opened.funnel->scavenger_waits() > 0);

        storm_stop.store(true, std::memory_order_relaxed);
        for (int i = 0; i < 5000 && storm_done.load() < 6; ++i) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(storm_done.load() == 6);
        co_await image::park_image_fills(opened);
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
        BlobMapServer server({{digest0, blob0}, {digest1, blob1}}, 19204);
        server.set_latency(std::chrono::milliseconds(20));
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

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
        auto opened = co_await image::open_image(cfg, global);
        REQUIRE(opened.layer_stores.size() == 2);
        source::LayerStore* top = opened.layer_stores[1];

        const uint64_t extents =
            (top->size() + 64 * 1024 - 1) / (64 * 1024);
        REQUIRE(extents >= 64);  // enough cold extents for a real storm

        // Phase 1: eight concurrent on-demand readers streaming cold
        // extents of the top layer's store, partitioned by stride.
        std::atomic<int> readers_done{0};
        const uint64_t d0_before = server.data_gets(digest0);
        const uint64_t d1_before = server.data_gets(digest1);
        for (int i = 0; i < 8; ++i) {
            elio::go([&, i]() -> elio::coro::task<void> {
                std::vector<uint8_t> buf(64 * 1024);
                for (uint64_t e = static_cast<uint64_t>(i); e < extents;
                     e += 8) {
                    const ssize_t r = co_await top->pread(
                        buf.data(), buf.size(), e * 64 * 1024);
                    if (r <= 0) break;
                }
                readers_done.fetch_add(1, std::memory_order_relaxed);
            });
        }
        for (int i = 0; i < 30000 && readers_done.load() < 8; ++i) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(readers_done.load() == 8);

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
                co_await elio::time::sleep_for(std::chrono::milliseconds(1));
            }
        }
        REQUIRE(recovered);

        co_await image::park_image_fills(opened);
        co_return 0;
    });
    REQUIRE(rc == 0);
}
