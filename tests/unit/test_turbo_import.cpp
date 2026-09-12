#include "../../tools/turbo_import.hpp"
#include "../../tools/turbo_package.hpp"
#include "../../tools/gzip_index_builder.hpp"
#include "format/writer.hpp"
#include "common/sha256.hpp"
#include "../support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iterator>
#include <fcntl.h>
#include <zlib.h>

namespace {
std::vector<uint8_t> load_import_file(const std::string& path) {
    std::ifstream f(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
std::string digest_of(const std::vector<uint8_t>& data) {
    return "sha256:"+obd::common::Sha256::hex(data.data(),data.size());
}
void json_file(const std::string& path,const nlohmann::json& value) {
    std::ofstream out(path); out<<value.dump();
}
struct ImportFixture {
    obd::test::TempDir dir;
    nlohmann::json descriptor;
    explicit ImportFixture(bool gzip_target=false, const std::string& parent_uuid={}) {
        auto header=obd::test::make_tar_header(512);
        std::vector<uint8_t> target(header.begin(),header.end());
        target.resize(2048,0);
        std::fill(target.begin()+512,target.begin()+1024,'t');
        if(gzip_target) {
            std::vector<uint8_t> compressed(compressBound(target.size())+100);
            z_stream z{};
            REQUIRE(deflateInit2(&z,6,Z_DEFLATED,31,8,Z_DEFAULT_STRATEGY)==Z_OK);
            z.next_in=target.data(); z.avail_in=target.size();
            z.next_out=compressed.data(); z.avail_out=compressed.size();
            REQUIRE(deflate(&z,Z_FINISH)==Z_STREAM_END);
            compressed.resize(z.total_out); deflateEnd(&z); target=std::move(compressed);
        }
        obd::test::write_file(dir/"target",target);
        obd::test::write_file(dir/"raw",std::vector<uint8_t>(512,'m'));
        int fd=open((dir/"raw").c_str(),O_RDONLY);
        REQUIRE(fd>=0);
        try {
            obd::format::LsmtWriteOptions options;
            options.uuid="11111111-1111-1111-1111-111111111111";
            options.parent_uuid=parent_uuid;
            obd::format::write_lsmt_warp_layer(fd,1024,
                {{0,1,0,false,0},{1,1,1,false,1}},dir/"metadata",options);
        } catch(...) { close(fd); throw; }
        close(fd);
        if(gzip_target) obd::convert::build_gzip_index(dir/"target",dir/"gzip.idx");
        obd::convert::write_turbo_package(dir/"metadata",gzip_target ? dir/"gzip.idx":"",dir/"package");
        auto package=load_import_file(dir/"package");
        const std::string prefix="containerd.io/snapshot/overlaybd/";
        descriptor={{"mediaType","application/vnd.oci.image.layer.v1.tar+gzip"},
            {"digest",digest_of(package)},{"size",package.size()},
            {"annotations",{{prefix+"version","0.1.0-turbo.ociv1"},
                {prefix+"turbo-oci/target-digest",digest_of(target)},
                {prefix+"turbo-oci/target-media-type",gzip_target ?
                    "application/vnd.oci.image.layer.v1.tar+gzip":"application/vnd.oci.image.layer.v1.tar"}}}};
        json_file(dir/"descriptor",descriptor);
    }
    nlohmann::json run() {
        return obd::convert::import_turbo_image(dir/"package",dir/"descriptor",dir/"target",dir/"imported");
    }
    void refresh_package() {
        auto data=load_import_file(dir/"package");
        descriptor["digest"]=digest_of(data); descriptor["size"]=data.size();
        json_file(dir/"descriptor",descriptor);
    }
};
}

TEST_CASE("convert: TurboOCI descriptor import returns validated runnable config", "[convert][turbo]") {
    for(bool gzip:{false,true}) {
        ImportFixture f(gzip);
        if(gzip) json_file(f.dir/"descriptor",nlohmann::json{{"descriptor",f.descriptor}});
        const auto config=f.run();
        const auto& lower=config.at("lowers").at(0);
        REQUIRE(lower.at("targetFile")==std::filesystem::absolute(f.dir/"target").string());
        REQUIRE(lower.at("targetDigest")==f.descriptor["annotations"]["containerd.io/snapshot/overlaybd/turbo-oci/target-digest"]);
        REQUIRE(load_import_file(lower.at("file").get<std::string>())==load_import_file(f.dir/"metadata"));
        REQUIRE(lower.contains("gzipIndex")==gzip);
        REQUIRE(config.at("converter").at("imported")==true);
        REQUIRE_THROWS(f.run());
    }
}

TEST_CASE("convert: TurboOCI descriptor and native validation fail before publication", "[convert][turbo]") {
    for(int mutation=0;mutation<7;++mutation) {
        ImportFixture f;
        switch(mutation) {
            case 0: f.descriptor["digest"]="sha256:"+std::string(64,'0'); break;
            case 1: f.descriptor["size"]=1; break;
            case 2: f.descriptor["annotations"]["containerd.io/snapshot/overlaybd/version"]="other"; break;
            case 3: f.descriptor["annotations"]["containerd.io/snapshot/overlaybd/turbo-oci/target-digest"]="sha256:"+std::string(64,'0'); break;
            case 4: f.descriptor["annotations"]["containerd.io/snapshot/overlaybd/turbo-oci/target-media-type"]="application/vnd.oci.image.layer.v1.tar+gzip"; break;
            case 5: f.descriptor["mediaType"]="application/octet-stream"; break;
            case 6:
                obd::test::write_file(f.dir/"metadata",std::vector<uint8_t>(8192,0));
                obd::convert::write_turbo_package(f.dir/"metadata","",f.dir/"package");
                f.refresh_package(); break;
        }
        json_file(f.dir/"descriptor",f.descriptor);
        REQUIRE_THROWS(f.run());
        REQUIRE_FALSE(std::filesystem::exists(f.dir/"imported"));
        for(const auto& entry:std::filesystem::directory_iterator(f.dir.path()))
            REQUIRE(entry.path().filename().string().find(".tmp.")==std::string::npos);
    }
}

TEST_CASE("convert: TurboOCI importer rejects malformed lazy gzip dictionary", "[convert][turbo]") {
    ImportFixture f(true);
    auto index=load_import_file(f.dir/"gzip.idx");
    REQUIRE(index.size()>333);
    index[333]^=0xff;
    obd::test::write_file(f.dir/"gzip.idx",index);
    obd::convert::write_turbo_package(f.dir/"metadata",f.dir/"gzip.idx",f.dir/"package");
    f.refresh_package();
    REQUIRE_THROWS(f.run());
    REQUIRE_FALSE(std::filesystem::exists(f.dir/"imported"));
}

TEST_CASE("convert: TurboOCI importer accepts upstream ZFile wrapped warp metadata", "[convert][turbo]") {
    ImportFixture f;
    const auto raw=load_import_file(f.dir/"metadata");
    int fd=open((f.dir/"metadata").c_str(),O_RDONLY);
    REQUIRE(fd>=0);
    try {
        obd::format::write_zfile(fd,raw.size(),f.dir/"compressed.meta");
    } catch(...) { close(fd); throw; }
    close(fd);
    const auto compressed=load_import_file(f.dir/"compressed.meta");
    obd::convert::write_turbo_package(f.dir/"compressed.meta","",f.dir/"package");
    f.refresh_package();
    const auto config=f.run();
    const auto& lower=config.at("lowers").at(0);
    REQUIRE(load_import_file(lower.at("file").get<std::string>())==compressed);
    REQUIRE(lower.at("digest")==digest_of(compressed));
    REQUIRE(config.at("converter").at("virtual_size")==1024);
}

TEST_CASE("convert: TurboOCI differential import rejects missing and mismatched parent chains", "[convert][turbo]") {
    ImportFixture base;
    const auto parent=base.run();
    for(int mutation=0;mutation<8;++mutation) {
        ImportFixture child(false,"11111111-1111-1111-1111-111111111111");
        auto config=parent;
        std::string parent_path=child.dir/"parent.json";
        switch(mutation) {
        case 0: parent_path.clear(); break;
        case 1: config["lowers"][0]["digest"]="sha256:"+std::string(64,'0'); break;
        case 2: config["lowers"][0].erase("file"); break;
        case 3: config["lowers"][0]["targetDigest"]="sha256:"+std::string(64,'0'); break;
        case 4: config["upper"]={{"dir","mutable"}}; break;
        case 5: config["lowers"]=nlohmann::json::array(); break;
        case 6: config["lowers"].push_back(config["lowers"][0]); break;
        case 7: while(config["lowers"].size()<255) config["lowers"].push_back(config["lowers"][0]); break;
        }
        if(!parent_path.empty()) json_file(parent_path,config);
        REQUIRE_THROWS(obd::convert::import_turbo_image(child.dir/"package",child.dir/"descriptor",
            child.dir/"target",child.dir/"imported",parent_path));
        REQUIRE_FALSE(std::filesystem::exists(child.dir/"imported"));
    }
    ImportFixture wrong(false,"22222222-2222-2222-2222-222222222222");
    json_file(wrong.dir/"parent.json",parent);
    REQUIRE_THROWS(obd::convert::import_turbo_image(wrong.dir/"package",wrong.dir/"descriptor",
        wrong.dir/"target",wrong.dir/"imported",wrong.dir/"parent.json"));
    REQUIRE_FALSE(std::filesystem::exists(wrong.dir/"imported"));
    ImportFixture root;
    json_file(root.dir/"parent.json",parent);
    REQUIRE_THROWS(obd::convert::import_turbo_image(root.dir/"package",root.dir/"descriptor",
        root.dir/"target",root.dir/"imported",root.dir/"parent.json"));
    REQUIRE_FALSE(std::filesystem::exists(root.dir/"imported"));
}

TEST_CASE("convert: TurboOCI differential import rejects incompatible parent geometry", "[convert][turbo]") {
    ImportFixture child(false,"11111111-1111-1111-1111-111111111111");
    int fd=open((child.dir/"raw").c_str(),O_RDONLY);
    REQUIRE(fd>=0);
    obd::format::LsmtWriteOptions options;
    options.uuid="11111111-1111-1111-1111-111111111111";
    try { obd::format::write_lsmt_single_layer(fd,512,child.dir/"parent.meta",options); }
    catch(...) { close(fd); throw; }
    close(fd);
    json_file(child.dir/"parent.json",{{"lowers",nlohmann::json::array({{{"file",child.dir/"parent.meta"}}})}});
    REQUIRE_THROWS(obd::convert::import_turbo_image(child.dir/"package",child.dir/"descriptor",
        child.dir/"target",child.dir/"imported",child.dir/"parent.json"));
    REQUIRE_FALSE(std::filesystem::exists(child.dir/"imported"));
}
