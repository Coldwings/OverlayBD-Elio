#include "turbo_layered.hpp"
#include "oci_layer_plan.hpp"
#include "oci_ext2_builder.hpp"
#include "gzip_index_builder.hpp"
#include "turbo_package.hpp"
#include "common/errors.hpp"
#include "common/sha256.hpp"
#include "format/lsmt_format.hpp"
#include "format/writer.hpp"

#include <nlohmann/json.hpp>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <linux/fs.h>
#include <memory>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace obd::convert {
namespace {
namespace fs=std::filesystem;
struct File {
    int fd=-1;
    explicit File(const std::string& path,int flags=O_RDONLY) {
        fd=::open(path.c_str(),flags|O_CLOEXEC,0644);
        if(fd<0) throw_errno(errno,"open "+path);
    }
    ~File() { if(fd>=0) ::close(fd); }
    File(const File&)=delete;
    File& operator=(const File&)=delete;
};
uint64_t length(int fd) {
    struct stat st {};
    if(::fstat(fd,&st)!=0) throw_errno(errno,"stat TurboOCI input");
    if(!S_ISREG(st.st_mode)||st.st_size<0) throw format_error("TurboOCI input is not a regular file");
    return static_cast<uint64_t>(st.st_size);
}
void read_at(int fd,void* out,size_t n,uint64_t offset) {
    size_t done=0;
    while(done<n) {
        const auto r=::pread(fd,static_cast<uint8_t*>(out)+done,n-done,static_cast<off_t>(offset+done));
        if(r<0) { if(errno==EINTR) continue; throw_errno(errno,"read TurboOCI data"); }
        if(r==0) throw error(EIO,"short TurboOCI read");
        done+=static_cast<size_t>(r);
    }
}
void write_at(int fd,const void* data,size_t n,uint64_t offset) {
    size_t done=0;
    while(done<n) {
        const auto r=::pwrite(fd,static_cast<const uint8_t*>(data)+done,n-done,static_cast<off_t>(offset+done));
        if(r<0) { if(errno==EINTR) continue; throw_errno(errno,"write TurboOCI data"); }
        if(r==0) throw error(EIO,"short TurboOCI write");
        done+=static_cast<size_t>(r);
    }
}
std::string digest(int fd,uint64_t n) {
    common::Sha256 hash;
    std::array<uint8_t,1<<20> buf {};
    for(uint64_t off=0;off<n;) {
        const auto count=static_cast<size_t>(std::min<uint64_t>(buf.size(),n-off));
        read_at(fd,buf.data(),count,off);
        hash.update(buf.data(),count);
        off+=count;
    }
    return hash.final_hex();
}
std::string digest(const std::string& path) { File file(path); return digest(file.fd,length(file.fd)); }
void sync_file(const std::string& path) {
    File file(path);
    if(::fsync(file.fd)!=0) throw_errno(errno,"sync TurboOCI file");
}
void sync_dir(const std::string& path) {
    File dir(path,O_RDONLY|O_DIRECTORY);
    if(::fsync(dir.fd)!=0) throw_errno(errno,"sync TurboOCI directory");
}
void json_file(const std::string& path,const nlohmann::json& value) {
    File file(path,O_WRONLY|O_CREAT|O_EXCL);
    const auto text=value.dump(2)+"\n";
    write_at(file.fd,text.data(),text.size(),0);
    if(::fsync(file.fd)!=0) throw_errno(errno,"sync TurboOCI JSON");
}
struct Stage {
    fs::path path;
    ~Stage() { if(!path.empty()) { std::error_code ec; fs::remove_all(path,ec); } }
};
uint64_t add(uint64_t a,uint64_t b) {
    if(b>UINT64_MAX-a) throw format_error("TurboOCI capacity overflow");
    return a+b;
}
uint64_t multiply(uint64_t a,uint64_t b) {
    if(b && a>UINT64_MAX/b) throw format_error("TurboOCI capacity overflow");
    return a*b;
}
uint64_t ceil_div(uint64_t n,uint64_t d) { return n/d+(n%d!=0); }
struct Input {
    std::string original,tar,subdir,index;
    uint64_t size=0;
    std::string hash;
    bool gzip=false;
    LayerPlan plan;
};
void spool_gzip(const std::string& original,const std::string& output) {
    // build_gzip_index has already validated a single complete gzip member.
    gzFile gzip=::gzopen(original.c_str(),"rbe");
    if(!gzip) throw error(EIO,"open gzip tar spool");
    struct GzGuard { gzFile file; ~GzGuard(){if(file) gzclose(file);} } guard{gzip};
    File out(output,O_WRONLY|O_CREAT|O_EXCL);
    std::array<uint8_t,1<<20> buf {};
    uint64_t offset=0;
    for(;;) {
        const int n=gzread(gzip,buf.data(),static_cast<unsigned>(buf.size()));
        if(n<0) throw format_error("gzip decompression failed");
        if(n==0) break;
        if(uint64_t(n)>uint64_t(std::numeric_limits<off_t>::max())-offset)
            throw format_error("gzip spool exceeds supported file offsets");
        write_at(out.fd,buf.data(),static_cast<size_t>(n),offset);
        offset+=static_cast<uint64_t>(n);
    }
    guard.file=nullptr;
    if(gzclose(gzip)!=Z_OK) throw format_error("gzip spool finalization failed");
}
std::pair<uint64_t,uint32_t> capacity(const std::vector<Input>& inputs,uint64_t requested) {
    uint64_t blocks=4096,inodes=128;
    for(const auto& input:inputs) {
        for(const auto& entry:input.plan.entries) {
            const uint64_t depth=1+static_cast<uint64_t>(std::count(entry.path.begin(),entry.path.end(),'/'));
            inodes=add(inodes,depth+1);
            // Every path component may create a directory. Count duplicates
            // conservatively; directory blocks and indirect tables can grow.
            blocks=add(blocks,multiply(depth+1,2));
            if(entry.kind==EntryKind::Regular) {
                uint64_t allocated=0;
                for(const auto& span:entry.payload_spans)
                    allocated=add(allocated,ceil_div(add(span.length,span.logical_offset%4096),4096));
                blocks=add(blocks,allocated);
                // Sparse spans can each need indirect paths even when few
                // data blocks exist at high logical positions.
                blocks=add(blocks,add(ceil_div(allocated,1024),multiply(entry.payload_spans.size(),3)));
            } else if(entry.kind==EntryKind::Symlink) {
                blocks=add(blocks,ceil_div(entry.link_target.size(),4096));
            }
            for(const auto& [key,value]:entry.xattrs) {
                (void)key;
                blocks=add(blocks,add(ceil_div(value.size(),4096),1));
            }
        }
        for(const auto& opaque:input.plan.opaque_directories) {
            const uint64_t depth=1+static_cast<uint64_t>(std::count(opaque.begin(),opaque.end(),'/'));
            inodes=add(inodes,depth);
            blocks=add(blocks,multiply(depth,2));
        }
    }
    inodes=multiply(ceil_div(inodes,128),128);
    if(inodes>UINT32_MAX) throw format_error("TurboOCI inode capacity exceeds filesystem limit");
    blocks=add(blocks,ceil_div(multiply(inodes,256),4096));
    blocks=multiply(blocks,2); // bitmap, backup superblock and allocation slack
    // ext2 inode bitmap capacity is 32768 per 128 MiB block group.
    if(inodes>32768) blocks=std::max(blocks,multiply(ceil_div(inodes,32768),32768));
    const uint64_t automatic=multiply(blocks,4096);
    const uint64_t size=requested ? requested : automatic;
    if(size==0 || size%4096 || size/4096>UINT32_MAX)
        throw format_error("TurboOCI virtual size must be aligned and fit ext2 geometry");
    return {size,static_cast<uint32_t>(inodes)};
}
std::string uuid(const std::string& raw_digest,const std::string& parent) {
    const auto seed=raw_digest+":"+parent;
    std::string hex=common::Sha256::hex(seed.data(),seed.size()).substr(0,32);
    hex.insert(20,1,'-'); hex.insert(16,1,'-'); hex.insert(12,1,'-'); hex.insert(8,1,'-');
    return hex;
}
std::vector<bytes::segment_mapping> differential(
    int raw,int previous,uint64_t size,const std::vector<bytes::segment_mapping>& remote) {
    using Mapping=bytes::segment_mapping;
    std::vector<Mapping> out;
    uint64_t remote_end=0;
    for(const auto& m:remote) {
        if(m.offset<remote_end || m.length==0 || m.end()>size/512 || m.tag!=1 || m.zeroed)
            throw format_error("TurboOCI invalid current payload mapping");
        remote_end=m.end();
    }
    auto append=[&](Mapping m) {
        if(!out.empty()) {
            auto& last=out.back();
            if(last.end()==m.offset && last.tag==m.tag && last.zeroed==m.zeroed &&
               (m.zeroed || last.mend()==m.moffset) && m.length<=Mapping::kMaxLength-last.length) {
                last.length+=m.length; return;
            }
        }
        out.push_back(m);
        if(out.size()>format::lsmt::kMaxRoIndexSize) throw format_error("TurboOCI differential index too large");
    };
    std::array<uint8_t,1<<20> current {}, old {};
    size_t remote_index=0;
    for(uint64_t off=0;off<size;) {
        const auto n=static_cast<size_t>(std::min<uint64_t>(current.size(),size-off));
        read_at(raw,current.data(),n,off);
        if(previous>=0) read_at(previous,old.data(),n,off);
        for(size_t pos=0;pos<n;pos+=512) {
            const uint64_t sector=(off+pos)/512;
            while(remote_index<remote.size() && remote[remote_index].end()<=sector) ++remote_index;
            const bool is_remote=remote_index<remote.size() && remote[remote_index].offset<=sector;
            if(is_remote) {
                const auto& m=remote[remote_index];
                append({sector,1,m.moffset+sector-m.offset,false,1});
            } else if(sector==0 || previous<0 || std::memcmp(current.data()+pos,old.data()+pos,512)!=0) {
                const bool zero=std::all_of(current.begin()+pos,current.begin()+pos+512,[](auto c){return c==0;});
                append({sector,1,sector,zero,0});
            }
        }
        off+=n;
    }
    // Filesystem sector zero is always metadata; it is also the required
    // minimum-tag anchor when a layer otherwise contains remote data only.
    if(out.empty() || out.front().offset!=0 || out.front().tag!=0)
        throw format_error("TurboOCI payload unexpectedly overlaps filesystem anchor");
    return out;
}
void snapshot(int source,int destination,uint64_t size) {
    if(::ftruncate(destination,0)!=0 || ::ftruncate(destination,static_cast<off_t>(size))!=0)
        throw_errno(errno,"size TurboOCI snapshot");
    std::array<uint8_t,1<<20> buffer {};
    for(uint64_t off=0;off<size;) {
        const auto n=static_cast<size_t>(std::min<uint64_t>(buffer.size(),size-off));
        read_at(source,buffer.data(),n,off);
        if(!std::all_of(buffer.begin(),buffer.begin()+n,[](auto c){return c==0;}))
            write_at(destination,buffer.data(),n,off);
        off+=n;
    }
}
}

nlohmann::json convert_turbo_layers(const std::vector<std::string>& filenames,
                                    const std::string& output_directory,
                                    uint64_t requested_size,bool keep_raw) {
    if(filenames.empty() || filenames.size()>format::lsmt::kMaxStackLayers)
        throw format_error("TurboOCI requires between 1 and 255 input layers");
    const fs::path destination=fs::absolute(output_directory).lexically_normal();
    if(destination.filename().empty()) throw format_error("invalid TurboOCI destination");
    struct stat st {};
    if(::lstat(destination.c_str(),&st)==0) throw error(EEXIST,"TurboOCI destination already exists");
    if(errno!=ENOENT) throw_errno(errno,"inspect TurboOCI destination");
    fs::create_directories(destination.parent_path());
    std::string pattern=(destination.parent_path()/".turbo-stage-XXXXXX").string();
    std::vector<char> temp(pattern.begin(),pattern.end()); temp.push_back(0);
    if(!::mkdtemp(temp.data())) throw_errno(errno,"create TurboOCI stage");
    Stage stage{fs::path(temp.data())};
    std::vector<Input> inputs;
    for(size_t i=0;i<filenames.size();++i) {
        Input input;
        input.original=fs::absolute(filenames[i]).lexically_normal().string();
        File original(input.original);
        input.size=length(original.fd);
        input.hash=digest(original.fd,input.size);
        std::array<uint8_t,2> signature {};
        if(input.size>=2) read_at(original.fd,signature.data(),2,0);
        input.gzip=signature[0]==0x1f && signature[1]==0x8b;
        input.subdir="layer-"+std::to_string(i);
        const auto directory=stage.path/input.subdir;
        fs::create_directory(directory);
        input.tar=input.original;
        if(input.gzip) {
            input.index=(directory/"gzip.meta").string();
            build_gzip_index(input.original,input.index);
            input.tar=(stage.path/(".tar-"+std::to_string(i))).string();
            spool_gzip(input.original,input.tar);
        }
        input.plan=parse_oci_layer_plan(input.tar);
        inputs.push_back(std::move(input));
    }
    const auto [size,inodes]=capacity(inputs,requested_size);
    const std::string raw_path=(stage.path/"rootfs.ext2").string();
    const std::string snapshot_path=(stage.path/".previous.ext2").string();
    nlohmann::json result={{"repoBlobUrl",""},{"lowers",nlohmann::json::array()},
        {"descriptors",nlohmann::json::array()},{"packages",nlohmann::json::array()}};
    std::string parent_uuid,raw_hash;
    {
        OciExt2Builder builder(raw_path,size,inodes);
        File raw(raw_path);
        File previous(snapshot_path,O_RDWR|O_CREAT|O_EXCL);
        for(size_t i=0;i<inputs.size();++i) {
            const auto& input=inputs[i];
            const auto remote=builder.apply(input.plan,input.tar);
            const auto mappings=differential(raw.fd,i==0 ? -1 : previous.fd,size,remote);
            raw_hash=digest(raw.fd,size);
            format::LsmtWriteOptions opts;
            opts.uuid=uuid(raw_hash,parent_uuid);
            opts.parent_uuid=parent_uuid;
            opts.user_tag="obd-convert turboOCI layered libe2fs";
            const auto local=stage.path/input.subdir;
            const auto published=destination/input.subdir;
            const std::string metadata=(local/"ext4.fs.meta").string();
            const std::string package=(local/"turboOCIv1.tar.gz").string();
            format::write_lsmt_warp_layer(raw.fd,size,mappings,metadata,opts);
            write_turbo_package(metadata,input.index,package);
            const std::string package_hash="sha256:"+digest(package);
            File package_file(package);
            const uint64_t package_size=length(package_file.fd);
            const std::string prefix="containerd.io/snapshot/overlaybd/";
            nlohmann::json descriptor={{"mediaType","application/vnd.oci.image.layer.v1.tar+gzip"},
                {"digest",package_hash},{"size",package_size},{"annotations",{
                    {prefix+"version","0.1.0-turbo.ociv1"},
                    {prefix+"blob-digest",package_hash},{prefix+"blob-size",std::to_string(package_size)},
                    {prefix+"turbo-oci/target-digest","sha256:"+input.hash},
                    {prefix+"turbo-oci/target-media-type",input.gzip ?
                        "application/vnd.oci.image.layer.v1.tar+gzip" : "application/vnd.oci.image.layer.v1.tar"}}}};
            File metadata_file(metadata);
            nlohmann::json lower={{"file",(published/"ext4.fs.meta").string()},
                {"digest","sha256:"+digest(metadata_file.fd,length(metadata_file.fd))},
                {"size",length(metadata_file.fd)},{"targetFile",input.original},
                {"targetDigest","sha256:"+input.hash},{"targetSize",input.size}};
            if(input.gzip) lower["gzipIndex"]=(published/"gzip.meta").string();
            result["lowers"].push_back(lower);
            result["descriptors"].push_back(descriptor);
            result["packages"].push_back((published/"turboOCIv1.tar.gz").string());
            json_file((local/"descriptor.json").string(),descriptor);
            json_file((local/"config.json").string(),nlohmann::json{{"repoBlobUrl",""},{"lowers",result["lowers"]}});
            sync_file(metadata); sync_file(package);
            if(input.gzip) sync_file(input.index);
            sync_dir(local.string());
            if(i+1<inputs.size()) snapshot(raw.fd,previous.fd,size);
            parent_uuid=opts.uuid;
        }
    }
    if(digest(raw_path)!=raw_hash)
        throw format_error("TurboOCI filesystem changed after final layer snapshot");
    for(const auto& input:inputs) {
        File original(input.original);
        if(length(original.fd)!=input.size || digest(original.fd,input.size)!=input.hash)
            throw format_error("TurboOCI original changed during conversion");
        if(input.gzip) fs::remove(input.tar);
    }
    fs::remove(snapshot_path);
    result["converter"]={{"backend","libe2fs"},{"filesystem","ext2"},
        {"virtual_size",size},{"raw_digest","sha256:"+raw_hash}};
    if(keep_raw) {
        sync_file(raw_path);
        result["converter"]["raw_file"]=(destination/"rootfs.ext2").string();
    } else fs::remove(raw_path);
    if(inputs.size()==1) {
        result["descriptor"]=result["descriptors"][0];
        result["package"]=result["packages"][0];
    }
    json_file((stage.path/"config.json").string(),result);
    sync_dir(stage.path.string());
    if(::syscall(SYS_renameat2,AT_FDCWD,stage.path.c_str(),AT_FDCWD,destination.c_str(),RENAME_NOREPLACE)!=0)
        throw_errno(errno,"publish TurboOCI layer stack without replacement");
    stage.path.clear();
    sync_dir(destination.parent_path().string());
    return result;
}
}  // namespace obd::convert
