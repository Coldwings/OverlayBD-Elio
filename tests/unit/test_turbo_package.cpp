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

TEST_CASE("format: TurboOCI package has deterministic upstream tar layout", "[convert][turbo]") {
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

TEST_CASE("format: TurboOCI package errors preserve outputs and reject aliases", "[convert][turbo]") {
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

namespace {
std::vector<uint8_t> gzip_tar(const std::vector<uint8_t>& tar) {
    std::vector<uint8_t> out(compressBound(tar.size())+100);
    z_stream z{};
    REQUIRE(deflateInit2(&z,6,Z_DEFLATED,31,8,Z_DEFAULT_STRATEGY)==Z_OK);
    z.next_in=const_cast<uint8_t*>(tar.data()); z.avail_in=tar.size();
    z.next_out=out.data(); z.avail_out=out.size();
    REQUIRE(deflate(&z,Z_FINISH)==Z_STREAM_END);
    out.resize(z.total_out); deflateEnd(&z);
    return out;
}
void fix_checksum(std::vector<uint8_t>& tar,size_t at) {
    std::fill(tar.begin()+at+148,tar.begin()+at+156,' ');
    unsigned sum=0;
    for(size_t i=0;i<512;++i) sum+=tar[at+i];
    char text[8]{};
    std::snprintf(text,sizeof(text),"%06o",sum);
    std::memcpy(tar.data()+at+148,text,7); tar[at+155]=' ';
}
}

TEST_CASE("format: TurboOCI importer publishes validated metadata and optional index", "[convert][turbo]") {
    obd::test::TempDir dir;
    const auto metadata=obd::test::pattern_bytes(1003,17), index=obd::test::pattern_bytes(519,19);
    obd::test::write_file(dir/"metadata",metadata); obd::test::write_file(dir/"index",index);
    for(bool has_index:{false,true}) {
        obd::convert::write_turbo_package(dir/"metadata",has_index ? dir/"index":"",dir/"package.gz");
        const std::string destination=dir/(has_index ? "with-index":"without-index");
        auto imported=obd::convert::import_turbo_package(dir/"package.gz",destination);
        REQUIRE(imported.metadata_path==destination+"/ext4.fs.meta");
        REQUIRE(read_package(imported.metadata_path)==metadata);
        if(has_index) REQUIRE(read_package(imported.gzip_index_path)==index);
        else REQUIRE(imported.gzip_index_path.empty());
        REQUIRE_THROWS(obd::convert::import_turbo_package(dir/"package.gz",destination));
        REQUIRE(read_package(imported.metadata_path)==metadata);
    }
}

TEST_CASE("format: TurboOCI importer rejects malformed archives without publication", "[convert][turbo]") {
    obd::test::TempDir dir;
    obd::test::write_file(dir/"metadata",std::vector<uint8_t>(3,'a'));
    obd::convert::write_turbo_package(dir/"metadata","",dir/"valid.gz");
    const auto valid=unpack(read_package(dir/"valid.gz"));
    // The tiny payload makes the marker header start at offset 1024.
    for(int mutation=0;mutation<11;++mutation) {
        auto tar=valid;
        switch(mutation) {
            case 0: tar[148]^=1; break; // checksum
            case 1: std::fill(tar.begin(),tar.begin()+100,0);
                    std::memcpy(tar.data(),"../escape",9); fix_checksum(tar,0); break;
            case 2: tar[156]='2'; fix_checksum(tar,0); break; // symlink
            case 3: std::fill(tar.begin(),tar.begin()+100,0);
                    std::memcpy(tar.data(),"erofs.fs.meta",13); fix_checksum(tar,0); break;
            case 4: std::fill(tar.begin()+1024,tar.begin()+1124,0);
                    std::memcpy(tar.data()+1024,"ext4.fs.meta",12); fix_checksum(tar,1024); break;
            case 5: std::fill(tar.begin()+1024,tar.end(),0); break; // missing marker
            case 6: tar[257]='x'; fix_checksum(tar,0); break;
            case 7: tar[124]=0x80; fix_checksum(tar,0); break; // unsupported binary size
            case 8: tar.back()=1; break; // bad tar termination
            case 9: break; // gzip trailer corruption below
            case 10: break; // concatenated gzip below
        }
        auto gzip=gzip_tar(tar);
        if(mutation==9) gzip[gzip.size()-8]^=1;
        if(mutation==10) { const auto member=gzip; gzip.insert(gzip.end(),member.begin(),member.end()); }
        obd::test::write_file(dir/"bad.gz",gzip);
        REQUIRE_THROWS(obd::convert::import_turbo_package(dir/"bad.gz",dir/"destination"));
        REQUIRE_FALSE(std::filesystem::exists(dir/"destination"));
        REQUIRE_FALSE(std::filesystem::exists(dir/"escape"));
        for(const auto& entry:std::filesystem::directory_iterator(dir.path()))
            REQUIRE(entry.path().filename().string().find(".tmp.")==std::string::npos);
    }
}
