#include "oci_ext2_builder.hpp"
#include "common/errors.hpp"
#include "format/lsmt_format.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

#if OBD_HAVE_LIBE2FS
extern "C" {
#include <et/com_err.h>
#include <ext2fs/ext2_fs.h>
#include <ext2fs/ext2_io.h>
#include <ext2fs/ext2fs.h>
}
#endif

namespace obd::convert {
#if OBD_HAVE_LIBE2FS
namespace {
void check(errcode_t rc, const char* what) {
    if (rc) throw std::runtime_error(std::string("OCI ext2: ")+what+": "+error_message(rc));
}
struct Fd {
    int fd=-1;
    ~Fd() { if(fd>=0) ::close(fd); }
};
void read_at(int fd, void* buffer, size_t count, uint64_t offset) {
    size_t done=0;
    while(done<count) {
        const auto n=::pread(fd,static_cast<uint8_t*>(buffer)+done,count-done,static_cast<off_t>(offset+done));
        if(n<0) { if(errno==EINTR) continue; throw_errno(errno,"read OCI payload"); }
        if(n==0) throw error(EIO,"short OCI payload");
        done+=static_cast<size_t>(n);
    }
}
void write_at(int fd,const void* buffer,size_t count,uint64_t offset) {
    size_t done=0;
    while(done<count) {
        const auto n=::pwrite(fd,static_cast<const uint8_t*>(buffer)+done,count-done,static_cast<off_t>(offset+done));
        if(n<0) { if(errno==EINTR) continue; throw_errno(errno,"write OCI filesystem"); }
        if(n==0) throw error(EIO,"short OCI filesystem write");
        done+=static_cast<size_t>(n);
    }
}
void pin(struct ext2_super_block& super) {
    const uint8_t uuid[16]={0x4f,0x42,0x44,0x45,0x4c,0x49,0x4f,0x2d,0x43,0x4f,0x4e,0x56,0x45,0x52,0x54,0};
    std::memcpy(super.s_uuid,uuid,sizeof(uuid));
    std::memset(super.s_volume_name,0,sizeof(super.s_volume_name));
    std::memcpy(super.s_volume_name,"obd-convert",11);
    super.s_mtime=super.s_wtime=super.s_lastcheck=super.s_mkfs_time=0;
    super.s_mtime_hi=super.s_wtime_hi=super.s_lastcheck_hi=super.s_mkfs_time_hi=0;
}
std::string parent_of(const std::string& path) {
    const auto slash=path.rfind('/');
    return slash==std::string::npos ? "" : path.substr(0,slash);
}
std::string leaf_of(const std::string& path) {
    const auto slash=path.rfind('/');
    return path.substr(slash==std::string::npos ? 0 : slash+1);
}
uint16_t inode_type(EntryKind kind) {
    switch(kind) {
    case EntryKind::Regular: return LINUX_S_IFREG;
    case EntryKind::Directory: return LINUX_S_IFDIR;
    case EntryKind::Symlink: return LINUX_S_IFLNK;
    case EntryKind::Character: return LINUX_S_IFCHR;
    case EntryKind::Block: return LINUX_S_IFBLK;
    case EntryKind::Fifo: return LINUX_S_IFIFO;
    case EntryKind::Hardlink: break;
    }
    throw format_error("OCI ext2: invalid inode kind");
}
int dirent_type(EntryKind kind) {
    switch(kind) {
    case EntryKind::Regular: return EXT2_FT_REG_FILE;
    case EntryKind::Directory: return EXT2_FT_DIR;
    case EntryKind::Symlink: return EXT2_FT_SYMLINK;
    case EntryKind::Character: return EXT2_FT_CHRDEV;
    case EntryKind::Block: return EXT2_FT_BLKDEV;
    case EntryKind::Fifo: return EXT2_FT_FIFO;
    case EntryKind::Hardlink: break;
    }
    throw format_error("OCI ext2: invalid directory entry kind");
}
}

struct OciExt2Builder::Impl {
    struct Object { ext2_ino_t ino; EntryKind kind; };
    ext2_filsys fs=nullptr;
    std::string raw_path;
    uint64_t virtual_size;
    std::map<std::string,Object> paths;
    std::map<ext2_ino_t,std::vector<PayloadSpan>> current_payloads;

    Impl(const std::string& path,uint64_t size,uint32_t capacity)
        : raw_path(path),virtual_size(size) {
        if(size==0 || size%4096 || size/4096>UINT32_MAX || capacity<16)
            throw format_error("OCI ext2: invalid filesystem capacity");
        Fd file{::open(path.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0644)};
        if(file.fd<0) throw_errno(errno,"create OCI filesystem");
        if(size>static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
           ::ftruncate(file.fd,static_cast<off_t>(size))!=0)
            throw error(EINVAL,"size OCI filesystem");
        struct ext2_super_block param {};
        param.s_rev_level=EXT2_DYNAMIC_REV;
        param.s_log_block_size=param.s_log_cluster_size=2;
        param.s_blocks_count=static_cast<__u32>(size/4096);
        param.s_inodes_count=capacity;
        param.s_inode_size=256;
        param.s_min_extra_isize=param.s_want_extra_isize=sizeof(struct ext2_inode_large)-EXT2_GOOD_OLD_INODE_SIZE;
        param.s_first_ino=EXT2_GOOD_OLD_FIRST_INO;
        param.s_feature_compat=EXT2_FEATURE_COMPAT_EXT_ATTR;
        param.s_feature_incompat=EXT2_FEATURE_INCOMPAT_FILETYPE;
        param.s_feature_ro_compat=EXT2_FEATURE_RO_COMPAT_LARGE_FILE|EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE;
        param.s_errors=EXT2_ERRORS_DEFAULT;
        pin(param);
        check(ext2fs_initialize(path.c_str(),EXT2_FLAG_RW|EXT2_FLAG_EXCLUSIVE,&param,unix_io_manager,&fs),"initialize");
        try {
            fs->now=1;
            pin(*fs->super);
            check(ext2fs_allocate_tables(fs),"allocate tables");
            for(ext2_ino_t ino=1;ino<EXT2_FIRST_INODE(fs->super);++ino)
                if(ino!=EXT2_ROOT_INO) ext2fs_inode_alloc_stats2(fs,ino,1,0);
            check(ext2fs_mkdir(fs,EXT2_ROOT_INO,EXT2_ROOT_INO,nullptr),"create root");
            paths.emplace("",Object{EXT2_ROOT_INO,EntryKind::Directory});
            LayerEntry root;
            root.kind=EntryKind::Directory; root.mode=0755;
            set_metadata(EXT2_ROOT_INO,root);
            flush();
        } catch(...) { ext2fs_close_free(&fs); throw; }
    }
    ~Impl() { if(fs) ext2fs_close_free(&fs); }

    void link(ext2_ino_t parent,const std::string& leaf,ext2_ino_t ino,EntryKind kind) {
        auto rc=ext2fs_link(fs,parent,leaf.c_str(),ino,dirent_type(kind));
        if(rc==EXT2_ET_DIR_NO_SPACE) {
            check(ext2fs_expand_dir(fs,parent),"expand directory");
            rc=ext2fs_link(fs,parent,leaf.c_str(),ino,dirent_type(kind));
        }
        check(rc,"link directory entry");
    }
    ext2_ino_t lookup(ext2_ino_t parent,const std::string& leaf) {
        ext2_ino_t ino=0;
        check(ext2fs_lookup(fs,parent,leaf.c_str(),static_cast<int>(leaf.size()),nullptr,&ino),"lookup created inode");
        return ino;
    }
    void set_metadata(ext2_ino_t ino,const LayerEntry& entry) {
        struct ext2_inode_large inode {};
        auto* small=reinterpret_cast<struct ext2_inode*>(&inode);
        check(ext2fs_read_inode_full(fs,ino,small,sizeof(inode)),"read metadata inode");
        inode.i_mode=static_cast<__u16>((inode.i_mode & LINUX_S_IFMT)|(entry.mode & 07777));
        inode.i_uid=static_cast<__u16>(entry.uid);
        inode.i_gid=static_cast<__u16>(entry.gid);
        ext2fs_set_i_uid_high(inode,static_cast<__u16>(entry.uid>>16));
        ext2fs_set_i_gid_high(inode,static_cast<__u16>(entry.gid>>16));
        if(entry.mtime_seconds < -2147483648LL || entry.mtime_seconds > 15032385535LL ||
           entry.mtime_nanoseconds >= 1000000000U)
            throw format_error("OCI ext2: mtime exceeds large-inode timestamp range");
        inode.i_extra_isize=sizeof(inode)-EXT2_GOOD_OLD_INODE_SIZE;
        inode.i_atime=inode.i_ctime=inode.i_crtime=0;
        inode.i_atime_extra=inode.i_ctime_extra=inode.i_crtime_extra=0;
        inode.i_mtime=static_cast<uint32_t>(entry.mtime_seconds);
        const int64_t signed_low = inode.i_mtime & 0x80000000U ?
            static_cast<int64_t>(inode.i_mtime)-4294967296LL : inode.i_mtime;
        inode.i_mtime_extra=(entry.mtime_nanoseconds<<2)|
            static_cast<uint32_t>((entry.mtime_seconds-signed_low)/4294967296LL);
        check(ext2fs_write_inode_full(fs,ino,small,sizeof(inode)),"write metadata inode");
        if(entry.kind==EntryKind::Directory || !entry.xattrs.empty()) {
            struct ext2_xattr_handle* handle=nullptr;
            check(ext2fs_xattrs_open(fs,ino,&handle),"open xattrs");
            try {
                check(ext2fs_xattrs_read(handle),"read xattrs");
                if(entry.kind==EntryKind::Directory) {
                    // An explicit directory entry replaces its attributes,
                    // including attributes absent from the new layer. Gather
                    // names before removal, which mutates the handle's array.
                    std::vector<std::string> previous;
                    check(ext2fs_xattrs_iterate(handle,
                        [](char* name,char*,size_t,void* context) {
                            static_cast<std::vector<std::string>*>(context)->emplace_back(name);
                            return 0;
                        },&previous),"list prior directory xattrs");
                    for(const auto& name:previous)
                        check(ext2fs_xattr_remove(handle,name.c_str()),"remove prior directory xattr");
                }
                for(const auto& [name,value]:entry.xattrs)
                    check(ext2fs_xattr_set(handle,name.c_str(),value.data(),value.size()),"set xattr");
                check(ext2fs_xattrs_write(handle),"write xattrs");
            } catch(...) { ext2fs_xattrs_close(&handle); throw; }
            check(ext2fs_xattrs_close(&handle),"close xattrs");
        }
    }
    ext2_ino_t ensure_dir(const std::string& path) {
        auto found=paths.find(path);
        if(found!=paths.end()) {
            if(found->second.kind!=EntryKind::Directory)
                throw format_error("OCI ext2: parent is not a directory: "+path);
            return found->second.ino;
        }
        // Iterate components; never follow a symlink and never recurse on
        // untrusted archive nesting depth.
        std::string current;
        ext2_ino_t parent=EXT2_ROOT_INO;
        size_t pos=0;
        while(pos<path.size()) {
            const auto slash=path.find('/',pos);
            const auto component=path.substr(pos,slash==std::string::npos ? path.size()-pos : slash-pos);
            if(!current.empty()) current+='/';
            current+=component;
            found=paths.find(current);
            if(found!=paths.end()) {
                if(found->second.kind!=EntryKind::Directory)
                    throw format_error("OCI ext2: parent is not a directory: "+current);
                parent=found->second.ino;
            } else {
                struct ext2_inode inode {};
                check(ext2fs_read_inode(fs,parent,&inode),"read parent links");
                if(inode.i_links_count==UINT16_MAX) throw format_error("OCI ext2: too many directory links");
                auto rc=ext2fs_mkdir(fs,parent,0,component.c_str());
                if(rc==EXT2_ET_DIR_NO_SPACE) {
                    check(ext2fs_expand_dir(fs,parent),"expand parent");
                    rc=ext2fs_mkdir(fs,parent,0,component.c_str());
                }
                check(rc,"create directory");
                parent=lookup(parent,component);
                paths.emplace(current,Object{parent,EntryKind::Directory});
                LayerEntry entry;
                entry.kind=EntryKind::Directory; entry.mode=0755;
                set_metadata(parent,entry);
            }
            if(slash==std::string::npos) break;
            pos=slash+1;
        }
        return parent;
    }
    void erase_one(const std::string& path) {
        const auto found=paths.find(path);
        if(found==paths.end()) return;
        if(path.empty()) throw format_error("OCI ext2: cannot unlink root");
        const auto object=found->second;
        const auto parent=paths.at(parent_of(path)).ino;
        check(ext2fs_unlink(fs,parent,leaf_of(path).c_str(),object.ino,0),"unlink entry");
        struct ext2_inode_large inode {};
        check(ext2fs_read_inode_full(fs,object.ino,reinterpret_cast<struct ext2_inode*>(&inode),sizeof(inode)),"read removed inode");
        if(object.kind==EntryKind::Directory) {
            inode.i_links_count=0;
            struct ext2_inode parent_inode {};
            check(ext2fs_read_inode(fs,parent,&parent_inode),"read removed directory parent");
            if(parent_inode.i_links_count<=2) throw format_error("OCI ext2: invalid parent link count");
            --parent_inode.i_links_count;
            check(ext2fs_write_inode(fs,parent,&parent_inode),"write parent links");
        } else {
            if(inode.i_links_count==0) throw format_error("OCI ext2: zero-link live inode");
            --inode.i_links_count;
        }
        if(inode.i_links_count==0) {
            // Same lifecycle as e2fsprogs misc/fuse2fs.c remove_inode:
            // free xattrs, punch valid data/indirect blocks, release bitmap.
            check(ext2fs_free_ext_attr(fs,object.ino,&inode),"free xattrs");
            auto* small=reinterpret_cast<struct ext2_inode*>(&inode);
            if(ext2fs_inode_has_valid_blocks2(fs,small))
                check(ext2fs_punch(fs,object.ino,small,nullptr,0,~uint64_t(0)),"free inode blocks");
            ext2fs_inode_alloc_stats2(fs,object.ino,-1,object.kind==EntryKind::Directory);
            current_payloads.erase(object.ino);
            // No directory entry or allocation now references this inode.
            // Clear it completely: a small deterministic i_dtime would be
            // interpreted as an orphan-list inode number by e2fsck pass 1.
            inode = {};
        }
        inode.i_ctime=0;
        check(ext2fs_write_inode_full(fs,object.ino,reinterpret_cast<struct ext2_inode*>(&inode),sizeof(inode)),"write removed inode");
        paths.erase(found);
    }
    void remove(const std::string& path,bool keep_directory=false) {
        auto found=paths.find(path);
        if(found==paths.end()) return;
        if(found->second.kind==EntryKind::Directory) {
            std::vector<std::string> children;
            const std::string prefix=path.empty() ? "" : path+"/";
            for(auto it=paths.lower_bound(prefix);it!=paths.end() && it->first.compare(0,prefix.size(),prefix)==0;++it)
                if(it->first!=path) children.push_back(it->first);
            // Reverse lexical order places descendants before their parent.
            for(auto it=children.rbegin();it!=children.rend();++it) erase_one(*it);
        } else if(keep_directory) {
            throw format_error("OCI ext2: opaque marker names non-directory");
        }
        if(!keep_directory) erase_one(path);
    }
    void write_regular(ext2_ino_t ino,const LayerEntry& entry,int tar_fd,uint64_t tar_size) {
        ext2_file_t file=nullptr;
        check(ext2fs_file_open(fs,ino,EXT2_FILE_WRITE,&file),"open regular file");
        try {
            std::vector<uint8_t> buffer(1<<20);
            uint64_t end=0;
            for(const auto& span:entry.payload_spans) {
                if(span.logical_offset<end || span.logical_offset>entry.logical_size ||
                   span.length>entry.logical_size-span.logical_offset || span.tar_offset>tar_size ||
                   span.length>tar_size-span.tar_offset)
                    throw format_error("OCI ext2: invalid payload span");
                check(ext2fs_file_llseek(file,span.logical_offset,EXT2_SEEK_SET,nullptr),"seek sparse span");
                for(uint64_t done=0;done<span.length;) {
                    const auto n=static_cast<size_t>(std::min<uint64_t>(buffer.size(),span.length-done));
                    read_at(tar_fd,buffer.data(),n,span.tar_offset+done);
                    size_t written=0;
                    while(written<n) {
                        unsigned amount=0;
                        check(ext2fs_file_write(file,buffer.data()+written,static_cast<unsigned>(n-written),&amount),"write regular payload");
                        if(amount==0) throw error(EIO,"OCI ext2: short payload write");
                        written+=amount;
                    }
                    done+=n;
                }
                end=span.logical_offset+span.length;
            }
            check(ext2fs_file_set_size2(file,entry.logical_size),"set logical file size");
        } catch(...) { ext2fs_file_close(file); throw; }
        check(ext2fs_file_close(file),"close regular file");
        current_payloads[ino]=entry.payload_spans;
    }
    void materialize(const LayerEntry& entry,int tar_fd,uint64_t tar_size) {
        if(entry.kind==EntryKind::Directory) {
            auto it=paths.find(entry.path);
            if(it!=paths.end() && it->second.kind!=EntryKind::Directory) remove(entry.path);
            const auto ino=ensure_dir(entry.path);
            set_metadata(ino,entry);
            return;
        }
        if(entry.path.empty()) throw format_error("OCI ext2: file names root");
        const auto parent=ensure_dir(parent_of(entry.path));
        remove(entry.path);
        ext2_ino_t ino=0;
        if(entry.kind==EntryKind::Symlink) {
            auto rc=ext2fs_symlink(fs,parent,0,leaf_of(entry.path).c_str(),entry.link_target.c_str());
            if(rc==EXT2_ET_DIR_NO_SPACE) {
                check(ext2fs_expand_dir(fs,parent),"expand symlink parent");
                rc=ext2fs_symlink(fs,parent,0,leaf_of(entry.path).c_str(),entry.link_target.c_str());
            }
            check(rc,"create symlink");
            ino=lookup(parent,leaf_of(entry.path));
        } else {
            check(ext2fs_new_inode(fs,parent,inode_type(entry.kind)|entry.mode,nullptr,&ino),"allocate inode");
            struct ext2_inode inode {};
            inode.i_mode=static_cast<__u16>(inode_type(entry.kind)|entry.mode);
            inode.i_links_count=1;
            if(entry.kind==EntryKind::Character || entry.kind==EntryKind::Block) {
                const uint32_t major=entry.device_major, minor=entry.device_minor;
                if(major>0xfff || minor>0xfffff) throw format_error("OCI ext2: device number exceeds Linux encoding");
                if(major<256 && minor<256) inode.i_block[0]=(major<<8)|minor;
                else inode.i_block[1]=(minor&0xff)|(major<<8)|((minor&~0xffu)<<12);
            }
            check(ext2fs_write_new_inode(fs,ino,&inode),"write new inode");
            ext2fs_inode_alloc_stats2(fs,ino,1,0);
            link(parent,leaf_of(entry.path),ino,entry.kind);
            if(entry.kind==EntryKind::Regular) write_regular(ino,entry,tar_fd,tar_size);
        }
        paths.emplace(entry.path,Object{ino,entry.kind});
        set_metadata(ino,entry);
    }
    bool hardlink(const LayerEntry& entry) {
        const auto target=paths.find(entry.link_target);
        if(target==paths.end()) return false;
        const Object object=target->second;
        if(object.kind==EntryKind::Directory) throw format_error("OCI ext2: hardlink to directory");
        if(entry.path==entry.link_target || entry.link_target.rfind(entry.path+"/",0)==0)
            throw format_error("OCI ext2: hardlink replacement removes its own target");
        const auto parent=ensure_dir(parent_of(entry.path));
        remove(entry.path);
        struct ext2_inode inode {};
        check(ext2fs_read_inode(fs,object.ino,&inode),"read hardlink target");
        if(inode.i_links_count==UINT16_MAX) throw format_error("OCI ext2: too many hardlinks");
        link(parent,leaf_of(entry.path),object.ino,object.kind);
        ++inode.i_links_count;
        check(ext2fs_write_inode(fs,object.ino,&inode),"write hardlink count");
        paths.emplace(entry.path,object);
        set_metadata(object.ino,entry);
        return true;
    }
    void flush() {
        std::set<ext2_ino_t> seen;
        for(const auto& [path,object]:paths) {
            (void)path;
            if(!seen.insert(object.ino).second) continue;
            struct ext2_inode inode {};
            check(ext2fs_read_inode(fs,object.ino,&inode),"read inode times");
            inode.i_atime=inode.i_ctime=0;
            check(ext2fs_write_inode(fs,object.ino,&inode),"pin inode times");
        }
        pin(*fs->super);
        check(ext2fs_write_bitmaps(fs),"write bitmaps");
        check(ext2fs_flush(fs),"flush filesystem");
        check(io_channel_flush(fs->io),"flush filesystem IO");
        Fd file{::open(raw_path.c_str(),O_RDWR|O_CLOEXEC)};
        if(file.fd<0) throw_errno(errno,"open filesystem for timestamp pinning");
        // No sparse-super feature is enabled: every group has a backup.
        for(dgrp_t group=0;group<fs->group_desc_count;++group) {
            const uint64_t offset=group==0 ? 1024 :
                (uint64_t(fs->super->s_first_data_block)+uint64_t(group)*fs->super->s_blocks_per_group)*4096;
            struct ext2_super_block super {};
            read_at(file.fd,&super,sizeof(super),offset);
            pin(super);
            write_at(file.fd,&super,sizeof(super),offset);
        }
        pin(*fs->super);
        if(::fsync(file.fd)!=0) throw_errno(errno,"sync OCI filesystem");
    }
    std::vector<bytes::segment_mapping> payload_mappings() {
        std::vector<bytes::segment_mapping> out;
        std::set<ext2_ino_t> live;
        for(const auto& [path,object]:paths) { (void)path; live.insert(object.ino); }
        for(const auto& [ino,spans]:current_payloads) {
            if(!live.count(ino)) continue;
            struct ext2_inode inode {};
            check(ext2fs_read_inode(fs,ino,&inode),"read payload inode");
            for(const auto& span:spans) {
                if(span.logical_offset>UINT64_MAX-511) continue;
                uint64_t logical=(span.logical_offset+511)/512*512;
                const uint64_t end=span.logical_offset+span.length;
                if(logical>end || end-logical<512) continue;
                uint64_t target=span.tar_offset+(logical-span.logical_offset);
                if(target%512) continue;  // mixed/unaligned sparse sectors remain metadata
                while(end-logical>=512) {
                    blk64_t physical=0;
                    int flags=0;
                    check(ext2fs_bmap2(fs,ino,&inode,nullptr,0,logical/4096,&flags,&physical),"map payload block");
                    if(physical==0 || flags!=0) throw format_error("OCI ext2: unallocated payload block");
                    const uint32_t length=static_cast<uint32_t>(std::min<uint64_t>((end-logical)/512,8-(logical/512)%8));
                    bytes::segment_mapping m{physical*8+(logical/512)%8,length,target/512,false,1};
                    if(!out.empty() && out.back().end()==m.offset && out.back().mend()==m.moffset &&
                       m.length<=bytes::segment_mapping::kMaxLength-out.back().length) out.back().length+=m.length;
                    else out.push_back(m);
                    if(out.size()>format::lsmt::kMaxRoIndexSize) throw format_error("OCI ext2: payload index too large");
                    logical+=uint64_t(length)*512;
                    target+=uint64_t(length)*512;
                }
            }
        }
        std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return a.offset<b.offset;});
        for(size_t i=1;i<out.size();++i)
            if(out[i-1].end()>out[i].offset) throw format_error("OCI ext2: overlapping payload mappings");
        return out;
    }
    std::vector<bytes::segment_mapping> apply(const LayerPlan& plan,const std::string& tar_path) {
        Fd tar{::open(tar_path.c_str(),O_RDONLY|O_CLOEXEC)};
        if(tar.fd<0) throw_errno(errno,"open OCI layer payload");
        struct stat st {};
        if(::fstat(tar.fd,&st)!=0) throw_errno(errno,"stat OCI layer payload");
        if(!S_ISREG(st.st_mode)||st.st_size<0) throw format_error("OCI ext2: payload is not regular");
        current_payloads.clear();
        for(const auto& path:plan.whiteout_removals) remove(path);
        const std::set<std::string> opaque(plan.opaque_directories.begin(),
                                           plan.opaque_directories.end());
        std::set<std::string> explicit_directories;
        for(const auto& entry:plan.entries)
            if(entry.kind==EntryKind::Directory) explicit_directories.insert(entry.path);
        // First remove only lower objects. An explicit upper directory may
        // replace a lower symlink in an opaque path's ancestry; remove that
        // exact object without resolving through it or creating anything.
        for(const auto& path:opaque) {
            for(auto parent=parent_of(path);!parent.empty();parent=parent_of(parent)) {
                const auto existing=paths.find(parent);
                if(explicit_directories.count(parent) && existing!=paths.end() &&
                   existing->second.kind!=EntryKind::Directory)
                    remove(parent);
            }
            const auto existing=paths.find(path);
            if(existing!=paths.end())
                remove(path,existing->second.kind==EntryKind::Directory);
        }
        // Only after every lower removal is complete establish marker
        // directories. Lexical order places each parent before descendants,
        // making nested opaque markers independent of their archive order.
        for(const auto& path:opaque) ensure_dir(path);
        std::map<std::string,const LayerEntry*> pending;
        for(const auto& entry:plan.entries) {
            pending.erase(entry.path);
            if(entry.kind!=EntryKind::Directory) {
                const std::string prefix=entry.path+"/";
                auto it=pending.lower_bound(prefix);
                while(it!=pending.end() && it->first.compare(0,prefix.size(),prefix)==0)
                    it=pending.erase(it);
            }
            if(entry.kind==EntryKind::Hardlink) {
                if(!hardlink(entry)) { remove(entry.path); pending[entry.path]=&entry; }
            } else materialize(entry,tar.fd,static_cast<uint64_t>(st.st_size));
            bool progress=true;
            while(progress) {
                progress=false;
                for(auto it=pending.begin();it!=pending.end();) {
                    if(hardlink(*it->second)) { it=pending.erase(it); progress=true; }
                    else ++it;
                }
            }
        }
        if(!pending.empty()) throw format_error("OCI ext2: unresolved or cyclic hardlink target");
        auto mappings=payload_mappings();
        flush();
        return mappings;
    }
};
#else
struct OciExt2Builder::Impl {};
#endif

OciExt2Builder::OciExt2Builder(const std::string& raw_path,uint64_t virtual_size,uint32_t capacity) {
#if OBD_HAVE_LIBE2FS
    impl_=std::make_unique<Impl>(raw_path,virtual_size,capacity);
#else
    (void)raw_path; (void)virtual_size; (void)capacity;
    throw format_error("OCI ext2 builder requires libe2fs support");
#endif
}
OciExt2Builder::~OciExt2Builder()=default;
std::vector<bytes::segment_mapping> OciExt2Builder::apply(const LayerPlan& plan,const std::string& tar_path) {
#if OBD_HAVE_LIBE2FS
    return impl_->apply(plan,tar_path);
#else
    (void)plan; (void)tar_path;
    throw format_error("OCI ext2 builder requires libe2fs support");
#endif
}
}  // namespace obd::convert
