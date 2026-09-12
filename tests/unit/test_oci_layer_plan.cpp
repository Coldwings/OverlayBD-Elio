#include "../../tools/oci_layer_plan.hpp"
#include "common/errors.hpp"
#include "../support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstring>

using namespace obd;
namespace {
void checksum(std::vector<uint8_t>& h) {
    std::fill(h.begin()+148,h.begin()+156,' ');
    unsigned sum=0;
    for(auto c:h) sum+=c;
    std::snprintf(reinterpret_cast<char*>(h.data()+148),8,"%06o",sum);
    h[154]=0; h[155]=' ';
}
struct Tar {
    std::vector<uint8_t> data;
    uint64_t add(const std::string& name, char type='0', const std::string& payload={},
                 const std::string& link={}) {
        auto h=test::make_tar_header(payload.size(),type);
        std::fill(h.begin(),h.begin()+100,0);
        std::memcpy(h.data(),name.data(),std::min<size_t>(name.size(),100));
        std::memcpy(h.data()+157,link.data(),std::min<size_t>(link.size(),100));
        checksum(h);
        data.insert(data.end(),h.begin(),h.end());
        const uint64_t offset=data.size();
        data.insert(data.end(),payload.begin(),payload.end());
        data.resize((data.size()+511)/512*512,0);
        return offset;
    }
    convert::LayerPlan parse() {
        test::TempDir dir;
        auto finished=data;
        finished.resize(finished.size()+1024);
        return convert::parse_oci_layer_plan(test::write_file(dir/"layer.tar",finished));
    }
};
std::string pax(const std::string& key,const std::string& value) {
    const std::string body=" "+key+"="+value+"\n";
    size_t n=body.size()+1;
    for (;;) {
        const size_t next=body.size()+std::to_string(n).size();
        if (next==n) return std::to_string(n)+body;
        n=next;
    }
}
}

TEST_CASE("format: OCI plan preserves payload positions and whiteout operations", "[convert][oci-plan]") {
    Tar tar;
    tar.add("./d/",'5');
    const auto first=tar.add("d/a",'0',std::string(513,'a'));
    tar.add("d/.wh.old");
    tar.add("d/.wh..wh..opq");
    const auto second=tar.add("d/a",'0',"replacement");
    auto plan=tar.parse();
    REQUIRE(plan.entries.size()==3);
    REQUIRE(plan.entries[0].path=="d");
    REQUIRE(plan.entries[1].payload_spans[0].tar_offset==first);
    REQUIRE(plan.entries[1].payload_spans[0].length==513);
    REQUIRE(plan.entries[2].payload_spans[0].tar_offset==second);
    REQUIRE(plan.whiteout_removals==std::vector<std::string>{"d/old"});
    REQUIRE(plan.opaque_directories==std::vector<std::string>{"d"});
}

TEST_CASE("format: OCI plan handles PAX scope and binary xattrs", "[convert][oci-plan]") {
    Tar tar;
    tar.add("global",'g',pax("uid","123"));
    tar.add("local",'x',pax("path","long/path")+pax("uid","456")+
                           pax("SCHILY.xattr.user.binary",std::string("a\0b",3))+
                           pax("SCHILY.xattr.user.empty",""));
    tar.add("ignored",'0',"x");
    tar.add("next",'0',"y");
    tar.add("global",'g',pax("uid",""));
    tar.add("last");
    const auto plan=tar.parse();
    REQUIRE(plan.entries[0].path=="long/path");
    REQUIRE(plan.entries[0].uid==456);
    REQUIRE(plan.entries[0].xattrs.at("user.binary")==std::string("a\0b",3));
    REQUIRE(plan.entries[0].xattrs.at("user.empty").empty());
    REQUIRE(plan.entries[1].uid==123);
    REQUIRE(plan.entries[1].xattrs.empty());
    REQUIRE(plan.entries[2].uid==0);
}

TEST_CASE("format: OCI plan retains hardlinks symlinks and special node kinds", "[convert][oci-plan]") {
    Tar tar;
    tar.add("a",'0',"content");
    tar.add("hard",'1',{},"./a");
    tar.add("sym",'2',{},"../a");
    tar.add("char",'3');
    tar.add("block",'4');
    tar.add("fifo",'6');
    auto plan=tar.parse();
    REQUIRE(plan.entries[1].kind==convert::EntryKind::Hardlink);
    REQUIRE(plan.entries[1].link_target=="a");
    REQUIRE(plan.entries[2].link_target=="../a");
    REQUIRE(plan.entries[3].kind==convert::EntryKind::Character);
    REQUIRE(plan.entries[4].kind==convert::EntryKind::Block);
    REQUIRE(plan.entries[5].kind==convert::EntryKind::Fifo);
}

TEST_CASE("format: OCI plan applies GNU long names and links once", "[convert][oci-plan]") {
    Tar tar;
    const std::string name=std::string(90,'a')+"/"+std::string(90,'b');
    tar.add("././@LongLink",'L',name+std::string(1,'\0'));
    tar.add("ignored",'0',"x");
    tar.add("././@LongLink",'K',name+std::string(1,'\0'));
    tar.add("hard",'1',{},"ignored");
    tar.add("next");
    const auto plan=tar.parse();
    REQUIRE(plan.entries[0].path==name);
    REQUIRE(plan.entries[1].link_target==name);
    REQUIRE(plan.entries[2].path=="next");
}

TEST_CASE("format: OCI plan maps GNU sparse 0.1 original payload offsets", "[convert][oci-plan]") {
    Tar tar;
    tar.add("pax",'x',pax("GNU.sparse.map","512,3,2048,2")+
        pax("GNU.sparse.size","4096")+pax("GNU.sparse.numblocks","2"));
    const auto at=tar.add("sparse",'0',"abcde");
    const auto plan=tar.parse();
    const auto& entry=plan.entries[0];
    REQUIRE(entry.logical_size==4096);
    REQUIRE(entry.payload_spans.size()==2);
    REQUIRE(entry.payload_spans[0].logical_offset==512);
    REQUIRE(entry.payload_spans[0].tar_offset==at);
    REQUIRE(entry.payload_spans[1].logical_offset==2048);
    REQUIRE(entry.payload_spans[1].tar_offset==at+3);
}

TEST_CASE("format: OCI plan maps GNU sparse 1.0 past embedded map padding", "[convert][oci-plan]") {
    Tar tar;
    tar.add("pax",'x',pax("GNU.sparse.major","1")+pax("GNU.sparse.minor","0")+
        pax("GNU.sparse.realsize","4096")+pax("GNU.sparse.name","real-name"));
    std::string payload="2\n512\n3\n2048\n2\n";
    payload.resize(512,0);
    payload+="abcde";
    const auto at=tar.add("GNUSparseFile",'0',payload);
    const auto plan=tar.parse();
    REQUIRE(plan.entries[0].path=="real-name");
    REQUIRE(plan.entries[0].logical_size==4096);
    REQUIRE(plan.entries[0].payload_spans[0].tar_offset==at+512);
    REQUIRE(plan.entries[0].payload_spans[1].tar_offset==at+515);
}

TEST_CASE("format: OCI plan rejects malformed sparse encodings", "[convert][oci-plan]") {
    Tar tar;
    std::string metadata;
    SECTION("old duplicate-record encoding") { metadata=pax("GNU.sparse.offset","0")+pax("GNU.sparse.numbytes","5"); }
    SECTION("unsupported version") { metadata=pax("GNU.sparse.major","2")+pax("GNU.sparse.minor","0"); }
    SECTION("overlapping spans") { metadata=pax("GNU.sparse.map","0,3,2,2")+pax("GNU.sparse.size","8"); }
    SECTION("overflow extent") { metadata=pax("GNU.sparse.map","18446744073709551615,5")+pax("GNU.sparse.size","8"); }
    SECTION("stored size mismatch") { metadata=pax("GNU.sparse.map","0,4")+pax("GNU.sparse.size","8"); }
    SECTION("odd field count") { metadata=pax("GNU.sparse.map","0,5,8")+pax("GNU.sparse.size","8"); }
    tar.add("pax",'x',metadata);
    tar.add("sparse",'0',"abcde");
    REQUIRE_THROWS_AS(tar.parse(),format_error);
}

TEST_CASE("format: OCI plan rejects unsafe paths and malformed records", "[convert][oci-plan]") {
    Tar tar;
    SECTION("parent traversal") { tar.add("a/../b"); }
    SECTION("absolute path") { tar.add("/outside"); }
    SECTION("hardlink traversal") { tar.add("hard",'1',{},"../outside"); }
    SECTION("PAX traversal") { tar.add("pax",'x',pax("path","../outside")); tar.add("safe"); }
    SECTION("bad PAX length") { tar.add("pax",'x',"999 path=a\n"); tar.add("safe"); }
    SECTION("dangling PAX") { tar.add("pax",'x',pax("path","a")); }
    SECTION("whiteout payload") { tar.add(".wh.a",'0',"x"); }
    SECTION("whiteout directory") { tar.add(".wh.a",'5'); }
    SECTION("old GNU sparse type") { tar.add("sparse",'S'); }
    SECTION("checksum") { tar.add("safe"); tar.data[0]^=1; }
    REQUIRE_THROWS_AS(tar.parse(),format_error);
}

TEST_CASE("format: OCI plan validates archive termination and extension bounds", "[convert][oci-plan]") {
    test::TempDir dir;
    Tar tar;
    tar.add("a");
    SECTION("missing terminator") {}
    SECTION("one zero block") { tar.data.resize(tar.data.size()+512); }
    SECTION("nonzero after terminator") { tar.data.resize(tar.data.size()+1024); tar.data.push_back(1); }
    SECTION("oversized extension") {
        tar.data=test::make_tar_header(16*1024*1024+1,'x');
    }
    REQUIRE_THROWS_AS(convert::parse_oci_layer_plan(test::write_file(dir/"tar",tar.data)),format_error);
}

TEST_CASE("format: OCI plan preserves exact extended mtimes and rejects unrepresentable values", "[convert][oci-plan]") {
    struct Case { const char* text; int64_t seconds; uint32_t nanos; };
    for(const auto& item: {Case{"1700000000.123456789",1700000000,123456789},
                         Case{"-0.25",-1,750000000},
                         Case{"-2147483648",-2147483648LL,0},
                         Case{"2147483648.000000001",2147483648LL,1},
                         Case{"15032385535.999999999",15032385535LL,999999999},
                         Case{"0.123456789000",0,123456789}}) {
        Tar tar;
        tar.add("pax",'x',pax("mtime",item.text));
        tar.add("file");
        const auto plan=tar.parse();
        REQUIRE(plan.entries[0].mtime_seconds==item.seconds);
        REQUIRE(plan.entries[0].mtime_nanoseconds==item.nanos);
    }
    for(const auto* invalid: {"-2147483648.1","15032385536","0.0000000001","1.","1.2x","--1"}) {
        Tar tar;
        tar.add("pax",'x',pax("mtime",invalid));
        tar.add("file");
        REQUIRE_THROWS_AS(tar.parse(),format_error);
    }
    Tar negative;
    negative.add("file");
    std::fill(negative.data.begin()+136,negative.data.begin()+148,0xff);
    std::vector<uint8_t> header(negative.data.begin(),negative.data.begin()+512);
    checksum(header);
    std::copy(header.begin(),header.end(),negative.data.begin());
    REQUIRE(negative.parse().entries[0].mtime_seconds==-1);
    Tar positive;
    positive.add("file");
    std::snprintf(reinterpret_cast<char*>(positive.data.data()+136),12,"%011o",1700000000U);
    header.assign(positive.data.begin(),positive.data.begin()+512);
    checksum(header);
    std::copy(header.begin(),header.end(),positive.data.begin());
    REQUIRE(positive.parse().entries[0].mtime_seconds==1700000000);
}
