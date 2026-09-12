#include "../../tools/turbo_package.hpp"
#include "../support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <zlib.h>

namespace {
std::vector<uint8_t> read_package(const std::string& path) {
    std::ifstream file(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
uint64_t octal(const uint8_t* p,size_t n) {
    uint64_t value=0;
    for(size_t i=0;i<n && p[i]>='0' && p[i]<='7';++i) value=value*8+p[i]-'0';
    return value;
}
std::vector<uint8_t> unpack(const std::vector<uint8_t>& gzip) {
    std::vector<uint8_t> out(65536);
    z_stream z{};
    REQUIRE(inflateInit2(&z,31)==Z_OK);
    z.next_in=const_cast<uint8_t*>(gzip.data()); z.avail_in=gzip.size();
    z.next_out=out.data(); z.avail_out=out.size();
    REQUIRE(inflate(&z,Z_FINISH)==Z_STREAM_END);
    REQUIRE(z.avail_in==0);
    out.resize(z.total_out); inflateEnd(&z);
    return out;
}
size_t check_member(const std::vector<uint8_t>& tar,size_t at,const char* name,
                    const std::vector<uint8_t>& payload) {
    REQUIRE(at+512<=tar.size());
    const auto* h=tar.data()+at;
    REQUIRE(std::string(reinterpret_cast<const char*>(h))==name);
    REQUIRE(std::string(reinterpret_cast<const char*>(h+257),5)=="ustar");
    REQUIRE(octal(h+100,8)==0644);
    REQUIRE(octal(h+108,8)==0);
    REQUIRE(octal(h+116,8)==0);
    REQUIRE(octal(h+136,12)==0);
    REQUIRE(octal(h+124,12)==payload.size());
    unsigned checksum=0;
    for(size_t i=0;i<512;++i) checksum+=(i>=148 && i<156) ? ' ':h[i];
    REQUIRE(octal(h+148,7)==checksum);
    REQUIRE(at+512+payload.size()<=tar.size());
    REQUIRE(std::equal(payload.begin(),payload.end(),tar.begin()+at+512));
    return at+512+((payload.size()+511)/512)*512;
}
}

TEST_CASE("convert: TurboOCI package has deterministic upstream tar layout", "[convert][turbo]") {
    obd::test::TempDir dir;
    const auto meta=obd::test::pattern_bytes(1003,7), index=obd::test::pattern_bytes(519,9);
    obd::test::write_file(dir/"metadata",meta); obd::test::write_file(dir/"index",index);
    for(bool with_index:{false,true}) {
        const std::string index_path=with_index ? dir/"index":"";
        obd::convert::write_turbo_package(dir/"metadata",index_path,dir/"one.gz");
        obd::convert::write_turbo_package(dir/"metadata",index_path,dir/"two.gz");
        auto gzip=read_package(dir/"one.gz");
        REQUIRE(gzip==read_package(dir/"two.gz"));
        REQUIRE(gzip[4]==0); REQUIRE(gzip[5]==0); REQUIRE(gzip[6]==0); REQUIRE(gzip[7]==0);
        auto tar=unpack(gzip);
        size_t at=check_member(tar,0,"ext4.fs.meta",meta);
        at=check_member(tar,at,".turbo.ociv1",{});
        if(with_index) at=check_member(tar,at,"gzip.meta",index);
        REQUIRE(tar.size()==at+1024);
        REQUIRE(std::all_of(tar.begin()+at,tar.end(),[](auto b){return b==0;}));
    }
}

TEST_CASE("convert: TurboOCI package errors preserve outputs and reject aliases", "[convert][turbo]") {
    obd::test::TempDir dir;
    const std::vector<uint8_t> bytes{'k','e','e','p'};
    obd::test::write_file(dir/"metadata",bytes); obd::test::write_file(dir/"output",bytes);
    REQUIRE_THROWS(obd::convert::write_turbo_package(dir/"missing","",dir/"output"));
    REQUIRE_THROWS(obd::convert::write_turbo_package(dir/"metadata",dir/"missing",dir/"new"));
    REQUIRE(read_package(dir/"output")==bytes);
    REQUIRE_FALSE(std::filesystem::exists(dir/"new"));
    REQUIRE_THROWS(obd::convert::write_turbo_package(dir/"metadata","",dir/"metadata"));
    std::filesystem::create_hard_link(dir/"metadata",dir/"alias");
    REQUIRE_THROWS(obd::convert::write_turbo_package(dir/"metadata","",dir/"alias"));
    REQUIRE(read_package(dir/"metadata")==bytes);
    for(const auto& entry:std::filesystem::directory_iterator(dir.path()))
        REQUIRE(entry.path().filename().string().find(".tmp.")==std::string::npos);
}
