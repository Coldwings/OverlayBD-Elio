// Ordinary-file interoperability fixture using the pinned upstream LSMT API.
// Creates only a fresh caller-owned directory; no block device or daemon.
#include "file.h"
#include "index.h"
#include <photon/photon.h>
#include <photon/fs/localfs.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/falloc.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr size_t kSector = 512, kSectors = 64, kBytes = kSector * kSectors;
struct Op { size_t sector, count; uint8_t value; bool zero; };
const std::array<Op, 8> ops = {{{1,2,0x11,false}, {8,3,0x22,false},
    {2,1,0x33,false}, {9,1,0,true}, {9,1,0x44,false},
    {10,1,0,true}, {16,2,0,true}, {63,1,0x77,false}}};
void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(std::string(why) + ": errno=" + std::to_string(errno));
}
std::array<uint8_t,kBytes> expected_bytes() {
    std::array<uint8_t,kBytes> out{};
    for (const auto& op : ops)
        std::fill_n(out.begin()+op.sector*kSector, op.count*kSector, op.zero?0:op.value);
    return out;
}
std::array<uint8_t,kSectors> expected_states() {
    std::array<uint8_t,kSectors> out{};
    for (const auto& op : ops)
        std::fill_n(out.begin()+op.sector, op.count, op.zero?2:1);
    return out;
}
void raw_file(const std::string& path, const void* data, size_t size) {
    const int fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    require(fd>=0,"oracle create");
    const auto* p=static_cast<const uint8_t*>(data);
    size_t done=0;
    while(done<size) {
        const auto n=::write(fd,p+done,size-done);
        if(n<0 && errno==EINTR) continue;
        require(n>0,"oracle write"); done+=static_cast<size_t>(n);
    }
    require(::fsync(fd)==0,"oracle sync"); require(::close(fd)==0,"oracle close");
}
void verify(LSMT::IFileRO& file) {
    std::array<uint8_t,kBytes> actual{};
    require(file.pread(actual.data(),actual.size(),0)==static_cast<ssize_t>(actual.size()),"upstream read");
    require(actual==expected_bytes(),"logical bytes differ");
    LSMT::SegmentMapping maps[128]{};
    const auto n=file.index()->lookup(LSMT::Segment{0,kSectors},maps,128);
    require(n<128,"unexpected mapping count");
    std::array<uint8_t,kSectors> states{};
    for(size_t i=0;i<n;++i) {
        const auto& m=maps[i];
        require(m.end()<=kSectors && m.length>0,"mapping bounds");
        for(uint64_t s=m.offset;s<m.end();++s) states[s]=m.zeroed?2:1;
        std::cout<<"MAP offset="<<uint64_t(m.offset)<<" length="<<uint64_t(m.length)
                 <<" moffset="<<uint64_t(m.moffset)<<" zero="<<bool(m.zeroed)
                 <<" tag="<<uint32_t(m.tag)<<"\n";
    }
    require(states==expected_states(),"INHERIT/DATA/ZERO states differ");
}
void make_set(photon::fs::IFileSystem& fs,const std::string& prefix,bool seal,bool buffered) {
    std::unique_ptr<photon::fs::IFile> data(fs.open((prefix+".data").c_str(),O_RDWR|O_CREAT|O_EXCL,0600));
    std::unique_ptr<photon::fs::IFile> index(fs.open((prefix+".index").c_str(),O_RDWR|O_CREAT|O_EXCL,0600));
    require(data && index,"pair create");
    LSMT::LayerInfo info(data.get(),index.get());
    info.virtual_size=kBytes; info.rw_type=LSMT::RWType::Hybrid;
    require(info.uuid.parse("11111111-2222-4333-8444-555555555555",36)==0,"fixed uuid");
    char tag[]="overlaybd-elio-hybrid-interop-v1"; info.user_tag=tag; info.len=std::strlen(tag);
    std::unique_ptr<LSMT::IFileRW> file(LSMT::create_file_rw(info,false));
    require(bool(file),"upstream Hybrid create");
    if(buffered) require(file->set_index_group_commit(4096)==0,"buffered index");
    for(const auto& op:ops) {
        if(op.zero) {
            require(file->fallocate(FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,
                op.sector*kSector,op.count*kSector)==0,"upstream logical discard");
        } else {
            std::vector<uint8_t> bytes(op.count*kSector,op.value);
            require(file->pwrite(bytes.data(),bytes.size(),op.sector*kSector)==
                static_cast<ssize_t>(bytes.size()),"upstream write");
        }
    }
    require(file->fsync()==0,"upstream pair sync");
    std::cout<<"VERIFY "<<prefix<<"\n"; verify(*file);
    if(seal) {
        require(file->close_seal()==0,"upstream close_seal");
        require(data->fsync()==0,"sealed data sync");
    }
}
void check_rw(const std::string& data_path,const std::string& index_path) {
    std::unique_ptr<photon::fs::IFile> data(photon::fs::open_localfile_adaptor(data_path.c_str(),O_RDONLY));
    std::unique_ptr<photon::fs::IFile> index(photon::fs::open_localfile_adaptor(index_path.c_str(),O_RDONLY));
    require(data && index,"pair open");
    std::unique_ptr<LSMT::IFileRW> file(LSMT::open_file_rw(data.get(),index.get(),false));
    require(bool(file),"upstream reopen"); verify(*file);
}
void check_ro(const std::string& path) {
    std::unique_ptr<photon::fs::IFile> data(photon::fs::open_localfile_adaptor(path.c_str(),O_RDONLY));
    require(bool(data),"sealed open");
    std::unique_ptr<LSMT::IFileRO> file(LSMT::open_file_ro(data.get(),false));
    require(bool(file),"upstream sealed reader"); verify(*file);
}
}
int main(int argc,char** argv) {
    if(argc<3) return 64;
    bool initialized=false;
    try {
        require(photon::init(photon::INIT_EVENT_EPOLL,0)==0,"Photon init");
        initialized=true;
        const std::string mode=argv[1];
        if(mode=="produce" && argc==3) {
            require(::mkdir(argv[2],0700)==0,"fresh fixture directory");
            std::unique_ptr<photon::fs::IFileSystem> fs(photon::fs::new_localfs_adaptor(argv[2]));
            require(bool(fs),"local file adaptor");
            make_set(*fs,"writable",false,false);
            make_set(*fs,"buffered",false,true);
            make_set(*fs,"sealed",true,false);
            const auto bytes=expected_bytes(); const auto states=expected_states();
            raw_file(std::string(argv[2])+"/expected.raw",bytes.data(),bytes.size());
            raw_file(std::string(argv[2])+"/expected.states",states.data(),states.size());
        } else if(mode=="check-rw" && argc==4) check_rw(argv[2],argv[3]);
        else if(mode=="check-ro" && argc==3) check_ro(argv[2]);
        else throw std::runtime_error("usage: produce DIR | check-rw DATA INDEX | check-ro DATA");
        photon::fini(); initialized=false;
        std::cout<<"UPSTREAM_HYBRID_FIXTURE_OK "<<mode<<"\n"; return 0;
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<"\n";
        if(initialized) photon::fini();
        return 1;
    }
}
