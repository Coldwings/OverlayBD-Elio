// Integration tests: the full read pipeline (registry mock -> chunk cache
// -> tar adapter -> zfile -> lsmt merge), image assembly via open_image,
// DART optional-accelerator fallback, background download and the
// remote->local switch. No kernel dependencies (ublk E2E lives in
// test_ublk_e2e.cpp and self-skips). See docs/testing.md.
#include "common/sha256.hpp"
#include "format/lsmt.hpp"
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
};

/// RAII stop: an exception mid-test must not leave the accept loop pending
/// (a pending detached task hangs scheduler shutdown).
struct BlobGuard {
    BlobServer& server;
    ~BlobGuard() { server.stop(); }
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
