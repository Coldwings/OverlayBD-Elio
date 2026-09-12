#include "../../tools/gzip_index_builder.hpp"
#include "source/gzip_index_source.hpp"
#include "../support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iterator>
#include <zlib.h>

namespace {
std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
std::vector<uint8_t> gzip(const std::vector<uint8_t>& plain) {
    std::vector<uint8_t> out(compressBound(plain.size())+100);
    z_stream z{};
    REQUIRE(deflateInit2(&z,6,Z_DEFLATED,31,8,Z_DEFAULT_STRATEGY)==Z_OK);
    z.next_in=const_cast<uint8_t*>(plain.data()); z.avail_in=plain.size();
    z.next_out=out.data(); z.avail_out=out.size();
    REQUIRE(deflate(&z,Z_FINISH)==Z_STREAM_END);
    out.resize(z.total_out); deflateEnd(&z);
    return out;
}
uint64_t le(const std::vector<uint8_t>& v,size_t at,size_t n) {
    uint64_t result=0;
    for(size_t i=0;i<n;++i) result|=uint64_t(v[at+i])<<(8*i);
    return result;
}
}

TEST_CASE("format: gzip index builder is deterministic and readable", "[convert][gzip]") {
    obd::test::TempDir dir;
    auto plain=obd::test::pattern_bytes(2*1024*1024,53);
    auto compressed=gzip(plain);
    obd::test::write_file(dir/"input.gz",compressed);
    obd::convert::build_gzip_index(dir/"input.gz",dir/"one.idx",65536);
    obd::convert::build_gzip_index(dir/"input.gz",dir/"two.idx",65536);
    auto index=read_file(dir/"one.idx");
    REQUIRE(index==read_file(dir/"two.idx"));
    REQUIRE(std::string(reinterpret_cast<char*>(index.data()),7)=="ddgzidx");
    REQUIRE(index[8]==1);
    REQUIRE(index[10]==1);
    REQUIRE(le(index,17,4)==32768);
    REQUIRE(le(index,21,4)==29);
    REQUIRE(le(index,33,8)==compressed.size());
    REQUIRE(le(index,41,8)==index.size());
    REQUIRE(le(index,49,8)==plain.size());
    REQUIRE(le(index,313,8)+le(index,321,8)==index.size());
    const auto result = obd::test::run_coro([&]() -> elio::coro::task<int> {
        auto source=co_await obd::source::GzipIndexSource::open(
            std::make_unique<obd::test::VectorSource>(compressed),
            std::make_unique<obd::test::VectorSource>(index));
        for(uint64_t offset : {uint64_t(0),uint64_t(65530),uint64_t(1500000)}) {
            std::vector<uint8_t> out(8192);
            const auto read_count_1 = co_await source->pread(out.data(),out.size(),offset);
            REQUIRE(read_count_1 == ssize_t(out.size()));
            REQUIRE(std::equal(out.begin(),out.end(),plain.begin()+offset));
        }
        co_return 0;
    });
    REQUIRE(result == 0);
}

TEST_CASE("format: invalid gzip never publishes partial index", "[convert][gzip]") {
    obd::test::TempDir dir;
    const std::vector<uint8_t> previous{'k','e','e','p'};
    for(int mutation=0;mutation<3;++mutation) {
        auto data=gzip(std::vector<uint8_t>(10000,'x'));
        if(mutation==0) data.resize(data.size()-3);
        if(mutation==1) data[data.size()-8]^=1;
        if(mutation==2) data.push_back(0);
        obd::test::write_file(dir/"input.gz",data);
        obd::test::write_file(dir/"existing.idx",previous);
        REQUIRE_THROWS(obd::convert::build_gzip_index(dir/"input.gz",dir/"existing.idx"));
        REQUIRE(read_file(dir/"existing.idx")==previous);
        REQUIRE_THROWS(obd::convert::build_gzip_index(dir/"input.gz",dir/"absent.idx"));
        REQUIRE_FALSE(std::filesystem::exists(dir/"absent.idx"));
        for(const auto& entry:std::filesystem::directory_iterator(dir.path()))
            REQUIRE(entry.path().filename().string().find(".tmp.")==std::string::npos);
    }
}

TEST_CASE("format: gzip index output cannot replace its input", "[convert][gzip]") {
    obd::test::TempDir dir;
    const auto bytes = gzip(std::vector<uint8_t>(2048, 42));
    const auto path = obd::test::write_file(dir / "input.gz", bytes);
    REQUIRE_THROWS(obd::convert::build_gzip_index(path, path));
    REQUIRE(read_file(path) == bytes);
}

TEST_CASE("format: gzip index records the initial checkpoint for one final block", "[convert][gzip]") {
    obd::test::TempDir dir;
    const auto plain=obd::test::pattern_bytes(1024,71);
    // Handwritten gzip framing with exactly one final, uncompressed DEFLATE
    // block (BFINAL=1, BTYPE=00). No compressor-dependent block splitting.
    std::vector<uint8_t> compressed{0x1f,0x8b,8,0,0,0,0,0,0,255,1};
    auto append_le=[&](uint32_t value,size_t n) {
        for(size_t i=0;i<n;++i) compressed.push_back(static_cast<uint8_t>(value>>(8*i)));
    };
    append_le(plain.size(),2);
    append_le(static_cast<uint16_t>(~static_cast<uint16_t>(plain.size())),2);
    compressed.insert(compressed.end(),plain.begin(),plain.end());
    append_le(static_cast<uint32_t>(::crc32(0,plain.data(),plain.size())),4);
    append_le(plain.size(),4);
    obd::test::write_file(dir/"single.gz",compressed);
    obd::convert::build_gzip_index(dir/"single.gz",dir/"single.idx");
    const auto index=read_file(dir/"single.idx");
    REQUIRE(le(index,25,8)==1);
    std::vector<uint8_t> checkpoint(29);
    uLongf count=checkpoint.size();
    REQUIRE(::uncompress(checkpoint.data(),&count,index.data()+le(index,313,8),le(index,321,8))==Z_OK);
    REQUIRE(count==29);
    REQUIRE(le(checkpoint,0,8)==0);
    REQUIRE(le(checkpoint,8,8)==10); // Immediately after the gzip header.
    const auto result=obd::test::run_coro([&]() -> elio::coro::task<int> {
        auto source=co_await obd::source::GzipIndexSource::open(
            std::make_unique<obd::test::VectorSource>(compressed),
            std::make_unique<obd::test::VectorSource>(index));
        std::vector<uint8_t> output(plain.size());
        const auto n=co_await source->pread(output.data(),output.size(),0);
        REQUIRE(n==static_cast<ssize_t>(output.size()));
        REQUIRE(output==plain);
        co_return 0;
    });
    REQUIRE(result==0);
}
