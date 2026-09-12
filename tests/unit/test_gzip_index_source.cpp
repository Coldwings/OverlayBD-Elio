#include "source/gzip_index_source.hpp"
#include "../support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <zlib.h>
#include <elio/runtime/spawn.hpp>
#include <algorithm>
#include <array>
#include <cstring>

namespace {
class DelayedSource final : public obd::source::BlobSource {
    obd::test::VectorSource source_;
public:
    explicit DelayedSource(std::vector<uint8_t> data) : source_(std::move(data)) {}
    uint64_t size() const noexcept override { return source_.size(); }
    std::string_view label() const noexcept override { return "delayed gzip"; }
    elio::coro::task<ssize_t> pread(void* b, size_t n, uint64_t p) override {
        co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        co_return co_await source_.pread(b,n,p);
    }
};
void put(std::vector<uint8_t>& v, size_t at, uint64_t value, size_t n) {
    for (size_t i=0; i<n; ++i) v[at+i] = static_cast<uint8_t>(value >> (8*i));
}
void seal(std::vector<uint8_t>& v) {
    // Independent reflected Castagnoli implementation, seed zero, no complements.
    uint32_t crc = 0;
    for (size_t i=0; i<329; ++i) {
        crc ^= v[i];
        for (int bit=0; bit<8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78U : 0);
    }
    put(v,329,crc,4);
}
std::vector<uint8_t> pack(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> result(compressBound(bytes.size()));
    uLongf len = result.size();
    REQUIRE(compress2(result.data(), &len, bytes.data(), bytes.size(), 6) == Z_OK);
    result.resize(len);
    return result;
}
struct Fixture {
    std::vector<uint8_t> gzip, index;
    explicit Fixture(bool compressed) {
        // Independent golden gzip: stored final DEFLATE block containing abcde.
        gzip = {0x1f,0x8b,8,0,0,0,0,0,0,3, 1,5,0,0xfa,0xff,'a','b','c','d','e',
                0x65,0xd8,0x87,0x85,5,0,0,0};
        std::vector<uint8_t> dictionary(32768), entries(29);
        if (compressed) dictionary = pack(dictionary);
        put(entries,8,10,8);
        put(entries,16,333,8);
        put(entries,25,dictionary.size(),4);
        if (compressed) entries = pack(entries);
        index.resize(333);
        std::memcpy(index.data(),"ddgzidx",7);
        index[8]=1; index[10]=compressed; index[11]=6;
        put(index,13,1048576,4); put(index,17,32768,4); put(index,21,29,4);
        put(index,25,1,8); put(index,33,gzip.size(),8); put(index,49,5,8);
        put(index,313,333+dictionary.size(),8); put(index,321,entries.size(),8);
        index.insert(index.end(),dictionary.begin(),dictionary.end());
        index.insert(index.end(),entries.begin(),entries.end());
        put(index,41,index.size(),8); seal(index);
    }
};
}

TEST_CASE("source: ddgzidx golden raw and zlib random reads", "[source][gzip]") {
    for (bool compressed : {false,true}) {
        const auto result = obd::test::run_coro([compressed]() -> elio::coro::task<int> {
            Fixture f(compressed);
            auto source = co_await obd::source::GzipIndexSource::open(
                std::make_unique<obd::test::VectorSource>(f.gzip),
                std::make_unique<obd::test::VectorSource>(f.index));
            REQUIRE(source->size()==5);
            std::array<char,16> data{};
            const auto read_count_1 = co_await source->pread(data.data(),3,1);
            REQUIRE(read_count_1 == 3);
            REQUIRE(std::string(data.data(),3)=="bcd");
            const auto read_count_2 = co_await source->pread(data.data(),16,3);
            REQUIRE(read_count_2 == 2);
            REQUIRE(std::string(data.data(),2)=="de");
            const auto read_count_3 = co_await source->pread(data.data(),16,5);
            REQUIRE(read_count_3 == 0);
            const auto read_count_4 = co_await source->pread(data.data(),5,0);
            REQUIRE(read_count_4 == 5);
            REQUIRE(std::string(data.data(),5)=="abcde");
            co_return 0;
        });
        REQUIRE(result == 0);
    }
}

TEST_CASE("source: ddgzidx rejects malformed header and entry boundaries", "[source][gzip]") {
    for (int mutation=0; mutation<9; ++mutation) {
        const auto result = obd::test::run_coro([mutation]() -> elio::coro::task<int> {
            Fixture f(false);
            switch (mutation) {
                case 0: f.index[0]='x'; break;
                case 1: f.index[8]=2; break;
                case 2: put(f.index,25,UINT64_MAX,8); break;
                case 3: put(f.index,313,UINT64_MAX,8); break;
                case 4: put(f.index,33,1,8); break;
                case 5: f.index[333+32768+24]=8; break;
                case 6: put(f.index,333+32768+16,332,8); break;
                case 7: put(f.index,333+32768,1,8); break;
                case 8: put(f.index,333+32768+25,32769,4); break;
            }
            seal(f.index);
            bool rejected=false;
            try {
                auto source = co_await obd::source::GzipIndexSource::open(
                    std::make_unique<obd::test::VectorSource>(f.gzip),
                    std::make_unique<obd::test::VectorSource>(f.index));
            } catch (const std::exception&) { rejected=true; }
            REQUIRE(rejected);
            co_return 0;
        });
        REQUIRE(result == 0);
    }
}

TEST_CASE("source: ddgzidx primes partial-byte checkpoint with dictionary", "[source][gzip]") {
    const auto result = obd::test::run_coro([]() -> elio::coro::task<int> {
        // Build two real DEFLATE blocks with the first ending inside a byte.
        // Derive the checkpoint using zlib's documented Z_BLOCK data_type,
        // independently of the ddgzidx reader and project converter.
        std::vector<uint8_t> plain(8192, 'a'), encoded(16384);
        z_stream def{};
        REQUIRE(deflateInit2(&def,6,Z_DEFLATED,31,8,Z_DEFAULT_STRATEGY)==Z_OK);
        def.next_out=encoded.data(); def.avail_out=encoded.size();
        def.next_in=plain.data(); def.avail_in=4096;
        REQUIRE(deflate(&def,Z_BLOCK)==Z_OK);
        def.next_in=plain.data()+4096; def.avail_in=4096;
        REQUIRE(deflate(&def,Z_FINISH)==Z_STREAM_END);
        encoded.resize(def.total_out);
        deflateEnd(&def);
        z_stream inf{};
        REQUIRE(inflateInit2(&inf,31)==Z_OK);
        std::vector<uint8_t> output(8192);
        inf.next_in=encoded.data(); inf.avail_in=encoded.size();
        inf.next_out=output.data(); inf.avail_out=output.size();
        REQUIRE(inflate(&inf,Z_BLOCK)==Z_OK); // gzip header
        REQUIRE(inflate(&inf,Z_BLOCK)==Z_OK); // first block
        const auto input=inf.total_in, offset=inf.total_out;
        const auto bits=inf.data_type & 7;
        inflateEnd(&inf);
        REQUIRE(offset==4096);
        REQUIRE(bits!=0);
        Fixture f(false);
        f.gzip=encoded;
        f.index.resize(333+2*32768+2*29);
        std::fill(f.index.begin()+333,f.index.end(),0);
        std::fill(f.index.begin()+333+32768+32768-4096,
                  f.index.begin()+333+2*32768,'a');
        const size_t entries=333+2*32768;
        put(f.index,entries+8,10,8); put(f.index,entries+16,333,8);
        put(f.index,entries+25,32768,4);
        put(f.index,entries+29,offset,8); put(f.index,entries+37,input,8);
        put(f.index,entries+45,333+32768,8); f.index[entries+53]=bits;
        put(f.index,entries+54,32768,4);
        put(f.index,25,2,8); put(f.index,33,encoded.size(),8);
        put(f.index,41,f.index.size(),8); put(f.index,49,plain.size(),8);
        put(f.index,313,entries,8); put(f.index,321,58,8); seal(f.index);
        auto source=co_await obd::source::GzipIndexSource::open(
            std::make_unique<DelayedSource>(f.gzip),
            std::make_unique<DelayedSource>(f.index));
        std::array<uint8_t,100> read{};
        const auto read_count_5 = co_await source->pread(read.data(),read.size(),5000);
        REQUIRE(read_count_5 == 100);
        REQUIRE(std::all_of(read.begin(),read.end(),[](auto c){return c=='a';}));
        const auto read_count_6 = co_await source->pread(read.data(),read.size(),4050);
        REQUIRE(read_count_6 == 100);
        std::array<uint8_t,100> other{};
        auto first = elio::spawn(source->pread(read.data(),read.size(),5000));
        auto second = elio::spawn(source->pread(other.data(),other.size(),100));
        const auto a = co_await first;
        const auto b = co_await second;
        REQUIRE(a==100);
        REQUIRE(b==100);
        REQUIRE(read==other);
        co_return 0;
    });
    REQUIRE(result == 0);
}
