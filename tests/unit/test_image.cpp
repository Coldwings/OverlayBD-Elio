// Unit tests: image module — config parsing and local-file assembly.
#include "common/errors.hpp"
#include "image/image_file.hpp"
#include "format/writer.hpp"

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>

using namespace obd;
using obd::test::TempDir;

TEST_CASE("image: global config parses overlaybd.json fields", "[image]") {
    const std::string text = R"({
        "credentialConfig": {"mode": "file", "path": "/tmp/cred.json"},
        "p2pConfig": {"enable": true, "address": "localhost:19145/dart"},
        "download": {"enable": true, "delay": 120, "maxMBps": 50, "tryCnt": 1},
        "prefetch": {"enable": false},
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
    REQUIRE(g.download.try_count == 1);
    REQUIRE(!g.prefetch_enable);  // prefetch.enable honored (ADR-0012)
    REQUIRE(g.log_level == 0);
    // Absent prefetch section: the default is enabled.
    const auto g2 = image::GlobalConfig::from_json_text("{}");
    REQUIRE(g2.prefetch_enable);

    const auto default_try =
        image::GlobalConfig::from_json_text(R"({"download": {"enable": true}})");
    REQUIRE(default_try.download.try_count == 5);
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
        "download": {"maxMBps": 10, "tryCnt": 5}
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
    REQUIRE(cfg.download.try_count == 5);   // unsigned JSON integer accepted
    REQUIRE(image::ImageConfig::digest_sha256_hex("sha256:abc") == "abc");
    REQUIRE(image::ImageConfig::digest_sha256_hex("sha512:abc").empty());
}

TEST_CASE("image: invalid download tryCnt is rejected at config boundaries", "[image]") {
    auto expect_trycnt_error = [](auto&& fn) {
        try {
            fn();
            FAIL("invalid download.tryCnt should fail");
        } catch (const error& e) {
            REQUIRE(e.errno_value() == EINVAL);
            REQUIRE(std::string(e.what()).find("download.tryCnt") !=
                    std::string::npos);
        }
    };

    expect_trycnt_error([] {
        (void)image::GlobalConfig::from_json_text(
            R"({"download": {"tryCnt": 0}})");
    });
    expect_trycnt_error([] {
        (void)image::GlobalConfig::from_json_text(
            R"({"download": {"tryCnt": -1}})");
    });
    expect_trycnt_error([] {
        (void)image::GlobalConfig::from_json_text(
            R"({"download": {"tryCnt": 4294967296}})");
    });
    expect_trycnt_error([] {
        (void)image::GlobalConfig::from_json_text(
            R"({"download": {"tryCnt": 1.5}})");
    });
    expect_trycnt_error([] {
        (void)image::GlobalConfig::from_json_text(
            R"({"download": {"tryCnt": "2"}})");
    });

    image::DownloadConfig defaults;
    defaults.try_count = 7;
    expect_trycnt_error([&] {
        (void)image::ImageConfig::from_json_text(
            R"({
                "repoBlobUrl": "https://reg.example.com/v2/lib/nginx/blobs",
                "lowers": [{"digest": "sha256:aaa", "size": 123}],
                "download": {"tryCnt": -1}
            })",
            defaults);
    });

    TempDir dir;
    nlohmann::json cfgj;
    cfgj["repoBlobUrl"] = "http://127.0.0.1:1/v2";
    cfgj["lowers"] = nlohmann::json::array({nlohmann::json{
        {"digest", "sha256:" + std::string(64, 'a')},
        {"size", 65536},
        {"dir", dir / "layer"}}});
    auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
    cfg.download.try_count = 0;

    int thrown_errno = 0;
    std::string thrown_message;
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        const image::GlobalConfig global;
        try {
            (void)co_await image::open_image(cfg, global);
        } catch (const error& e) {
            thrown_errno = e.errno_value();
            thrown_message = e.what();
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(thrown_errno == EINVAL);
    REQUIRE(thrown_message.find("download.tryCnt") != std::string::npos);
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

TEST_CASE("image: device capacity honors the virtual_size headroom override grow-only",
          "[image]") {
    // D3 create-time headroom rule (device_capacity_bytes): no override
    // = the image's declared size; an override >= the image size is
    // sanctioned headroom (equal is a no-op); a smaller override would
    // shrink the device below its content and is rejected with a reason.
    std::string err;
    REQUIRE(image::device_capacity_bytes(512 * 64, 0, &err) == 512 * 64);
    REQUIRE(err.empty());
    REQUIRE(image::device_capacity_bytes(512 * 64, 512 * 64, &err) ==
            512 * 64);
    REQUIRE(err.empty());
    REQUIRE(image::device_capacity_bytes(512 * 64, 512 * 96, &err) ==
            512 * 96);
    REQUIRE(err.empty());
    // Null error pointer is safe on the rejection path.
    REQUIRE(image::device_capacity_bytes(512 * 64, 512 * 32, nullptr) == 0);
    const uint64_t rejected =
        image::device_capacity_bytes(512 * 64, 512 * 32, &err);
    REQUIRE(rejected == 0);
    REQUIRE(err.find("grow-only") != std::string::npos);
    REQUIRE(err.find("smaller than the image") != std::string::npos);
}

TEST_CASE("image: TurboOCI configuration retains target identity and index", "[image][turboci]") {
    nlohmann::json lower = {
        {"file", "ext4.fs.meta"}, {"targetFile", "original.tar.gz"},
        {"targetDigest", "sha256:" + std::string(64, 'a')},
        {"gzipIndex", "gzip.meta"}
    };
    const auto cfg = image::ImageConfig::from_json_text(
        nlohmann::json{{"lowers", nlohmann::json::array({lower})}}.dump(), {});
    REQUIRE(cfg.lowers.size() == 1);
    REQUIRE(cfg.lowers[0].target_file == "original.tar.gz");
    REQUIRE(cfg.lowers[0].target_digest == "sha256:" + std::string(64, 'a'));
    REQUIRE(cfg.lowers[0].gzip_index == "gzip.meta");
}

TEST_CASE("image: TurboOCI configuration rejects orphan index and bad digest", "[image][turboci]") {
    REQUIRE_THROWS_AS(image::ImageConfig::from_json_text(
        R"({"lowers":[{"file":"ext4.fs.meta","gzipIndex":"gzip.meta"}]})", {}),
        format_error);
    REQUIRE_THROWS_AS(image::ImageConfig::from_json_text(
        R"({"lowers":[{"targetDigest":"sha256:bad"}]})", {}), format_error);
}

TEST_CASE("image: TurboOCI assembly retains original tar byte offsets", "[image][turboci]") {
    TempDir dir;
    // Independent warp header/index words: logical sector0 is metadata;
    // logical sector1 comes from target sector1 (not tar-stripped sector0).
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
    const auto target_path = test::write_file(dir / "original.tar", target);
    nlohmann::json j = {{"lowers", nlohmann::json::array({
        {{"file", metadata_path}, {"targetFile", target_path}}})}};
    auto cfg = image::ImageConfig::from_json_text(j.dump(), {});
    const auto rc = test::run_coro([&]() -> elio::coro::task<int> {
        image::GlobalConfig global;
        global.prefetch_enable = false;
        auto opened = co_await image::open_image(cfg, global);
        std::vector<uint8_t> data(1024);
        const auto n = co_await opened.root->pread(data.data(), data.size(), 0);
        REQUIRE(n == 1024);
        REQUIRE(std::all_of(data.begin(), data.begin() + 512, [](auto c) { return c == 0x4d; }));
        REQUIRE(std::all_of(data.begin() + 512, data.end(), [](auto c) { return c == 0x54; }));
        co_return 0;
    });
    REQUIRE(rc == 0);
}
