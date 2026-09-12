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
#include <cctype>
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
struct RecordedInput { std::string path; Identity identity; };
using Inputs = std::vector<RecordedInput>;
Identity identify(const std::string& path) {
    File f{fopen(path.c_str(),"rbe")};
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
Identity record_input(Inputs& inputs,const std::string& path) {
    auto identity=identify(path);
    inputs.push_back({path,identity});
    return identity;
}
void revalidate_inputs(const Inputs& inputs) {
    for(const auto& input:inputs) {
        const auto current=identify(input.path);
        if(current.size!=input.identity.size || current.digest!=input.identity.digest ||
           current.gzip!=input.identity.gzip)
            throw format_error("TurboOCI input changed before publication: "+input.path);
    }
}
nlohmann::json read_control(const std::string& path,Inputs& inputs) {
    File file{fopen(path.c_str(),"rbe")};
    if(!file.p) throw error(errno,"open TurboOCI control input");
    // Bound the actual consumed bytes, including a concurrent file growth.
    std::vector<char> bytes(1024*1024+1);
    const size_t count=fread(bytes.data(),1,bytes.size(),file.p);
    if(ferror(file.p)) throw error(errno ? errno:EIO,"read TurboOCI control input");
    if(count>1024*1024) throw format_error("TurboOCI control input exceeds 1 MiB");
    inputs.push_back({path,{count,"sha256:"+common::Sha256::hex(bytes.data(),count),false}});
    return nlohmann::json::parse(bytes.data(),bytes.data()+count);
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
    File f{fopen(path.c_str(),"rbe")};
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
struct LocalLayer {
    nlohmann::json lower;
    std::string metadata, target, index;
    std::vector<uint64_t> checkpoints;
};
std::string uuid_key(std::string value) {
    if(value.empty()) return {};
    if(value.size()!=36) throw format_error("invalid TurboOCI layer UUID");
    for(size_t i=0;i<value.size();++i) {
        if(i==8 || i==13 || i==18 || i==23) {
            if(value[i]!='-') throw format_error("invalid TurboOCI layer UUID");
        } else if(!std::isxdigit(static_cast<unsigned char>(value[i])))
            throw format_error("invalid TurboOCI layer UUID");
        value[i]=static_cast<char>(std::tolower(static_cast<unsigned char>(value[i])));
    }
    return value=="00000000-0000-0000-0000-000000000000" ? "" : value;
}
elio::coro::task<void> validate_tar_target(source::BlobSource& target) {
    std::array<uint8_t,512> tar_header{};
    const auto tar_read=co_await target.pread(tar_header.data(),tar_header.size(),0);
    if(tar_read!=static_cast<ssize_t>(tar_header.size()))
        throw format_error("TurboOCI target does not have a complete tar header");
    if(std::all_of(tar_header.begin(),tar_header.end(),[](uint8_t b){return b==0;})) {
        const auto terminator=co_await target.pread(tar_header.data(),tar_header.size(),512);
        if(target.size()<1024 || terminator!=static_cast<ssize_t>(tar_header.size()) ||
           !std::all_of(tar_header.begin(),tar_header.end(),[](uint8_t b){return b==0;}))
            throw format_error("TurboOCI empty tar requires two zero terminator blocks");
    } else if(std::memcmp(tar_header.data()+257,"ustar",5)!=0)
        throw format_error("TurboOCI target does not have a tar header");
    co_return;
}
elio::coro::task<std::unique_ptr<format::LsmtLayer>> open_local_layer(const LocalLayer& local) {
    source::BlobSourcePtr metadata=co_await source::LocalFileSource::open(local.metadata);
    metadata=co_await source::TarOffsetSource::open(std::move(metadata));
    if(co_await format::is_zfile(*metadata))
        metadata=co_await format::ZFileSource::open(std::move(metadata),true);
    if(local.target.empty()) co_return co_await format::LsmtLayer::open(std::move(metadata));
    source::BlobSourcePtr target=co_await source::LocalFileSource::open(local.target);
    if(!local.index.empty()) {
        auto index=co_await source::LocalFileSource::open(local.index);
        auto gzip=co_await source::GzipIndexSource::open(std::move(target),std::move(index));
        for(uint64_t offset:local.checkpoints) {
            if(offset>=gzip->size()) continue;
            uint8_t byte=0;
            const auto n=co_await gzip->pread(&byte,1,offset);
            if(n!=1) throw format_error("invalid parent TurboOCI gzip restart checkpoint");
        }
        target=std::move(gzip);
    }
    co_await validate_tar_target(*target);
    co_return co_await format::LsmtLayer::open_warp(std::move(metadata),std::move(target));
}
std::vector<LocalLayer> load_parents(const std::string& path,Inputs& inputs) {
    namespace fs=std::filesystem;
    if(path.empty()) return {};
    const auto document=read_control(path,inputs);
    if(!document.is_object() || !document.contains("lowers") || !document["lowers"].is_array() ||
       document["lowers"].empty() || document["lowers"].size()>=255 ||
       (document.contains("upper") && !document["upper"].empty()))
        throw format_error("TurboOCI import requires a read-only parent stack of 1..254 layers");
    const auto base=fs::absolute(path).parent_path();
    auto resolve=[&](const std::string& value) {
        return (base/fs::path(value)).lexically_normal().string();
    };
    std::vector<LocalLayer> parents;
    for(const auto& lower:document["lowers"]) {
        if(!lower.is_object() || lower.value("file",std::string{}).empty())
            throw format_error("TurboOCI parent metadata requires a local file");
        LocalLayer local;
        local.metadata=resolve(lower.at("file").get<std::string>());
        const auto metadata=record_input(inputs,local.metadata);
        if((lower.contains("digest") && lower.at("digest")!=metadata.digest) ||
           (lower.contains("size") && lower.at("size")!=metadata.size))
            throw format_error("TurboOCI parent metadata digest or size mismatch");
        local.lower={{"file",local.metadata},{"digest",metadata.digest},{"size",metadata.size}};
        if(lower.contains("targetFile") && !lower.at("targetFile").get<std::string>().empty()) {
            local.target=resolve(lower.at("targetFile").get<std::string>());
            const auto target=record_input(inputs,local.target);
            if((lower.contains("targetDigest") && lower.at("targetDigest")!=target.digest) ||
               (lower.contains("targetSize") && lower.at("targetSize")!=target.size))
                throw format_error("TurboOCI parent target digest or size mismatch");
            local.lower["targetFile"]=local.target;
            local.lower["targetDigest"]=target.digest;
            local.lower["targetSize"]=target.size;
            if(lower.contains("gzipIndex") && !lower.at("gzipIndex").get<std::string>().empty()) {
                local.index=resolve(lower.at("gzipIndex").get<std::string>());
                (void)record_input(inputs,local.index); // require a regular local index before reading
                local.checkpoints=checkpoint_offsets(local.index);
                local.lower["gzipIndex"]=local.index;
            }
            if(target.gzip!=!local.index.empty())
                throw format_error("TurboOCI parent gzip target/index mismatch");
        } else if(lower.contains("targetDigest") || lower.contains("gzipIndex"))
            throw format_error("TurboOCI parent target requires a local file");
        parents.push_back(std::move(local));
    }
    return parents;
}

}

nlohmann::json import_turbo_image(const std::string& package_path,
                                  const std::string& descriptor_path,
                                  const std::string& target_path,
                                  const std::string& destination_dir,
                                  const std::string& parent_config_path,
                                  uint64_t metadata_budget
#ifdef OBD_TEST_TURBO_IMPORT_HOOK
                                  , const std::function<void()>& before_publish
#endif
                                  ) {
    namespace fs=std::filesystem;
    if(destination_dir.empty()) throw error(EINVAL,"empty TurboOCI destination");
    const auto destination=fs::absolute(destination_dir).lexically_normal();
    struct stat st{};
    if(lstat(destination.c_str(),&st)==0) throw error(EEXIST,"TurboOCI import destination exists");
    if(errno!=ENOENT) throw error(errno,"inspect TurboOCI import destination");
    Inputs inputs;
    const auto document=read_control(descriptor_path,inputs);
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
    const auto package=record_input(inputs,package_path), target=record_input(inputs,target_path);
    if(!package.gzip || package.digest!=digest || package.size!=descriptor.at("size").get<uint64_t>())
        throw format_error("TurboOCI package digest or size mismatch");
    if(target.digest!=target_digest || target.gzip!=gzip_media(target_media))
        throw format_error("TurboOCI target digest or media type mismatch");
    const auto parents=load_parents(parent_config_path,inputs);
    PrivateDirectory staging{destination.string()+".tmp.XXXXXX"};
    std::vector<char> name(staging.path.begin(),staging.path.end()); name.push_back(0);
    if(!mkdtemp(name.data())) { staging.path.clear(); throw error(errno,"create TurboOCI validation directory"); }
    staging.path=name.data();
    const auto extracted=import_turbo_package(package_path,(fs::path(staging.path)/"image").string(),metadata_budget);
    if((!extracted.gzip_index_path.empty())!=target.gzip)
        throw format_error("TurboOCI gzip index does not match target media type");
    (void)record_input(inputs,extracted.metadata_path);
    if(target.gzip) (void)record_input(inputs,extracted.gzip_index_path);
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
        co_await validate_tar_target(*target_source);
        source::BlobSourcePtr metadata=co_await source::LocalFileSource::open(extracted.metadata_path);
        metadata=co_await source::TarOffsetSource::open(std::move(metadata));
        if(co_await format::is_zfile(*metadata))
            metadata=co_await format::ZFileSource::open(std::move(metadata),true);
        auto layer=co_await format::LsmtLayer::open_warp(std::move(metadata),std::move(target_source));
        const auto child_parent=uuid_key(layer->header().parent_uuid);
        if(parents.empty()) {
            if(!child_parent.empty()) throw format_error("differential TurboOCI layer requires --parent-config");
        } else {
            if(child_parent.empty()) throw format_error("root TurboOCI layer cannot extend a parent stack");
            std::string previous;
            for(const auto& parent:parents) {
                auto opened=co_await open_local_layer(parent);
                if(uuid_key(opened->header().parent_uuid)!=previous)
                    throw format_error("TurboOCI parent UUID chain is incomplete or mismatched");
                if(opened->virtual_size()!=layer->virtual_size())
                    throw format_error("TurboOCI parent virtual size mismatch");
                previous=uuid_key(opened->header().uuid);
                if(previous.empty()) throw format_error("TurboOCI parent layer lacks a nonzero UUID");
            }
            if(previous!=child_parent) throw format_error("TurboOCI child parent UUID mismatch");
        }
        co_return layer->virtual_size();
    },config);
    const auto metadata=identify(extracted.metadata_path);
    nlohmann::json lower={{"file",(destination/"ext4.fs.meta").string()},
        {"digest",metadata.digest},{"size",metadata.size},{"targetFile",target_absolute},
        {"targetDigest",target.digest},{"targetSize",target.size}};
    if(target.gzip) lower["gzipIndex"]=(destination/"gzip.meta").string();
    nlohmann::json lowers=nlohmann::json::array();
    for(const auto& parent:parents) lowers.push_back(parent.lower);
    lowers.push_back(lower);
    nlohmann::json result={{"repoBlobUrl",""},{"lowers",std::move(lowers)},
        {"converter",{{"backend","turbo-import"},{"imported",true},{"virtual_size",virtual_size}}},
        {"descriptor",descriptor}};
#ifdef OBD_TEST_TURBO_IMPORT_HOOK
    if(before_publish) before_publish();
#endif
    revalidate_inputs(inputs);
    if(syscall(SYS_renameat2,AT_FDCWD,(fs::path(staging.path)/"image").c_str(),AT_FDCWD,
               destination.c_str(),RENAME_NOREPLACE)!=0)
        throw error(errno,"publish validated TurboOCI image");
    return result;
}
} // namespace obd::convert
