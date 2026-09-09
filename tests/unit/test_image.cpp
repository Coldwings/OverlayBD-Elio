// Unit tests: image module — config parsing and local-file assembly.
#include "common/errors.hpp"
#include "image/image_file.hpp"
#include "format/writer.hpp"

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/stat.h>

using namespace obd;
using obd::test::TempDir;

TEST_CASE("image: global config parses overlaybd.json fields", "[image]") {
    const std::string text = R"({
        "credentialConfig": {"mode": "file", "path": "/tmp/cred.json"},
        "p2pConfig": {"enable": true, "address": "localhost:19145/dart"},
        "download": {"enable": true, "delay": 120, "maxMBps": 50},
        "logConfig": {"logLevel": 0},
        "cacheConfig": {"ignored": true}
    })";
    const auto g = image::GlobalConfig::from_json_text(text);
    REQUIRE(g.credential_file == "/tmp/cred.json");
    REQUIRE(g.p2p_enable);
    REQUIRE(g.p2p_address == "localhost:19145/dart");
    REQUIRE(g.download.enable);
    REQUIRE(g.download.delay_sec == 120);
    REQUIRE(g.download.max_mbps == 50);
    REQUIRE(g.download.try_count == 5);  // defaults preserved
    REQUIRE(g.log_level == 0);
}

TEST_CASE("image: per-image download overrides merge over global defaults",
          "[image]") {
    image::DownloadConfig defaults;
    defaults.enable = true;
    defaults.delay_sec = 300;
    defaults.max_mbps = 100;
    const std::string text = R"({
        "repoBlobUrl": "https://reg.example.com/v2/lib/nginx/blobs",
        "lowers": [{"digest": "sha256:aaa", "size": 123, "dir": "/l1"}],
        "download": {"maxMBps": 10}
    })";
    const auto cfg = image::ImageConfig::from_json_text(text, defaults);
    REQUIRE(cfg.repo_blob_url ==
            "https://reg.example.com/v2/lib/nginx/blobs");
    REQUIRE(cfg.lowers.size() == 1);
    REQUIRE(cfg.lowers[0].digest == "sha256:aaa");
    REQUIRE(cfg.lowers[0].size == 123);
    REQUIRE(cfg.download.enable);           // inherited
    REQUIRE(cfg.download.delay_sec == 300); // inherited
    REQUIRE(cfg.download.max_mbps == 10);   // overridden
    REQUIRE(image::ImageConfig::digest_sha256_hex("sha256:abc") == "abc");
    REQUIRE(image::ImageConfig::digest_sha256_hex("sha512:abc").empty());
}

TEST_CASE("image: upper config parses; unknown type rejected", "[image]") {
    // ADR-0008: a non-empty upper engages the writable mode.
    const std::string text = R"({
        "repoBlobUrl": "x",
        "lowers": [{"digest": "sha256:a", "size": 1}],
        "upper": {"dir": "/upper"}
    })";
    const auto cfg = image::ImageConfig::from_json_text(text, {});
    REQUIRE(cfg.writable());
    REQUIRE(cfg.upper.dir == "/upper");
    REQUIRE(cfg.upper.type == "lsmt");  // default
    // Empty/absent upper stays read-only.
    const std::string ok = R"({"repoBlobUrl":"x","lowers":[]})";
    REQUIRE(!image::ImageConfig::from_json_text(ok, {}).writable());
    // Unknown upper types are rejected.
    const std::string bad = R"({
        "repoBlobUrl": "x", "lowers": [],
        "upper": {"dir": "/u", "type": "turboci"}
    })";
    REQUIRE_THROWS(image::ImageConfig::from_json_text(bad, {}));
}

TEST_CASE("image: assembly from local layer files reads merged content",
          "[image]") {
    TempDir dir;
    const auto bottom_raw = test::pattern_bytes(512 * 32, 31);
    const auto top_raw = test::pattern_bytes(512 * 32, 33);
    const std::string l1 = dir / "bottom.lsmt";
    const std::string l2 = dir / "top.lsmt";
    {
        const std::string r1 = test::write_file(dir / "b.img", bottom_raw);
        const int fd = ::open(r1.c_str(), O_RDONLY);
        REQUIRE(fd >= 0);
        format::write_lsmt_single_layer(fd, bottom_raw.size(), l1, {});
        ::close(fd);
        const std::string r2 = test::write_file(dir / "t.img", top_raw);
        const int fd2 = ::open(r2.c_str(), O_RDONLY);
        REQUIRE(fd2 >= 0);
        format::write_lsmt_single_layer(fd2, top_raw.size(), l2, {});
        ::close(fd2);
    }
    nlohmann::json cfgj;
    cfgj["repoBlobUrl"] = "";
    cfgj["lowers"] = nlohmann::json::array(
        {nlohmann::json{{"digest", "sha256:b"}, {"file", l1}},
         nlohmann::json{{"digest", "sha256:t"}, {"file", l2}}});
    const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        const image::GlobalConfig global;
        auto opened = co_await image::open_image(cfg, global);
        REQUIRE(opened.layer_count == 2);
        REQUIRE(opened.virtual_size == top_raw.size());
        std::vector<uint8_t> buf(top_raw.size());
        const ssize_t r = co_await opened.root->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(top_raw.size()));
        REQUIRE(buf == top_raw);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("image: malformed remote lower digest fails assembly", "[image]") {
    // ADR-0016: the remote-only degrade covers environment failures (an
    // unusable layer dir); a malformed lower digest is a structural
    // config error and must fail loud — before any registry I/O (the
    // repo URL below is intentionally dead), not by degrading.
    TempDir dir;
    nlohmann::json cfgj;
    cfgj["repoBlobUrl"] = "http://127.0.0.1:1/v2";
    cfgj["lowers"] = nlohmann::json::array({nlohmann::json{
        {"digest", "sha256:abcd"},
        {"size", 65536},
        {"dir", dir / "layer"}}});
    const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});

    int thrown_errno = 0;
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        const image::GlobalConfig global;
        try {
            (void)co_await image::open_image(cfg, global);
        } catch (const error& e) {
            thrown_errno = e.errno_value();
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(thrown_errno == EINVAL);
}

TEST_CASE("image: assembly picks the ZFile view for compressed layers",
          "[image]") {
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 32, 35);
    const std::string rawp = test::write_file(dir / "r.img", raw);
    const std::string lsmt = dir / "l.lsmt";
    const std::string zf = dir / "l.zfile";
    {
        const int fd = ::open(rawp.c_str(), O_RDONLY);
        REQUIRE(fd >= 0);
        format::write_lsmt_single_layer(fd, raw.size(), lsmt, {});
        ::close(fd);
        const int fd2 = ::open(lsmt.c_str(), O_RDONLY);
        REQUIRE(fd2 >= 0);
        struct stat st {};
        REQUIRE(::fstat(fd2, &st) == 0);
        format::write_zfile(fd2, static_cast<uint64_t>(st.st_size), zf, {});
        ::close(fd2);
    }
    nlohmann::json cfgj;
    cfgj["repoBlobUrl"] = "";
    cfgj["lowers"] = nlohmann::json::array(
        {nlohmann::json{{"digest", "sha256:z"}, {"file", zf}}});
    const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        const image::GlobalConfig global;
        auto opened = co_await image::open_image(cfg, global);
        REQUIRE(opened.virtual_size == raw.size());
        std::vector<uint8_t> buf(raw.size());
        const ssize_t r = co_await opened.root->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(raw.size()));
        REQUIRE(buf == raw);
        co_return 0;
    });
    REQUIRE(rc == 0);
}
