#include "turbo_import.hpp"
#include "turbo_package.hpp"
#include "common/errors.hpp"
#include "common/sha256.hpp"
#include "source/local_file.hpp"
#include "source/gzip_index_source.hpp"
#include "format/lsmt.hpp"
#include "format/zfile.hpp"
#include "source/tar_offset.hpp"
#include <elio/runtime/async_main.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>
#include <zlib.h>

namespace obd::convert {
namespace {
constexpr const char* prefix="containerd.io/snapshot/overlaybd/";
struct File {
    FILE* p=nullptr;
    ~File() { if(p) fclose(p); }
};
struct PrivateDirectory {
    std::string path;
    ~PrivateDirectory() {
        if(!path.empty()) { std::error_code ec; std::filesystem::remove_all(path,ec); }
    }
};
struct Identity { uint64_t size; std::string digest; bool gzip; };
Identity identify(const std::string& path) {
    File f{fopen(path.c_str(),"rb")};
    if(!f.p) throw error(errno,"open TurboOCI input for digest");
    struct stat st{};
    if(fstat(fileno(f.p),&st)!=0) throw error(errno,"stat TurboOCI input");
    if(!S_ISREG(st.st_mode) || st.st_size<0) throw error(EINVAL,"TurboOCI input must be regular");
    common::Sha256 hash;
    std::array<uint8_t,65536> data{};
    uint64_t total=0;
    bool is_gzip=false;
    for(;;) {
        size_t n=fread(data.data(),1,data.size(),f.p);
        if(!n) {
            if(ferror(f.p)) throw error(errno ? errno:EIO,"hash TurboOCI input");
            break;
        }
        if(!total) is_gzip=n>=2 && data[0]==0x1f && data[1]==0x8b;
        hash.update(data.data(),n); total+=n;
    }
    if(total!=static_cast<uint64_t>(st.st_size)) throw error(EIO,"TurboOCI input changed during hashing");
    return {total,"sha256:"+hash.final_hex(),is_gzip};
}
bool valid_digest(const std::string& value) {
    return value.size()==71 && value.starts_with("sha256:") &&
        std::all_of(value.begin()+7,value.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');});
}
bool gzip_media(const std::string& media) {
    return media=="application/vnd.oci.image.layer.v1.tar+gzip" ||
           media=="application/vnd.docker.image.rootfs.diff.tar.gzip";
}
bool tar_media(const std::string& media) {
    return media=="application/vnd.oci.image.layer.v1.tar" ||
           media=="application/vnd.docker.image.rootfs.diff.tar";
}
uint64_t le(const uint8_t* p,size_t n) {
    uint64_t result=0;
    for(size_t i=0;i<n;++i) result|=uint64_t(p[i])<<(8*i);
    return result;
}
// Decode bounded checkpoint positions before the async native reader validates
// the header/table. Probing them later rejects malformed lazy dictionaries.
std::vector<uint64_t> checkpoint_offsets(const std::string& path) {
    File f{fopen(path.c_str(),"rb")};
    if(!f.p) throw error(errno,"open imported gzip index");
    std::array<uint8_t,333> header{};
    if(fread(header.data(),1,header.size(),f.p)!=header.size()) throw format_error("truncated imported gzip header");
    const uint64_t count=le(header.data()+25,8), start=le(header.data()+313,8), len=le(header.data()+321,8);
    if(!count || count>(64*1024*1024)/29 || len>64*1024*1024 || start>INT64_MAX)
        throw format_error("imported gzip table exceeds limits");
    std::vector<uint8_t> packed(static_cast<size_t>(len)), decoded(static_cast<size_t>(count*29));
    if(fseeko(f.p,static_cast<off_t>(start),SEEK_SET)!=0 || fread(packed.data(),1,packed.size(),f.p)!=packed.size())
        throw error(errno ? errno:EIO,"read imported gzip table");
    if(header[10]) {
        uLongf output=decoded.size();
        if(uncompress(decoded.data(),&output,packed.data(),packed.size())!=Z_OK || output!=decoded.size())
            throw format_error("invalid imported gzip table");
    } else {
        if(packed.size()!=decoded.size()) throw format_error("invalid imported gzip table size");
        decoded=std::move(packed);
    }
    std::vector<uint64_t> offsets;
    offsets.reserve(count);
    for(size_t i=0;i<count;++i) offsets.push_back(le(decoded.data()+i*29,8));
    return offsets;
}
}

nlohmann::json import_turbo_image(const std::string& package_path,
                                  const std::string& descriptor_path,
                                  const std::string& target_path,
                                  const std::string& destination_dir) {
    namespace fs=std::filesystem;
    if(destination_dir.empty()) throw error(EINVAL,"empty TurboOCI destination");
    const auto destination=fs::absolute(destination_dir).lexically_normal();
    struct stat st{};
    if(lstat(destination.c_str(),&st)==0) throw error(EEXIST,"TurboOCI import destination exists");
    if(errno!=ENOENT) throw error(errno,"inspect TurboOCI import destination");
    std::ifstream descriptor_file(descriptor_path,std::ios::binary);
    if(!descriptor_file) throw error(errno ? errno:ENOENT,"open TurboOCI descriptor");
    // Descriptor input is control-plane metadata, bounded before JSON parsing.
    descriptor_file.seekg(0,std::ios::end);
    const auto descriptor_size=descriptor_file.tellg();
    if(descriptor_size<0 || descriptor_size>1024*1024) throw format_error("TurboOCI descriptor exceeds 1 MiB");
    descriptor_file.seekg(0);
    nlohmann::json document;
    descriptor_file>>document;
    const auto descriptor=document.contains("descriptor") ? document.at("descriptor"):document;
    if(!descriptor.is_object() || !descriptor.contains("size") || !descriptor.at("size").is_number_integer())
        throw format_error("invalid TurboOCI descriptor");
    if(descriptor.at("size").is_number_integer() && !descriptor.at("size").is_number_unsigned() &&
       descriptor.at("size").get<int64_t>()<0) throw format_error("negative TurboOCI descriptor size");
    const std::string media=descriptor.at("mediaType").get<std::string>();
    const std::string digest=descriptor.at("digest").get<std::string>();
    const auto& annotations=descriptor.at("annotations");
    if(!annotations.is_object() || !gzip_media(media) || !valid_digest(digest) ||
       annotations.at(std::string(prefix)+"version")!="0.1.0-turbo.ociv1")
        throw format_error("unsupported TurboOCI descriptor");
    const std::string target_digest=annotations.at(std::string(prefix)+"turbo-oci/target-digest").get<std::string>();
    const std::string target_media=annotations.at(std::string(prefix)+"turbo-oci/target-media-type").get<std::string>();
    if(!valid_digest(target_digest) || (!gzip_media(target_media) && !tar_media(target_media)))
        throw format_error("unsupported TurboOCI target annotation");
    const auto package=identify(package_path), target=identify(target_path);
    if(!package.gzip || package.digest!=digest || package.size!=descriptor.at("size").get<uint64_t>())
        throw format_error("TurboOCI package digest or size mismatch");
    if(target.digest!=target_digest || target.gzip!=gzip_media(target_media))
        throw format_error("TurboOCI target digest or media type mismatch");
    PrivateDirectory staging{destination.string()+".tmp.XXXXXX"};
    std::vector<char> name(staging.path.begin(),staging.path.end()); name.push_back(0);
    if(!mkdtemp(name.data())) { staging.path.clear(); throw error(errno,"create TurboOCI validation directory"); }
    staging.path=name.data();
    const auto extracted=import_turbo_package(package_path,(fs::path(staging.path)/"image").string());
    if((!extracted.gzip_index_path.empty())!=target.gzip)
        throw format_error("TurboOCI gzip index does not match target media type");
    const auto offsets=target.gzip ? checkpoint_offsets(extracted.gzip_index_path):std::vector<uint64_t>{};
    elio::runtime::run_config config;
    config.num_threads=1; config.blocking_threads=1;
    const auto target_absolute=fs::absolute(target_path).lexically_normal().string();
    const uint64_t virtual_size=elio::run([&]() -> elio::coro::task<uint64_t> {
        source::BlobSourcePtr target_source=co_await source::LocalFileSource::open(target_absolute);
        if(target.gzip) {
            auto index=co_await source::LocalFileSource::open(extracted.gzip_index_path);
            auto gzip=co_await source::GzipIndexSource::open(std::move(target_source),std::move(index));
            for(uint64_t offset:offsets) {
                if(offset>=gzip->size()) continue;
                uint8_t byte=0;
                const auto result=co_await gzip->pread(&byte,1,offset);
                if(result!=1) throw format_error("invalid TurboOCI gzip restart checkpoint");
            }
            target_source=std::move(gzip);
        }
        std::array<uint8_t,512> tar_header{};
        const auto tar_read=co_await target_source->pread(tar_header.data(),tar_header.size(),0);
        if(tar_read!=static_cast<ssize_t>(tar_header.size()) ||
           (!std::all_of(tar_header.begin(),tar_header.end(),[](uint8_t b){return b==0;}) &&
            std::memcmp(tar_header.data()+257,"ustar",5)!=0))
            throw format_error("TurboOCI target does not have a tar header");
        source::BlobSourcePtr metadata=co_await source::LocalFileSource::open(extracted.metadata_path);
        metadata=co_await source::TarOffsetSource::open(std::move(metadata));
        if(co_await format::is_zfile(*metadata))
            metadata=co_await format::ZFileSource::open(std::move(metadata),true);
        auto layer=co_await format::LsmtLayer::open_warp(std::move(metadata),std::move(target_source));
        co_return layer->virtual_size();
    },config);
    const auto metadata=identify(extracted.metadata_path);
    nlohmann::json lower={{"file",(destination/"ext4.fs.meta").string()},
        {"digest",metadata.digest},{"size",metadata.size},{"targetFile",target_absolute},
        {"targetDigest",target.digest},{"targetSize",target.size}};
    if(target.gzip) lower["gzipIndex"]=(destination/"gzip.meta").string();
    nlohmann::json result={{"repoBlobUrl",""},{"lowers",nlohmann::json::array({lower})},
        {"converter",{{"backend","turbo-import"},{"imported",true},{"virtual_size",virtual_size}}},
        {"descriptor",descriptor}};
    if(syscall(SYS_renameat2,AT_FDCWD,(fs::path(staging.path)/"image").c_str(),AT_FDCWD,
               destination.c_str(),RENAME_NOREPLACE)!=0)
        throw error(errno,"publish validated TurboOCI image");
    return result;
}
} // namespace obd::convert
