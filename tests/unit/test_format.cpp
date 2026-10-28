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
#include <algorithm>
#include <array>

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

TEST_CASE("format: lsmt tail placement preserves bytes with ordinary logical mappings",
          "[format]") {
    TempDir dir;
    std::vector<uint8_t> raw(8 * 512);
    for (size_t sector = 0; sector < 8; ++sector)
        std::fill_n(raw.begin() + sector * 512, 512, static_cast<uint8_t>(sector + 1));
    const auto input = test::write_file(dir / "raw", raw);
    const auto output = dir / "clustered.lsmt";
    const auto equivalent = dir / "equivalent.lsmt";
    format::LsmtWriteOptions options;
    options.uuid = "01234567-89ab-cdef-0123-456789abcdef";
    options.tail_ranges = {{0, 1}, {3, 2}, {7, 1}};
    const int fd = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    try {
        format::write_lsmt_single_layer(fd, raw.size(), output, options);
        options.tail_ranges = {{0, 1}, {3, 1}, {4, 1}, {7, 1}};
        format::write_lsmt_single_layer(fd, raw.size(), equivalent, options);
    } catch (...) { ::close(fd); throw; }
    REQUIRE(::close(fd) == 0);
    const int result = test::run_coro([&]() -> elio::coro::task<int> {
        auto blob = co_await source::LocalFileSource::open(output);
        auto layer = co_await format::LsmtLayer::open(std::move(blob));
        // The reader exposes the authoritative trailer, not the leading header.
        REQUIRE(layer->header().flags == 6);
        std::array<uint8_t, 4096> leading_header{};
        const auto header_read = co_await layer->data_source().pread(
            leading_header.data(), leading_header.size(), 0);
        REQUIRE(header_read == static_cast<ssize_t>(leading_header.size()));
        REQUIRE(bytes::load_u32_le(leading_header.data() + 28) == 7);
        REQUIRE(layer->header().index_offset == 8192);
        REQUIRE(layer->header().version == 1);
        REQUIRE(layer->segments().size() == 5);
        const std::vector<uint64_t> physical{12, 8, 13, 10, 15};
        for (size_t i = 0; i < physical.size(); ++i) {
            REQUIRE(layer->segments()[i].moffset == physical[i]);
            REQUIRE(layer->segments()[i].tag == 0);
            REQUIRE_FALSE(layer->segments()[i].zeroed);
        }
        // Inspect physical payload order independently of the merged reader.
        std::vector<uint8_t> packed(raw.size());
        const auto packed_read = co_await layer->data_source().pread(packed.data(), packed.size(), 4096);
        REQUIRE(packed_read == static_cast<ssize_t>(packed.size()));
        const std::vector<uint8_t> sector_values{2, 3, 6, 7, 1, 4, 5, 8};
        for (size_t i = 0; i < sector_values.size(); ++i)
            REQUIRE(std::all_of(packed.begin() + i * 512, packed.begin() + (i + 1) * 512,
                                [&](uint8_t value) { return value == sector_values[i]; }));
        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
        layers.push_back(std::move(layer));
        auto merged = co_await format::MergedLsmt::open(std::move(layers));
        std::vector<uint8_t> actual(raw.size());
        const auto read = co_await merged->pread(actual.data(), actual.size(), 0);
        REQUIRE(read == static_cast<ssize_t>(actual.size()));
        REQUIRE(actual == raw);
        auto first = co_await source::LocalFileSource::open(output);
        auto second = co_await source::LocalFileSource::open(equivalent);
        REQUIRE(first->size() == second->size());
        std::vector<uint8_t> first_bytes(first->size()), second_bytes(second->size());
        const auto first_read = co_await first->pread(first_bytes.data(), first_bytes.size(), 0);
        const auto second_read = co_await second->pread(second_bytes.data(), second_bytes.size(), 0);
        REQUIRE(first_read == static_cast<ssize_t>(first_bytes.size()));
        REQUIRE(second_read == static_cast<ssize_t>(second_bytes.size()));
        REQUIRE(first_bytes == second_bytes);
        co_return 0;
    });
    REQUIRE(result == 0);
}

TEST_CASE("format: lsmt invalid tail placement preserves an existing output",
          "[format]") {
    TempDir dir;
    const auto input = test::write_file(dir / "raw", pattern_bytes(4096));
    const std::vector<uint8_t> sentinel{1, 2, 3, 4};
    const auto output = test::write_file(dir / "output", sentinel);
    const std::vector<std::vector<format::LsmtPayloadRange>> invalid{
        {{0, 0}}, {{8, 1}}, {{7, 2}}, {{3, 2}, {4, 1}}, {{5, 1}, {1, 1}},
        {{0, UINT64_MAX}}, {{UINT64_MAX, 1}}};
    const int fd = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    for (const auto& ranges : invalid) {
        format::LsmtWriteOptions options;
        options.tail_ranges = ranges;
        REQUIRE_THROWS(format::write_lsmt_single_layer(fd, 4096, output, options));
    }
    REQUIRE(::close(fd) == 0);
    const int out = ::open(output.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(out >= 0);
    std::vector<uint8_t> actual(8);
    const auto count = ::read(out, actual.data(), actual.size());
    REQUIRE(::close(out) == 0);
    REQUIRE(count == 4);
    actual.resize(4);
    REQUIRE(actual == sentinel);
}

TEST_CASE("format: lsmt tail packing splits wire boundaries and normalizes uniform classes",
          "[format]") {
    TempDir dir;
    constexpr uint64_t limit = bytes::segment_mapping::kMaxLength;
    const auto raw = pattern_bytes((limit + 7) * 512);
    const auto input = test::write_file(dir / "raw", raw);
    const auto split_path = dir / "split.lsmt";
    const auto plain_path = dir / "plain.lsmt";
    const auto all_path = dir / "all.lsmt";
    format::LsmtWriteOptions options;
    options.uuid = "01234567-89ab-cdef-0123-456789abcdef";
    const int fd = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    try {
        options.tail_ranges = {{1, limit + 1}};
        format::write_lsmt_single_layer(fd, raw.size(), split_path, options);
        options.tail_ranges.clear();
        format::write_lsmt_single_layer(fd, raw.size(), plain_path, options);
        options.tail_ranges = {{0, 2}, {2, limit + 5}};
        format::write_lsmt_single_layer(fd, raw.size(), all_path, options);
    } catch (...) { ::close(fd); throw; }
    REQUIRE(::close(fd) == 0);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto blob = co_await source::LocalFileSource::open(split_path);
        auto layer = co_await format::LsmtLayer::open(std::move(blob));
        const auto& mappings = layer->segments();
        REQUIRE(mappings.size() == 4);
        REQUIRE(mappings[0].offset == 0);
        REQUIRE(mappings[0].moffset == 8);
        REQUIRE(mappings[1].offset == 1);
        REQUIRE(mappings[1].length == limit);
        REQUIRE(mappings[1].moffset == 14);
        REQUIRE(mappings[2].offset == limit + 1);
        REQUIRE(mappings[2].length == 1);
        REQUIRE(mappings[2].moffset == 14 + limit);
        REQUIRE(mappings[3].offset == limit + 2);
        REQUIRE(mappings[3].length == 5);
        REQUIRE(mappings[3].moffset == 9);
        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
        layers.push_back(std::move(layer));
        auto merged = co_await format::MergedLsmt::open(std::move(layers));
        std::vector<uint8_t> actual(raw.size());
        const auto read = co_await merged->pread(actual.data(), actual.size(), 0);
        REQUIRE(read == static_cast<ssize_t>(actual.size()));
        REQUIRE(actual == raw);
        auto plain = co_await source::LocalFileSource::open(plain_path);
        auto all = co_await source::LocalFileSource::open(all_path);
        REQUIRE(plain->size() == all->size());
        std::vector<uint8_t> first(65536), second(first.size());
        for (uint64_t offset = 0; offset < plain->size();) {
            const size_t count = std::min<uint64_t>(first.size(), plain->size() - offset);
            const auto one = co_await plain->pread(first.data(), count, offset);
            const auto two = co_await all->pread(second.data(), count, offset);
            REQUIRE(one == static_cast<ssize_t>(count));
            REQUIRE(two == static_cast<ssize_t>(count));
            REQUIRE(std::equal(first.begin(), first.begin() + count, second.begin()));
            offset += count;
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: merged index coalesces only compatible bounded mappings",
          "[format][lsmt][coalesce]") {
    using Mapping = bytes::segment_mapping;
    const auto mapping = [](uint64_t offset, uint32_t length, uint64_t physical,
                            bool zero = false) {
        Mapping m;
        m.offset = offset; m.length = length; m.moffset = physical;
        m.zeroed = zero;
        return m;
    };
    std::vector<Mapping> top{
        mapping(0, 2, 8), mapping(2, 2, 10), // compatible DATA
        mapping(4, 2, 30),                  // physical discontinuity
        mapping(6, 2, 40, true), mapping(8, 2, 99, true),
        mapping(12, 2, 50)};               // logical hole
    std::vector<Mapping> bottom{mapping(10, 2, 48)};
    std::vector<Mapping> merged;
    format::MergedLsmt::merge_indexes({&top, &bottom}, merged);
    REQUIRE(merged.size() == 5);
    CHECK(merged[0].length == 4);
    CHECK(merged[1].moffset == 30);
    CHECK(merged[2].zeroed);
    CHECK(merged[2].length == 4);
    CHECK(merged[3].tag == 1);
    CHECK(merged[4].tag == 0); // contiguous bytes, different layer

    top = {mapping(0, Mapping::kMaxLength - 1, 8),
           mapping(Mapping::kMaxLength - 1, 1, 8 + Mapping::kMaxLength - 1),
           mapping(Mapping::kMaxLength, 1, 8 + Mapping::kMaxLength),
           mapping(Mapping::kMaxLength + 2, 1, 8 + Mapping::kMaxLength + 1)};
    merged.clear();
    format::MergedLsmt::merge_indexes({&top}, merged);
    REQUIRE(merged.size() == 3);
    CHECK(merged[0].length == Mapping::kMaxLength);
    CHECK(merged[1].length == 1);
    CHECK(merged[2].offset == Mapping::kMaxLength + 2); // retain hole

    // The public helper appends; a pre-existing prefix is not ours to rewrite.
    top = {mapping(2, 2, 10)};
    merged = {mapping(0, 2, 8)};
    format::MergedLsmt::merge_indexes({&top}, merged);
    REQUIRE(merged.size() == 2);
    CHECK(merged[0].length == 2);
}

TEST_CASE("format: coalesced layer clipping preserves per-sector ownership",
          "[format][lsmt][coalesce]") {
    using Mapping = bytes::segment_mapping;
    // Independently resolve ownership from the original top-first layer
    // arrays. Vary holes, ZERO, physical continuity and overlap deterministically.
    for (uint64_t seed = 1; seed <= 16; ++seed) {
        uint64_t rng = seed;
        std::vector<std::vector<Mapping>> layers(3);
        for (auto& layer : layers) {
            for (uint64_t sector = 0; sector < 256;) {
                rng = rng * 6364136223846793005ULL + 1;
                const uint32_t length = static_cast<uint32_t>(
                    std::min<uint64_t>(1 + ((rng >> 32) % 7), 256 - sector));
                if (rng & 1) {
                    Mapping m;
                    m.offset = sector; m.length = length;
                    m.moffset = 8 + sector + ((rng & 4) ? 512 : 0);
                    m.zeroed = (rng & 8) != 0;
                    layer.push_back(m);
                }
                sector += length;
            }
        }
        std::vector<Mapping> merged;
        format::MergedLsmt::merge_indexes(
            {&layers[0], &layers[1], &layers[2]}, merged);
        for (uint64_t sector = 0; sector < 256; ++sector) {
            const Mapping* expected = nullptr;
            size_t expected_tag = 0;
            for (size_t tag = 0; tag < layers.size() && !expected; ++tag) {
                for (const auto& m : layers[tag]) {
                    if (sector >= m.offset && sector < m.end()) {
                        expected = &m; expected_tag = tag; break;
                    }
                }
            }
            const Mapping* actual = nullptr;
            for (const auto& m : merged) {
                CHECK(m.length <= Mapping::kMaxLength);
                if (sector >= m.offset && sector < m.end()) {
                    REQUIRE(actual == nullptr);
                    actual = &m;
                }
            }
            REQUIRE((actual != nullptr) == (expected != nullptr));
            if (!expected) continue;
            CHECK(actual->tag == expected_tag);
            CHECK(actual->zeroed == expected->zeroed);
            if (!expected->zeroed)
                CHECK(actual->moffset + sector - actual->offset ==
                      expected->moffset + sector - expected->offset);
        }
    }
}

TEST_CASE("format: contiguous tiny mappings read once with exact bytes",
          "[format][lsmt][coalesce]") {
    // A small sealed fixture with eight independently encoded sector mappings.
    // Expected payload bytes are generated independently of index merging.
    constexpr size_t index_offset = 8192;
    constexpr size_t trailer_offset = index_offset + 8 * 16;
    std::vector<uint8_t> file(trailer_offset + 4096);
    format::lsmt::HeaderTrailer ht;
    ht.flags = 7; ht.virtual_size = 4096;
    ht.index_offset = index_offset; ht.index_size = 8;
    ht.serialize(file.data()); ht.flags = 6;
    ht.serialize(file.data() + trailer_offset);
    std::vector<uint8_t> expected(4096);
    for (size_t sector = 0; sector < 8; ++sector) {
        std::fill_n(expected.data() + sector * 512, 512,
                    static_cast<uint8_t>(sector + 1));
        bytes::segment_mapping m;
        m.offset = sector; m.length = 1; m.moffset = sector + 8;
        bytes::store_segment_le(file.data() + index_offset + sector * 16, m);
    }
    std::copy(expected.begin(), expected.end(), file.begin() + 4096);
    const int result = test::run_coro([&]() -> elio::coro::task<int> {
        auto source = std::make_unique<test::VectorSource>(file);
        auto* observation = source.get();
        auto layer = co_await format::LsmtLayer::open(std::move(source));
        REQUIRE(layer->segments().size() == 8); // source index remains intact
        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
        layers.push_back(std::move(layer));
        auto merged = co_await format::MergedLsmt::open(std::move(layers));
        const auto before = observation->reads();
        std::vector<uint8_t> actual(4096);
        const auto read = co_await merged->pread(actual.data(), actual.size(), 0);
        REQUIRE(read == 4096);
        REQUIRE(actual == expected);
        REQUIRE(observation->reads() - before == 1);
        co_return 0;
    });
    REQUIRE(result == 0);
}

TEST_CASE("format: zfile partial edges cross compressed read windows",
          "[format][zfile][buffers]") {
    TempDir dir;
    const auto raw = pattern_bytes(2 * format::ZFileSource::kReadWindow + 13, 173);
    const auto input = test::write_file(dir / "window.raw", raw);
    format::ZFileWriteOptions options;
    SECTION("4 KiB LZ4 blocks") { options.block_size = 4096; }
    SECTION("64 KiB LZ4 blocks") { options.block_size = 65536; }
#if OBD_HAVE_ZSTD
    SECTION("64 KiB ZSTD blocks") {
        options.block_size = 65536;
        options.algo = format::zfile::kAlgoZstd;
    }
#endif
    const int fd = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    const auto output = dir / "window.zfile";
    try {
        format::write_zfile(fd, raw.size(), output, options);
    } catch (...) {
        ::close(fd);
        throw;
    }
    REQUIRE(::close(fd) == 0);
    const int result = test::run_coro([&]() -> elio::coro::task<int> {
        auto local = co_await source::LocalFileSource::open(output);
        auto zfile = co_await format::ZFileSource::open(std::move(local), true);
        // Random payload remains larger than one compressed window. Both
        // window reuse and partial/full decoding occur in the same request.
        const auto& jump = zfile->jump_table();
        REQUIRE(jump[jump.size() - 1] - jump[0] >
                format::ZFileSource::kReadWindow);
        constexpr size_t offset = 512;
        const size_t count = raw.size() - offset - 7;
        std::vector<uint8_t> actual(count, 0xa5);
        const auto read = co_await zfile->pread(actual.data(), count, offset);
        REQUIRE(read == static_cast<ssize_t>(count));
        REQUIRE(std::equal(actual.begin(), actual.end(), raw.begin() + offset));
        // A fresh full-block request needs no partial decode buffer.
        actual.assign(options.block_size, 0xa5);
        const auto direct = co_await zfile->pread(
            actual.data(), actual.size(), options.block_size);
        REQUIRE(direct == static_cast<ssize_t>(actual.size()));
        REQUIRE(std::equal(actual.begin(), actual.end(),
                           raw.begin() + options.block_size));
        co_return 0;
    });
    REQUIRE(result == 0);
}

TEST_CASE("format: large merged lookup preserves gaps endpoints and sparse offsets",
          "[format][lsmt][lookup]") {
    for (size_t entries : {4096u, 65535u, 65536u, 65537u, 262145u}) {
        constexpr uint64_t far = bytes::segment_mapping::kMaxOffset - 4;
        constexpr size_t index_offset = 5120;
        const size_t trailer_offset = index_offset + entries * 16;
        std::vector<uint8_t> file(trailer_offset + 4096);
        std::fill_n(file.data() + 4096, 512, 0x55);
        std::fill_n(file.data() + 4608, 512, 0xaa);
        format::lsmt::HeaderTrailer ht;
        ht.flags = 7; ht.virtual_size = (far + 3) * 512;
        ht.index_offset = index_offset; ht.index_size = entries;
        ht.serialize(file.data()); ht.flags = 6;
        ht.serialize(file.data() + trailer_offset);
        for (size_t i = 0; i < entries; ++i) {
            bytes::segment_mapping m;
            m.offset = i + 1 == entries ? far : i * 3;
            m.length = 1 + i % 2;
            m.moffset = 8;
            m.zeroed = i % 7 == 0;
            if (i + 1 == entries) { m.length = 1; m.zeroed = false; }
            bytes::store_segment_le(file.data() + index_offset + i * 16, m);
        }
        const int result = test::run_coro([&]() -> elio::coro::task<int> {
            auto layer = co_await format::LsmtLayer::open(
                std::make_unique<test::VectorSource>(file));
            std::vector<std::unique_ptr<format::LsmtLayer>> layers;
            layers.push_back(std::move(layer));
            auto merged = co_await format::MergedLsmt::open(std::move(layers));
            REQUIRE(merged->merged_index().size() == entries);
            std::vector<uint64_t> queries{0, 1, 63 * 3, 64 * 3 - 1,
                64 * 3, 65 * 3, (entries - 2) * 3, (entries - 1) * 3,
                far - 1, far, far + 1, far + 3};
            uint64_t random = 173;
            for (unsigned i = 0; i < 2000; ++i) {
                random = random * 6364136223846793005ULL + 1;
                queries.push_back((random >> 32) % (entries * 3));
            }
            for (uint64_t sector : queries) {
                std::array<uint8_t, 4096> actual;
                actual.fill(0xa5);
                const size_t count = std::min<uint64_t>(8, far + 3 - sector) * 512;
                const auto read = co_await merged->pread(
                    actual.data(), count, sector * 512);
                REQUIRE(read == static_cast<ssize_t>(count));
                for (size_t byte = 0; byte < count; ++byte) {
                    const uint64_t current = sector + byte / 512;
                    uint8_t expected = 0;
                    if (current == far) expected = 0x55;
                    else if (current % 3 < 1 + (current / 3) % 2 &&
                             current / 3 < entries - 1 &&
                             (current / 3) % 7 != 0)
                        expected = current % 3 ? 0xaa : 0x55;
                    if (actual[byte] != expected) {
                        INFO("entries=" << entries << " sector=" << current);
                        REQUIRE(actual[byte] == expected);
                    }
                }
            }
            co_return 0;
        });
        REQUIRE(result == 0);
    }
}
