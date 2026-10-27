#include "format/hybrid_format.hpp"
#include "common/errors.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace obd;
namespace {
std::vector<uint8_t> fixture_bytes(const std::filesystem::path& p) {
    std::ifstream stream(p, std::ios::binary);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::array<uint8_t, 16> record(bytes::segment_mapping m) {
    std::array<uint8_t, 16> raw{};
    bytes::store_segment_le(raw.data(), m);
    return raw;
}
}

TEST_CASE("format: hybrid replay imports reference logs without reusing ZERO aliases",
          "[format][hybrid]") {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      "fixtures/hybrid";
    for (const auto* producer : {"overlaybd", "agentenv"}) {
        for (const auto* name : {"writable", "buffered"}) {
            DYNAMIC_SECTION(producer << "/" << name) {
                const auto directory = root / producer;
                const auto data = fixture_bytes(directory / (std::string(name) + ".data"));
                const auto index = fixture_bytes(directory / (std::string(name) + ".index"));
                const auto dh = format::lsmt::HeaderTrailer::parse(data.data());
                const auto ih = format::lsmt::HeaderTrailer::parse(index.data());
                const auto count = format::hybrid::validate_pair(dh, ih, data.size(), index.size());
                REQUIRE(count == (index.size() - 4096) / 16);
                format::hybrid::Replay replay(dh.virtual_size, data.size());
                // Exercise streaming boundaries: one record per chunk, including padding.
                for (size_t pos = 4096; pos < index.size(); pos += 16) {
                    replay.append(std::span(index.data() + pos, 16));
                }
                const auto mappings = replay.finish();
                std::vector<uint8_t> actual(32768, 0), states(64, 0);
                bool retained_zero = false;
                for (const auto& m : mappings) {
                    REQUIRE(m.end() <= states.size());
                    REQUIRE(m.tag == 0);
                    if (m.zeroed && m.offset == 10) {
                        retained_zero = m.moffset != format::hybrid::kNoPhysicalOffset;
                    }
                    for (uint64_t s = m.offset; s < m.end(); ++s) {
                        states[s] = m.zeroed ? 2 : 1;
                        if (!m.zeroed) {
                            const auto off = (m.moffset + s - m.offset) * 512;
                            REQUIRE(off + 512 <= data.size());
                            std::copy_n(data.begin() + static_cast<ptrdiff_t>(off), 512,
                                        actual.begin() + static_cast<ptrdiff_t>(s * 512));
                        }
                    }
                }
                REQUIRE(actual == fixture_bytes(directory / "expected.raw"));
                REQUIRE(states == fixture_bytes(directory / "expected.states"));
                REQUIRE(retained_zero == (std::string(producer) == "agentenv"));
            }
        }
    }
}

TEST_CASE("format: hybrid replay rejects invalid bounds and normalizes overlapping ZERO backing",
          "[format][hybrid]") {
    using format::hybrid::Replay;
    SECTION("partial record") {
        Replay replay(32768, 8192);
        std::array<uint8_t, 15> partial{};
        REQUIRE_THROWS_AS(replay.append(partial), format_error);
    }
    SECTION("only canonical padding is skipped") {
        Replay replay(32768, 8192);
        replay.append(record({bytes::segment_mapping::kInvalidOffset, 0, 0, false, 0}));
        REQUIRE(replay.finish().empty());
        REQUIRE_THROWS_AS(replay.append(record(
            {bytes::segment_mapping::kInvalidOffset, 0, 8, false, 0})), format_error);
    }
    SECTION("data aliases") {
        Replay replay(32768, 8192);
        replay.append(record({1, 2, 8, false, 0}));
        replay.append(record({8, 1, 9, false, 0}));
        REQUIRE_THROWS_AS(replay.finish(), format_error);
    }
    SECTION("zero aliases do not become reusable") {
        Replay replay(32768, 8192);
        replay.append(record({1, 2, 8, true, 0}));
        replay.append(record({8, 1, 9, true, 0}));
        const auto maps = replay.finish();
        REQUIRE(maps.size() == 2);
        REQUIRE(maps[0].moffset == format::hybrid::kNoPhysicalOffset);
        REQUIRE(maps[1].moffset == format::hybrid::kNoPhysicalOffset);
    }
    SECTION("replacement splits retain correct physical offsets") {
        Replay replay(32768, 16384);
        replay.append(record({1, 6, 8, false, 0}));
        replay.append(record({3, 2, 20, false, 0}));
        const auto maps = replay.finish();
        REQUIRE(maps.size() == 3);
        REQUIRE(maps[0].length == 2);
        REQUIRE(maps[1].moffset == 20);
        REQUIRE(maps[2].offset == 5);
        REQUIRE(maps[2].moffset == 12);
    }
    SECTION("logical and physical ranges") {
        for (const auto& m : {bytes::segment_mapping{63, 2, 8, false, 0},
                              bytes::segment_mapping{1, 1, 16, false, 0},
                              bytes::segment_mapping{1, 0, 8, false, 0}}) {
            Replay replay(32768, 8192);
            REQUIRE_THROWS_AS(replay.append(record(m)), format_error);
        }
    }
}

TEST_CASE("format: hybrid pair validation rejects identity and size errors before replay",
          "[format][hybrid]") {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      "fixtures/hybrid/overlaybd";
    const auto data = fixture_bytes(root / "writable.data");
    const auto index = fixture_bytes(root / "writable.index");
    const auto dh = format::lsmt::HeaderTrailer::parse(data.data());
    const auto ih = format::lsmt::HeaderTrailer::parse(index.data());
    using format::hybrid::validate_pair;
    SECTION("mismatched UUID") {
        auto other = ih;
        other.uuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
        REQUIRE_THROWS_AS(validate_pair(dh, other, data.size(), index.size()), format_error);
    }
    SECTION("mismatched virtual size") {
        auto other = ih;
        other.virtual_size += 512;
        REQUIRE_THROWS_AS(validate_pair(dh, other, data.size(), index.size()), format_error);
    }
    SECTION("mismatched parent") {
        auto other = ih;
        other.parent_uuid.clear();
        REQUIRE_THROWS_AS(validate_pair(dh, other, data.size(), index.size()), format_error);
    }
    SECTION("unknown and conflicting flags") {
        for (const auto flag : {1u << 4, 1u << 6, 1u << 31}) {
            auto other = dh;
            other.flags |= flag;
            REQUIRE_THROWS_AS(validate_pair(other, ih, data.size(), index.size()), format_error);
        }
    }
    SECTION("unsupported version") {
        auto other = dh;
        other.version = 2;
        REQUIRE_THROWS_AS(validate_pair(other, ih, data.size(), index.size()), format_error);
    }
    SECTION("truncated index and short data") {
        REQUIRE_THROWS_AS(validate_pair(dh, ih, data.size(), index.size() - 1), format_error);
        REQUIRE_THROWS_AS(validate_pair(dh, ih, 4095, index.size()), format_error);
        REQUIRE_THROWS_AS(validate_pair(dh, ih, data.size(), 4095), format_error);
    }
    SECTION("replay limit without metadata allocation") {
        const uint64_t max_bytes = 4096 + format::hybrid::kMaxLogRecords * 16;
        REQUIRE(validate_pair(dh, ih, data.size(), max_bytes) == format::hybrid::kMaxLogRecords);
        REQUIRE_THROWS_AS(validate_pair(dh, ih, data.size(), max_bytes + 16), format_error);
    }
    SECTION("invalid physical marker") {
        format::hybrid::Replay replay(32768, 8192);
        REQUIRE_THROWS_AS(replay.append(record({1, 1, 7, true, 0})), format_error);
    }
}
