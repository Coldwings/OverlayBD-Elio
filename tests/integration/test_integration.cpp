// Integration tests: the full read pipeline (registry mock -> chunk cache
// -> tar adapter -> zfile -> lsmt merge), image assembly via open_image
// (remote layers served through the LayerStore, ADR-0011), DART
// optional-accelerator fallback, background download and the
// remote->local switch. No kernel dependencies (ublk E2E lives in
// test_ublk_e2e.cpp and self-skips). See docs/testing.md.
#include "common/sha256.hpp"
#include "format/lsmt.hpp"
#include "format/trace.hpp"
#include "format/writer.hpp"
#include "format/zfile.hpp"
#include "image/image_file.hpp"
#include "source/chunk_cache.hpp"
#include "source/dart.hpp"
#include "source/downloader.hpp"
#include "source/registry.hpp"
#include "source/switch_source.hpp"
#include "source/tar_offset.hpp"

#include "../support.hpp"

#include <elio/http/http_server.hpp>
#include <elio/runtime/spawn.hpp>
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
            served_extents_.insert(first / (64 * 1024));
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
/// right layer (ADR-0013 trace replay test).
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
            std::lock_guard lk(st->extents_mu);
            for (uint64_t e = first / (64 * 1024); e * 64 * 1024 <= last;
                 ++e) {
                st->extents.insert(e);
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
        auto cache = co_await source::ChunkCache::open(std::move(reg));
        std::vector<uint8_t> buf(1000);
        REQUIRE(co_await cache->pread(buf.data(), buf.size(), 100) == 1000);
        auto untarred =
            co_await source::TarOffsetSource::open(std::move(cache));
        REQUIRE(co_await format::is_zfile(*untarred));
        auto view =
            co_await format::ZFileSource::open(std::move(untarred), true);
        auto layer = co_await format::LsmtLayer::open(std::move(view));
        REQUIRE(layer->virtual_size() == raw.size());
        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
        layers.push_back(std::move(layer));
        auto merged = co_await format::MergedLsmt::open(std::move(layers));
        std::vector<uint8_t> all(raw.size());
        REQUIRE(co_await merged->pread(all.data(), all.size(), 0) ==
                static_cast<ssize_t>(raw.size()));
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

TEST_CASE("integration: downloader writes, verifies and installs the blob",
          "[integration]") {
    TempDir dir;
    auto blob = test::pattern_bytes(96 * 1024, 61);
    const std::string digest_hex = sha256_hex_of(blob);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobServer server(blob, 19196);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

        auto client = std::make_shared<source::RegistryClient>(
            nullptr, source::RegistryClientConfig{});
        auto remote =
            co_await source::RegistrySource::open(client, server.url("blob"));
        const std::string layer_dir = dir / "layer1";
        std::filesystem::create_directories(layer_dir);

        source::DownloadConfig dcfg;
        dcfg.enable = true;
        dcfg.delay_sec = 0;
        dcfg.delay_extra_sec = 0;
        dcfg.block_size = 16 * 1024;
        source::Downloader dl(std::move(remote), layer_dir, digest_hex, dcfg);
        dl.start();
        for (int i = 0; i < 400 &&
                        dl.status() != source::Downloader::Status::kDone &&
                        dl.status() != source::Downloader::Status::kFailed;
             ++i) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(25));
        }
        REQUIRE(dl.status() == source::Downloader::Status::kDone);
        REQUIRE(dl.bytes_done() == blob.size());
        const int fd =
            ::open(source::Downloader::target_path(layer_dir).c_str(),
                   O_RDONLY);
        REQUIRE(fd >= 0);
        std::vector<uint8_t> got(blob.size());
        REQUIRE(::read(fd, got.data(), got.size()) ==
                static_cast<ssize_t>(got.size()));
        ::close(fd);
        REQUIRE(got == blob);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: switch source swaps reads to the local copy",
          "[integration]") {
    TempDir dir;
    auto blob = test::pattern_bytes(64 * 1024, 71);
    const std::string digest_hex = sha256_hex_of(blob);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        BlobServer server(blob, 19197);
        elio::go([&server]() -> elio::coro::task<void> {
            co_await server.run();
        });
        BlobGuard guard{server};
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));

        auto client = std::make_shared<source::RegistryClient>(
            nullptr, source::RegistryClientConfig{});
        const std::string url = server.url("blob");
        auto cached = co_await source::ChunkCache::open(
            co_await source::RegistrySource::open(client, url));
        auto raw_remote = co_await source::RegistrySource::open(client, url);
        const std::string layer_dir = dir / "layer2";
        std::filesystem::create_directories(layer_dir);
        source::DownloadConfig dcfg;
        dcfg.enable = true;
        dcfg.delay_sec = 0;
        dcfg.delay_extra_sec = 0;
        auto sw = co_await source::SwitchSource::open(
            std::move(cached), std::move(raw_remote), layer_dir, digest_hex,
            dcfg);
        REQUIRE(!sw->switched());
        std::vector<uint8_t> buf(4096);
        REQUIRE(co_await sw->pread(buf.data(), buf.size(), 0) == 4096);
        for (int i = 0; i < 400 && !sw->switched(); ++i) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(25));
        }
        REQUIRE(sw->switched());
        std::vector<uint8_t> buf2(4096);
        REQUIRE(co_await sw->pread(buf2.data(), buf2.size(), 8192) == 4096);
        REQUIRE(buf2 == std::vector<uint8_t>(blob.begin() + 8192,
                                             blob.begin() + 8192 + 4096));
        co_return 0;
    });
    REQUIRE(rc == 0);
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
        const image::GlobalConfig global;

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
    //   [65024, +4096)  -> underlying [65536, 69632)     extent 1
    //   [131072, +4096) -> underlying [131584, 135680)   extent 2
    format::trace::TraceWriter tw;
    REQUIRE(tw.append({'R', 0, 4096, 0}));
    REQUIRE(tw.append({'R', 0, 4096, 65024}));
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
        const image::GlobalConfig global;
        auto opened = co_await image::open_image(cfg, global);

        // The trace layer is set aside: one data layer, full content.
        REQUIRE(opened.layer_count == 1);
        REQUIRE(opened.virtual_size == raw.size());
        // The trace was recognized and replayed end to end.
        REQUIRE(opened.trace.trace_present);
        REQUIRE(opened.trace.records_total == 3);
        REQUIRE(opened.trace.records_replayed == 3);
        REQUIRE(opened.trace.bytes_warmed == 3 * 4096);
        // The acceleration layer blob itself was fetched (small, direct).
        REQUIRE(server.data_gets(accel_digest) >= 1);
        // Warm-up reached the data layer through LayerStore::populate with
        // the tar-base translation: extents 1 and 2 are fetched ONLY by
        // the replay (assembly probes touch the zfile header in extent 0
        // and the trailer/index in the last extents).
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
