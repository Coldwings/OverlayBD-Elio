// Unit tests: format module — golden wire-format bytes and round-trips
// through the synchronous writers and the coroutine readers.
#include "format/lsmt.hpp"
#include "format/lsmt_format.hpp"
#include "format/writer.hpp"
#include "format/zfile.hpp"
#include "format/zfile_format.hpp"
#include "source/local_file.hpp"

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>

using namespace obd;
using obd::test::TempDir;
using obd::test::pattern_bytes;

namespace {

std::string write_pattern_file(const TempDir& dir, const std::string& name,
                               const std::vector<uint8_t>& data) {
    return test::write_file(dir / name, data);
}

}  // namespace

TEST_CASE("format: zfile header bytes match the OverlayBD wire format",
          "[format]") {
    TempDir dir;
    const auto raw = pattern_bytes(3 * 4096 + 100);
    const std::string in = write_pattern_file(dir, "raw.img", raw);
    const std::string out = dir / "out.zfile";
    const int fd = ::open(in.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    format::write_zfile(fd, raw.size(), out, {});
    ::close(fd);

    const auto bytes = std::vector<uint8_t>{};
    (void)bytes;
    // Read the 512B header region and check the pinned fields.
    const int out_fd = ::open(out.c_str(), O_RDONLY);
    REQUIRE(out_fd >= 0);
    uint8_t hdr[512];
    REQUIRE(::pread(out_fd, hdr, sizeof(hdr), 0) == sizeof(hdr));

    // magic0 "ZFile\0\1\0" and magic1 the overlaybd magic.
    REQUIRE(std::memcmp(hdr, "ZFile\0\1\0", 7) == 0);
    const char* kMagic1 = "tuji.yyf@Alibaba";
    REQUIRE(std::memcmp(hdr + 8, kMagic1, 16) == 0);
    REQUIRE(test::run_coro([]() -> elio::coro::task<int> { co_return 0; }) == 0);
    REQUIRE(obd::bytes::load_u32_le(hdr + 24) == 96);     // ht size
    const uint32_t flags = obd::bytes::load_u32_le(hdr + 32);
    REQUIRE(flags & (1u << format::zfile::kFlagShiftHeader));    // is_header
    REQUIRE(flags & (1u << format::zfile::kFlagShiftType));      // data file
    REQUIRE(flags & (1u << format::zfile::kFlagShiftSealed));    // sealed
    REQUIRE(obd::bytes::load_u64_le(hdr + 56) == raw.size());    // vsize
    ::close(out_fd);
}

TEST_CASE("format: zfile round-trip reads back the original content",
          "[format]") {
    TempDir dir;
    const auto raw = pattern_bytes(200 * 1024 + 13);
    const std::string in = write_pattern_file(dir, "raw.img", raw);
    const std::string out = dir / "out.zfile";
    const int fd = ::open(in.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    format::ZFileWriteOptions opts;
    opts.block_size = 4096;
    format::write_zfile(fd, raw.size(), out, opts);
    ::close(fd);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto local = co_await source::LocalFileSource::open(out);
        source::BlobSourcePtr base = std::move(local);
        auto zf = co_await format::ZFileSource::open(std::move(base),
                                                     /*caller_verify=*/true);
        REQUIRE(zf->size() == raw.size());
        std::vector<uint8_t> buf(raw.size());
        const ssize_t r = co_await zf->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(raw.size()));
        REQUIRE(buf == raw);
        // Unaligned tail read.
        std::vector<uint8_t> tail(1000);
        const ssize_t r2 =
            co_await zf->pread(tail.data(), tail.size(), raw.size() - 700);
        REQUIRE(r2 == 700);
        REQUIRE(std::memcmp(tail.data(), raw.data() + raw.size() - 700,
                            700) == 0);
        // Past EOF.
        const ssize_t r3 = co_await zf->pread(tail.data(), 10, raw.size());
        REQUIRE(r3 == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: lsmt header bytes match the OverlayBD wire format",
          "[format]") {
    TempDir dir;
    const auto raw = pattern_bytes(512 * 100);
    const std::string in = write_pattern_file(dir, "raw.img", raw);
    const std::string out = dir / "out.lsmt";
    const int fd = ::open(in.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    format::write_lsmt_single_layer(fd, raw.size(), out,
                                    {/*.uuid=*/"", "", "tag"});
    ::close(fd);

    const int out_fd = ::open(out.c_str(), O_RDONLY);
    REQUIRE(out_fd >= 0);
    uint8_t hdr[4096];
    REQUIRE(::pread(out_fd, hdr, sizeof(hdr), 0) == sizeof(hdr));
    // magic0 "LSMT\0\1\2\0"
    REQUIRE(std::memcmp(hdr, "LSMT\0\1\2\0", 7) == 0);
    const uint8_t kMagic1[16] = {0x65, 0x7e, 0x63, 0xd2, 0x94, 0x44,
                                 0x08, 0x4c, 0xa2, 0xd2, 0xc8, 0xec,
                                 0x4f, 0xcf, 0xae, 0x8a};
    REQUIRE(std::memcmp(hdr + 8, kMagic1, 16) == 0);
    REQUIRE(obd::bytes::load_u32_le(hdr + 24) == 390);  // ht size
    const uint32_t flags = obd::bytes::load_u32_le(hdr + 28);
    REQUIRE(flags & (1u << format::lsmt::kFlagShiftHeader));
    REQUIRE(flags & (1u << format::lsmt::kFlagShiftType));
    REQUIRE(flags & (1u << format::lsmt::kFlagShiftSealed));
    REQUIRE(obd::bytes::load_u64_le(hdr + 48) == raw.size());  // virtual_size
    ::close(out_fd);
}

TEST_CASE("format: lsmt round-trip and multi-layer merge semantics",
          "[format]") {
    TempDir dir;
    // Bottom layer: 64 sectors of 'A'-pattern.
    const auto bottom_raw = pattern_bytes(512 * 64, 7);
    const std::string f1 = write_pattern_file(dir, "bottom.img", bottom_raw);
    // Top layer: 64 sectors of 'B'-pattern (covers the whole device).
    const auto top_raw = pattern_bytes(512 * 64, 9);
    const std::string f2 = write_pattern_file(dir, "top.img", top_raw);

    const std::string l1 = dir / "bottom.lsmt";
    const std::string l2 = dir / "top.lsmt";
    int fd = ::open(f1.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    format::write_lsmt_single_layer(fd, bottom_raw.size(), l1, {"", "", ""});
    ::close(fd);
    fd = ::open(f2.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    format::write_lsmt_single_layer(fd, top_raw.size(), l2, {"", "", ""});
    ::close(fd);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        // Single-layer read.
        {
            auto s = co_await source::LocalFileSource::open(l1);
            source::BlobSourcePtr b = std::move(s);
            auto layer = co_await format::LsmtLayer::open(std::move(b));
            REQUIRE(layer->virtual_size() == bottom_raw.size());
            REQUIRE(!layer->segments().empty());
            uint8_t buf[512];
            const ssize_t r = co_await layer->data_source().pread(
                buf, sizeof(buf), 4096);  // data starts at sector 8
            REQUIRE(r == 512);
            REQUIRE(std::memcmp(buf, bottom_raw.data(), 512) == 0);
        }
        // Merged view: top layer wins over its whole range.
        {
            std::vector<std::unique_ptr<format::LsmtLayer>> layers;
            {
                auto s = co_await source::LocalFileSource::open(l1);
                source::BlobSourcePtr b = std::move(s);
                layers.push_back(
                    co_await format::LsmtLayer::open(std::move(b)));
            }
            {
                auto s = co_await source::LocalFileSource::open(l2);
                source::BlobSourcePtr b = std::move(s);
                layers.push_back(
                    co_await format::LsmtLayer::open(std::move(b)));
            }
            auto merged =
                co_await format::MergedLsmt::open(std::move(layers));
            REQUIRE(merged->size() == top_raw.size());
            std::vector<uint8_t> buf(top_raw.size());
            const ssize_t r = co_await merged->pread(buf.data(), buf.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(top_raw.size()));
            REQUIRE(buf == top_raw);
            // Unaligned read must fail (sector contract).
            const ssize_t bad = co_await merged->pread(buf.data(), 100, 3);
            REQUIRE(bad == -EINVAL);
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: merge falls through holes to lower layers", "[format]") {
    // Direct index-merge test through the public merge_indexes helper:
    // top covers [4,8), bottom covers [0,16) → merged = bottom[0,4) +
    // top[4,8) + bottom[8,16).
    std::vector<bytes::segment_mapping> bottom(1), top(1);
    bottom[0].offset = 0;
    bottom[0].length = 16;
    bottom[0].moffset = 8;
    top[0].offset = 4;
    top[0].length = 4;
    top[0].moffset = 8;
    const std::vector<const std::vector<bytes::segment_mapping>*> stack = {
        &top, &bottom};  // topmost first
    std::vector<bytes::segment_mapping> out;
    format::MergedLsmt::merge_indexes(stack, out);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0].offset == 0);
    REQUIRE(out[0].length == 4);
    REQUIRE(out[0].tag == 1);  // bottom
    REQUIRE(out[1].offset == 4);
    REQUIRE(out[1].length == 4);
    REQUIRE(out[1].tag == 0);  // top
    REQUIRE(out[2].offset == 8);
    REQUIRE(out[2].length == 8);
    REQUIRE(out[2].tag == 1);
    // Bottom segment clipped head: moffset shifted.
    REQUIRE(out[2].moffset == 8 + 8);
}

TEST_CASE("format: zfile reader rejects a corrupted block digest",
          "[format]") {
    TempDir dir;
    const auto raw = pattern_bytes(8192);
    const std::string in = write_pattern_file(dir, "raw.img", raw);
    const std::string out = dir / "out.zfile";
    const int fd = ::open(in.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    format::write_zfile(fd, raw.size(), out, {});
    ::close(fd);

    // Flip a byte inside the first compressed block (starts at offset 512).
    const int rw = ::open(out.c_str(), O_RDWR);
    REQUIRE(rw >= 0);
    uint8_t b = 0;
    REQUIRE(::pread(rw, &b, 1, 512) == 1);
    b ^= 0xFF;
    REQUIRE(::pwrite(rw, &b, 1, 512) == 1);
    ::close(rw);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto local = co_await source::LocalFileSource::open(out);
        source::BlobSourcePtr base = std::move(local);
        auto zf = co_await format::ZFileSource::open(std::move(base), true);
        std::vector<uint8_t> buf(raw.size());
        const ssize_t r = co_await zf->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == -EIO);  // crc32c_salt mismatch
        co_return 0;
    });
    REQUIRE(rc == 0);
}
