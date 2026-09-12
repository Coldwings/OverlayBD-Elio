#include "format/lsmt.hpp"
#include "common/errors.hpp"
#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <array>
#include <algorithm>
#include <limits>

using namespace obd;
namespace {
// Independent literal words from upstream's packed SegmentMapping layout:
// logical 0,len1 -> metadata sector8, logical1,len2 -> target sector0.
using Record = std::array<uint64_t, 2>;
std::vector<uint8_t> fixture(std::vector<Record> records) {
    constexpr size_t index = 4608;
    std::vector<uint8_t> data(index + records.size() * 16 + 4096);
    const uint8_t magic[] = {0x4c,0x53,0x4d,0x54,0,1,2,0,
        0x65,0x7e,0x63,0xd2,0x94,0x44,8,0x4c,0xa2,0xd2,0xc8,0xec,0x4f,0xcf,0xae,0x8a};
    for (auto off : {size_t(0), data.size() - 4096}) {
        std::copy(std::begin(magic), std::end(magic), data.begin() + off);
        bytes::store_u32_le(data.data() + off + 24, 390);
        bytes::store_u32_le(data.data() + off + 28, off == 0 ? 3 : 6);
        bytes::store_u64_le(data.data() + off + 32, index);
        bytes::store_u64_le(data.data() + off + 40, records.size());
        bytes::store_u64_le(data.data() + off + 48, 4 * 512);
        data[off + 132] = data[off + 133] = 1;
    }
    std::fill(data.begin() + 4096, data.begin() + index, 0x4d);
    for (size_t i = 0; i < records.size(); ++i) {
        bytes::store_u64_le(data.data() + index + i * 16, records[i][0]);
        bytes::store_u64_le(data.data() + index + i * 16 + 8, records[i][1]);
    }
    return data;
}
std::vector<uint8_t> fixture() {
    return fixture({{0x0004000000000000ULL, 0x0700000000000008ULL},
                    {0x0008000000000001ULL, 0x0800000000000000ULL}});
}
source::BlobSourcePtr mem(std::vector<uint8_t> data) {
    return std::make_unique<test::VectorSource>(std::move(data));
}
}

TEST_CASE("format: native warp golden tags dispatch metadata and target", "[format][warp]") {
    const auto result = test::run_coro([]() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtLayer::open_warp(
            mem(fixture()), mem(std::vector<uint8_t>(1024, 0x54)));
        REQUIRE(layer->segments()[0].tag == 0);
        REQUIRE(layer->segments()[1].tag == 0);
        std::vector<std::unique_ptr<format::LsmtLayer>> stack;
        stack.push_back(std::move(layer));
        auto merged = co_await format::MergedLsmt::open(std::move(stack));
        std::vector<uint8_t> out(2048, 0xff);
        const auto n = co_await merged->pread(out.data(), out.size(), 0);
        REQUIRE(n == 2048);
        REQUIRE(std::all_of(out.begin(), out.begin()+512, [](auto c){return c==0x4d;}));
        REQUIRE(std::all_of(out.begin()+512, out.begin()+1536, [](auto c){return c==0x54;}));
        REQUIRE(std::all_of(out.begin()+1536, out.end(), [](auto c){return c==0;}));
        co_return 0;
    });
    REQUIRE(result == 0);
}

TEST_CASE("format: native warp offsets survive layer merge clipping", "[format][warp]") {
    const auto result = test::run_coro([]() -> elio::coro::task<int> {
        std::vector<uint8_t> target(1024, 0x54);
        std::fill(target.begin()+512, target.end(), 0x55);
        std::vector<std::unique_ptr<format::LsmtLayer>> stack;
        stack.push_back(co_await format::LsmtLayer::open_warp(mem(fixture()), mem(target)));
        // Ordinary top layer replaces logical sector1 with metadata bytes.
        auto upper = fixture({{0x0004000000000001ULL, 8}});
        stack.push_back(co_await format::LsmtLayer::open(mem(std::move(upper))));
        auto merged = co_await format::MergedLsmt::open(std::move(stack));
        std::vector<uint8_t> out(1536);
        const auto n = co_await merged->pread(out.data(), out.size(), 0);
        REQUIRE(n == 1536);
        REQUIRE(out[0] == 0x4d);
        REQUIRE(out[512] == 0x4d);
        REQUIRE(out[1024] == 0x55);
        REQUIRE(merged->merged_index().back().tag == 1);
        co_return 0;
    });
    REQUIRE(result == 0);
}

TEST_CASE("format: native warp singleton tags normalize to metadata", "[format][warp]") {
    const auto result = test::run_coro([]() -> elio::coro::task<int> {
        auto data = fixture({
            {0x0004000000000000ULL, 0xff00000000000008ULL},
            // INVALID_OFFSET padding must not participate in minimum tag.
            {0x0003ffffffffffffULL, 0}});
        auto layer = co_await format::LsmtLayer::open_warp(
            mem(std::move(data)), mem(std::vector<uint8_t>()));
        REQUIRE(layer->segments().size() == 1);
        REQUIRE(layer->segments()[0].moffset == 8);
        co_return 0;
    });
    REQUIRE(result == 0);
}

TEST_CASE("format: native warp rejects malformed tags and extents", "[format][warp]") {
    std::vector<Record> records = {
        {0x0004000000000000ULL, 0x0700000000000008ULL},
        {0x0008000000000001ULL, 0x0800000000000000ULL}};
    size_t target_size = 1024;
    SECTION("tag gap") { records[1][1] = 0x0900000000000000ULL; }
    SECTION("target short by a byte") { target_size = 1023; }
    SECTION("target huge mapped offset") { records[1][1] = 0x087fffffffffffffULL; }
    SECTION("metadata points into header") { records[0][1] = 0x0700000000000007ULL; }
    SECTION("metadata points into index") { records[0][1] = 0x0700000000000009ULL; }
    SECTION("overlap") { records[1][0] = 0x0008000000000000ULL; }
    SECTION("zero length") { records[1][0] = 1; }
    SECTION("past virtual size") { records[1][0] = 0x0008000000000003ULL; }
    const auto open_invalid = [&]() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtLayer::open_warp(
            mem(fixture(records)), mem(std::vector<uint8_t>(target_size)));
        (void)layer;
        co_return 0;
    };
    REQUIRE_THROWS_AS(test::run_coro(open_invalid), format_error);
}

TEST_CASE("format: native warp zero extents mask target bytes", "[format][warp]") {
    const auto result = test::run_coro([]() -> elio::coro::task<int> {
        auto data = fixture({{0x0004000000000000ULL, 0x0700000000000008ULL},
                             {0x0008000000000001ULL, 0x0880000000000000ULL}});
        std::vector<std::unique_ptr<format::LsmtLayer>> stack;
        stack.push_back(co_await format::LsmtLayer::open_warp(mem(data), mem(std::vector<uint8_t>())));
        auto merged = co_await format::MergedLsmt::open(std::move(stack));
        std::vector<uint8_t> out(1024, 0xff);
        const auto n = co_await merged->pread(out.data(), out.size(), 512);
        REQUIRE(n == 1024);
        REQUIRE(std::all_of(out.begin(), out.end(), [](auto c){return c==0;}));
        co_return 0;
    });
    REQUIRE(result == 0);
}

TEST_CASE("format: native warp rejects combined source size overflow", "[format][warp]") {
    class HugeSource final : public source::BlobSource {
    public:
        uint64_t size() const noexcept override {
            return std::numeric_limits<uint64_t>::max();
        }
        std::string_view label() const noexcept override { return "huge"; }
        elio::coro::task<ssize_t> pread(void*, size_t, uint64_t) override {
            co_return -EIO;
        }
    };
    const auto open_invalid = []() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtLayer::open_warp(
            mem(fixture()), std::make_unique<HugeSource>());
        (void)layer;
        co_return 0;
    };
    REQUIRE_THROWS_AS(test::run_coro(open_invalid), format_error);
}
