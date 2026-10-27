#include "format/lsmt_rw_index.hpp"
#include "common/errors.hpp"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace obd;
namespace {
std::vector<uint8_t> read_fixture(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::array<uint8_t, 16> record(uint64_t offset, uint32_t length,
                               uint64_t physical, bool zero = false) {
    bytes::segment_mapping mapping;
    mapping.offset = offset;
    mapping.length = length;
    mapping.moffset = physical;
    mapping.zeroed = zero;
    mapping.tag = 23;  // Runtime tags must not survive replay.
    std::array<uint8_t, 16> raw{};
    bytes::store_segment_le(raw.data(), mapping);
    return raw;
}
}

TEST_CASE("format: LSMT writable replay matches upstream append and buffered fixtures",
          "[format][lsmt-rw-index]") {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      "fixtures/lsmt/overlaybd";
    for (const auto* name : {"writable", "buffered"}) {
        DYNAMIC_SECTION(name) {
            const auto data = read_fixture(root / (std::string(name) + ".data"));
            const auto index = read_fixture(root / (std::string(name) + ".index"));
            const auto dh = format::lsmt::HeaderTrailer::parse(data.data());
            const auto ih = format::lsmt::HeaderTrailer::parse(index.data());
            const auto count = format::lsmt_rw::validate_pair(dh, ih, data.size(), index.size());
            REQUIRE(count == (index.size() - 4096) / 16);
            REQUIRE(dh.flags == 3);
            REQUIRE(ih.flags == 1);
            format::lsmt_rw::Replay replay(dh.virtual_size, data.size());
            for (size_t pos = 4096; pos < index.size(); pos += 16)
                replay.append(std::span(index.data() + pos, 16));
            std::vector<uint8_t> actual(32768, 0), states(64, 0);
            for (const auto& m : replay.finish()) {
                REQUIRE(m.tag == 0);
                for (uint64_t sector = m.offset; sector < m.end(); ++sector) {
                    states.at(sector) = m.zeroed ? 2 : 1;
                    if (!m.zeroed) {
                        const auto start = (m.moffset + sector - m.offset) * 512;
                        std::copy_n(data.begin() + static_cast<ptrdiff_t>(start), 512,
                                    actual.begin() + static_cast<ptrdiff_t>(sector * 512));
                    }
                }
            }
            REQUIRE(actual == read_fixture(root / "expected.raw"));
            REQUIRE(states == read_fixture(root / "expected.states"));
            auto wrong = ih;
            wrong.uuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
            REQUIRE_THROWS_AS(format::lsmt_rw::validate_pair(dh, wrong, data.size(), index.size()), format_error);
            wrong = ih;
            wrong.virtual_size += 512;
            REQUIRE_THROWS_AS(format::lsmt_rw::validate_pair(dh, wrong, data.size(), index.size()), format_error);
            REQUIRE_THROWS_AS(format::lsmt_rw::validate_pair(dh, ih, data.size(), index.size() - 1), format_error);
        }
    }
}

TEST_CASE("format: LSMT writable replay preserves ZERO splits and rejects DATA aliasing",
          "[format][lsmt-rw-index]") {
    using format::lsmt_rw::Replay;
    SECTION("later updates split prior ownership in log order") {
        Replay replay(32768, 16384);
        replay.append(record(0, 8, 8));
        replay.append(record(2, 4, bytes::segment_mapping::kMaxMoffset, true));
        replay.append(record(3, 1, 20));
        const auto mappings = replay.finish();
        REQUIRE(mappings.size() == 5);
        REQUIRE(mappings[0].length == 2);
        REQUIRE(mappings[1].zeroed);
        REQUIRE(mappings[2].moffset == 20);
        REQUIRE(mappings[3].zeroed);
        REQUIRE(mappings[3].length == 2);
        REQUIRE(mappings[4].moffset == 14);
        REQUIRE(mappings[4].offset == 6);
    }
    SECTION("live DATA aliases cannot be opened for in-place mutation") {
        Replay replay(32768, 16384);
        replay.append(record(0, 4, 8));
        replay.append(record(8, 1, 10));
        REQUIRE_THROWS_AS(replay.finish(), format_error);
        // An obsolete alias does not invalidate the final live ownership.
        replay.append(record(8, 1, 31));
        REQUIRE(replay.finish().size() == 2);
    }
    SECTION("physical ZERO placeholders never become DATA ownership") {
        Replay replay(32768, 8192);
        replay.append(record(0, 8, 8));
        replay.append(record(20, 8, 8, true));
        const auto mappings = replay.finish();
        REQUIRE(mappings.size() == 2);
        REQUIRE(mappings[1].zeroed);
    }
    SECTION("logical and physical bounds and incomplete records fail closed") {
        Replay replay(32768, 8192);
        REQUIRE_THROWS_AS(replay.append(record(64, 1, 8)), format_error);
        REQUIRE_THROWS_AS(replay.append(record(0, 9, 8)), format_error);
        REQUIRE_THROWS_AS(replay.append(record(0, 0, 8)), format_error);
        REQUIRE_THROWS_AS(replay.append(record(0, 1, 7)), format_error);
        std::array<uint8_t, 15> partial{};
        REQUIRE_THROWS_AS(replay.append(partial), format_error);
    }
}
