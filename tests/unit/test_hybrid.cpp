// External Hybrid fixtures are produced by pinned, unmodified reference writers.
#include "format/lsmt.hpp"
#include "common/errors.hpp"
#include "source/local_file.hpp"
#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

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
}

TEST_CASE("format: hybrid sealed reference fixtures preserve bytes and zero masks",
          "[format][hybrid]") {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      "fixtures/hybrid";
    for (const auto* producer : {"overlaybd", "agentenv"}) {
        DYNAMIC_SECTION(producer) {
            const auto fixture = root / producer;
            const auto expected = read_fixture(fixture / "expected.raw");
            const auto expected_states = read_fixture(fixture / "expected.states");
            REQUIRE(expected.size() == 32768);
            REQUIRE(expected_states.size() == 64);
            const int rc = test::run_coro([&]() -> elio::coro::task<int> {
                auto local = co_await source::LocalFileSource::open(
                    (fixture / "sealed.data").string());
                auto layer = co_await format::LsmtLayer::open(std::move(local));
                REQUIRE(layer->virtual_size() == expected.size());
                std::vector<uint8_t> states(64, 0);
                for (const auto& mapping : layer->segments()) {
                    REQUIRE(mapping.length > 0);
                    REQUIRE(mapping.end() <= states.size());
                    for (uint64_t s = mapping.offset; s < mapping.end(); ++s) {
                        states[s] = mapping.zeroed ? 2 : 1;
                    }
                }
                REQUIRE(states == expected_states);
                std::vector<std::unique_ptr<format::LsmtLayer>> layers;
                layers.push_back(std::move(layer));
                auto merged = co_await format::MergedLsmt::open(std::move(layers));
                std::vector<uint8_t> actual(expected.size());
                const auto count = co_await merged->pread(actual.data(), actual.size(), 0);
                REQUIRE(count == static_cast<ssize_t>(actual.size()));
                REQUIRE(actual == expected);
                co_return 0;
            });
            REQUIRE(rc == 0);
        }
    }
}

TEST_CASE("format: hybrid ZERO sentinel does not bypass DATA or layout validation",
          "[format][hybrid]") {
    const auto fixture = std::filesystem::path(__FILE__).parent_path().parent_path() /
                         "fixtures/hybrid/agentenv/sealed.data";
    for (const auto* mutation : {"ordinary", "data", "unknown flag", "flag mismatch", "logical range"}) {
        DYNAMIC_SECTION(mutation) {
            auto raw = read_fixture(fixture);
            const size_t trailer = raw.size() - 4096;
            const auto index = bytes::load_u64_le(raw.data() + trailer + 32);
            const auto count = bytes::load_u64_le(raw.data() + trailer + 40);
            const std::string change(mutation);
            if (change == "ordinary") {
                for (const size_t offset : {size_t(0), trailer}) {
                    const auto flags = bytes::load_u32_le(raw.data() + offset + 28);
                    bytes::store_u32_le(raw.data() + offset + 28, flags & ~(1u << 5));
                }
            } else if (change == "unknown flag") {
                auto flags = bytes::load_u32_le(raw.data() + trailer + 28);
                bytes::store_u32_le(raw.data() + trailer + 28, flags | (1u << 6));
            } else if (change == "flag mismatch") {
                auto flags = bytes::load_u32_le(raw.data() + trailer + 28);
                bytes::store_u32_le(raw.data() + trailer + 28, flags & ~(1u << 5));
            } else {
                bool changed = false;
                for (uint64_t i = 0; i < count; ++i) {
                    auto* record = raw.data() + index + i * 16;
                    auto m = bytes::load_segment_le(record);
                    if (m.zeroed) {
                        if (change == "data") m.zeroed = false;
                        else m.offset = 64;
                        bytes::store_segment_le(record, m);
                        changed = true;
                        break;
                    }
                }
                REQUIRE(changed);
            }
            test::TempDir temporary;
            const auto path = test::write_file(temporary / "invalid.data", raw);
            const bool rejected = test::run_coro([&]() -> elio::coro::task<bool> {
                auto local = co_await source::LocalFileSource::open(path);
                try {
                    auto layer = co_await format::LsmtLayer::open(std::move(local));
                    (void)layer;
                } catch (const format_error&) {
                    co_return true;
                }
                co_return false;
            });
            REQUIRE(rejected);
        }
    }
}
