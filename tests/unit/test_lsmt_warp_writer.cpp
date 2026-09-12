#include "format/writer.hpp"
#include "format/lsmt.hpp"
#include "common/errors.hpp"
#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <fstream>
#include <iterator>

using namespace obd;
namespace {
struct Input {
    int fd;
    explicit Input(const std::string& path) : fd(::open(path.c_str(), O_RDONLY)) {
        if (fd < 0) throw_errno(errno, "open test input");
    }
    ~Input() { ::close(fd); }
};
std::vector<uint8_t> load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}
format::LsmtWriteOptions options() {
    format::LsmtWriteOptions opts;
    opts.uuid = "11111111-2222-3333-4444-555555555555";
    return opts;
}
}

TEST_CASE("format: warp writer compacts metadata with golden wire words", "[format][warp]") {
    test::TempDir dir;
    std::vector<uint8_t> filesystem(4096, 0xee);
    std::fill(filesystem.begin()+1024, filesystem.begin()+1536, 0x4d);
    const auto raw = test::write_file(dir / "raw", filesystem);
    Input input(raw);
    const std::vector<bytes::segment_mapping> mappings = {
        {0, 1, 2, false, 0}, {1, 2, 3, false, 1}, {3, 1, 0, true, 0}};
    format::write_lsmt_warp_layer(input.fd, 2048, mappings, dir / "warp", options());
    format::write_lsmt_warp_layer(input.fd, 2048, mappings, dir / "warp2", options());
    const auto blob = load(dir / "warp");
    REQUIRE(blob == load(dir / "warp2"));
    REQUIRE(blob.size() == 4096 + 512 + 48 + 4096);
    REQUIRE(bytes::load_u32_le(blob.data()+28) == 7);
    REQUIRE(bytes::load_u32_le(blob.data()+blob.size()-4096+28) == 6);
    REQUIRE(bytes::load_u64_le(blob.data()+32) == 4608);
    REQUIRE(bytes::load_u64_le(blob.data()+40) == 3);
    REQUIRE(bytes::load_u64_le(blob.data()+48) == 2048);
    // Literal independently specified wire words, not store_segment_le output.
    REQUIRE(bytes::load_u64_le(blob.data()+4608) == 0x0004000000000000ULL);
    REQUIRE(bytes::load_u64_le(blob.data()+4616) == 0x0000000000000008ULL);
    REQUIRE(bytes::load_u64_le(blob.data()+4624) == 0x0008000000000001ULL);
    REQUIRE(bytes::load_u64_le(blob.data()+4632) == 0x0100000000000003ULL);
    REQUIRE(bytes::load_u64_le(blob.data()+4648) == 0x0080000000000009ULL);
    REQUIRE(std::all_of(blob.begin()+4096, blob.begin()+4608, [](auto c){return c==0x4d;}));
    REQUIRE(test::run_coro([&]() -> elio::coro::task<int> {
        std::vector<uint8_t> target(2560, 0x54);
        std::vector<std::unique_ptr<format::LsmtLayer>> stack;
        stack.push_back(co_await format::LsmtLayer::open_warp(
            std::make_unique<test::VectorSource>(blob),
            std::make_unique<test::VectorSource>(target)));
        auto merged = co_await format::MergedLsmt::open(std::move(stack));
        std::vector<uint8_t> out(2048, 0xff);
        const auto n = co_await merged->pread(out.data(), out.size(), 0);
        REQUIRE(n == 2048);
        REQUIRE(std::all_of(out.begin(), out.begin()+512, [](auto c){return c==0x4d;}));
        REQUIRE(std::all_of(out.begin()+512, out.begin()+1536, [](auto c){return c==0x54;}));
        REQUIRE(std::all_of(out.begin()+1536, out.end(), [](auto c){return c==0;}));
        co_return 0;
    }) == 0);
}

TEST_CASE("format: warp writer validates before output mutation", "[format][warp]") {
    test::TempDir dir;
    Input input(test::write_file(dir / "raw", std::vector<uint8_t>(1024)));
    std::vector<bytes::segment_mapping> mappings = {{0, 1, 0, false, 0}, {1, 1, 2, false, 1}};
    auto opts = options();
    uint64_t virtual_size = 1024;
    SECTION("unknown tag") { mappings[1].tag = 2; }
    SECTION("remote only ambiguous normalization") { mappings.erase(mappings.begin()); }
    SECTION("overlap") { mappings[1].offset = 0; }
    SECTION("zero length") { mappings[0].length = 0; }
    SECTION("length cannot be encoded") { mappings[0].length = 16384; }
    SECTION("metadata outside file") { mappings[0].moffset = 2; }
    SECTION("logical outside device") { mappings[1].offset = 2; }
    SECTION("mapped arithmetic cannot be encoded") { mappings[1].moffset = bytes::segment_mapping::kMaxMoffset; }
    SECTION("virtual alignment") { virtual_size = 1023; }
    SECTION("invalid uuid length") { opts.uuid = std::string(100, 'x'); }
    const std::vector<uint8_t> sentinel = {1,2,3,4};
    test::write_file(dir / "out", sentinel);
    REQUIRE_THROWS_AS(format::write_lsmt_warp_layer(input.fd, virtual_size, mappings, dir / "out", opts), format_error);
    REQUIRE(load(dir / "out") == sentinel);
}

TEST_CASE("format: warp writer rejects metadata output aliases", "[format][warp]") {
    test::TempDir dir;
    const std::vector<uint8_t> data(512, 0x42);
    const auto raw = test::write_file(dir / "raw", data);
    Input input(raw);
    const std::vector<bytes::segment_mapping> mappings = {{0,1,0,false,0}};
    REQUIRE_THROWS_AS(format::write_lsmt_warp_layer(input.fd, 512, mappings, raw, options()), format_error);
    REQUIRE(load(raw) == data);
}

TEST_CASE("format: warp writer zero metadata anchors remote normalization", "[format][warp]") {
    test::TempDir dir;
    Input input(test::write_file(dir / "raw", std::vector<uint8_t>()));
    const std::vector<bytes::segment_mapping> mappings = {{0,1,0,true,0}, {1,1,0,false,1}};
    format::write_lsmt_warp_layer(input.fd, 1024, mappings, dir / "out", options());
    const auto blob = load(dir / "out");
    REQUIRE(blob.size() == 4096 + 32 + 4096);
    REQUIRE(bytes::load_u64_le(blob.data()+32) == 4096);
    REQUIRE(bytes::load_u64_le(blob.data()+4104) == 0x0080000000000008ULL);
    REQUIRE(bytes::load_u64_le(blob.data()+4120) == 0x0100000000000000ULL);
}
