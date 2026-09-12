#include "turbo_package.hpp"
#include "common/errors.hpp"
#include <array>
#include <algorithm>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <zlib.h>

namespace obd::convert {
namespace {
struct File {
    FILE* p=nullptr;
    ~File() { if(p) fclose(p); }
};
struct Temporary {
    std::string path;
    ~Temporary() { if(!path.empty()) unlink(path.c_str()); }
};
class GzipWriter {
    FILE* file_;
    z_stream z_{};
    bool active_=false;
    std::array<uint8_t,65536> output_{};
public:
    explicit GzipWriter(FILE* file):file_(file) {
        if(deflateInit2(&z_,6,Z_DEFLATED,31,8,Z_DEFAULT_STRATEGY)!=Z_OK)
            throw error(ENOMEM,"initialize TurboOCI gzip writer");
        active_=true;
        // zlib's default gzip header has MTIME zero and no optional fields.
    }
    ~GzipWriter() { if(active_) deflateEnd(&z_); }
    void write(const void* data,size_t size,bool finish=false) {
        z_.next_in=const_cast<Bytef*>(static_cast<const Bytef*>(data));
        z_.avail_in=static_cast<uInt>(size);
        int status;
        do {
            z_.next_out=output_.data(); z_.avail_out=output_.size();
            status=deflate(&z_,finish ? Z_FINISH:Z_NO_FLUSH);
            if(status!=Z_OK && status!=Z_STREAM_END) throw error(EIO,"compress TurboOCI package");
            size_t produced=output_.size()-z_.avail_out;
            if(fwrite(output_.data(),1,produced,file_)!=produced)
                throw error(errno ? errno:EIO,"write TurboOCI package");
        } while(z_.avail_in || (finish && status!=Z_STREAM_END));
    }
};
void octal(uint8_t* out,size_t length,uint64_t value) {
    std::memset(out,'0',length-1); out[length-1]=0;
    for(size_t i=length-1;value && i>0;) { out[--i]='0'+(value&7); value>>=3; }
    if(value) throw error(EFBIG,"TurboOCI tar field exceeds USTAR limits");
}
void header(GzipWriter& gzip,const char* name,uint64_t size) {
    std::array<uint8_t,512> h{};
    std::memcpy(h.data(),name,std::strlen(name));
    octal(h.data()+100,8,0644); octal(h.data()+108,8,0); octal(h.data()+116,8,0);
    octal(h.data()+124,12,size); octal(h.data()+136,12,0);
    std::memset(h.data()+148,' ',8); h[156]='0';
    std::memcpy(h.data()+257,"ustar",5); std::memcpy(h.data()+263,"00",2);
    octal(h.data()+329,8,0); octal(h.data()+337,8,0);
    unsigned checksum=0; for(auto byte:h) checksum+=byte;
    octal(h.data()+148,7,checksum); h[155]=' ';
    gzip.write(h.data(),h.size());
}
uint64_t size_of(FILE* file) {
    struct stat st{};
    if(fstat(fileno(file),&st)!=0) throw error(errno,"stat TurboOCI input");
    if(!S_ISREG(st.st_mode) || st.st_size<0) throw error(EINVAL,"TurboOCI input must be a regular file");
    return static_cast<uint64_t>(st.st_size);
}
void member(GzipWriter& gzip,FILE* file,const char* name) {
    uint64_t left=size_of(file);
    const uint64_t size=left;
    header(gzip,name,size);
    std::array<uint8_t,65536> buffer{};
    while(left) {
        size_t count=static_cast<size_t>(std::min<uint64_t>(left,buffer.size()));
        if(fread(buffer.data(),1,count,file)!=count)
            throw error(errno ? errno:EIO,"read TurboOCI input");
        gzip.write(buffer.data(),count); left-=count;
    }
    if(fgetc(file)!=EOF || ferror(file)) throw error(EIO,"TurboOCI input changed while packaging");
    const size_t padding=(512-size%512)%512;
    std::array<uint8_t,512> zero{};
    if(padding) gzip.write(zero.data(),padding);
}
void reject_alias(const std::string& input,const std::string& output) {
    if(input.empty()) return;
    std::error_code ec;
    const bool same=std::filesystem::equivalent(input,output,ec);
    if(same || std::filesystem::weakly_canonical(input)==std::filesystem::weakly_canonical(output))
        throw error(EINVAL,"TurboOCI package output aliases an input");
}
}

void write_turbo_package(const std::string& metadata_path,const std::string& gzip_index_path,
                         const std::string& output_path) {
    reject_alias(metadata_path,output_path); reject_alias(gzip_index_path,output_path);
    File metadata{fopen(metadata_path.c_str(),"rb")};
    if(!metadata.p) throw error(errno,"open TurboOCI metadata");
    File index{gzip_index_path.empty() ? nullptr:fopen(gzip_index_path.c_str(),"rb")};
    if(!gzip_index_path.empty() && !index.p) throw error(errno,"open TurboOCI gzip index");
    Temporary temporary{output_path+".tmp.XXXXXX"};
    std::vector<char> name(temporary.path.begin(),temporary.path.end()); name.push_back(0);
    int fd=mkstemp(name.data());
    if(fd<0) { temporary.path.clear(); throw error(errno,"create TurboOCI package temporary"); }
    temporary.path=name.data();
    File output{fdopen(fd,"wb")};
    if(!output.p) { int saved=errno; close(fd); throw error(saved,"open TurboOCI package temporary"); }
    {
        GzipWriter gzip(output.p);
        member(gzip,metadata.p,"ext4.fs.meta");
        header(gzip,".turbo.ociv1",0);
        if(index.p) member(gzip,index.p,"gzip.meta");
        std::array<uint8_t,1024> zero{};
        gzip.write(zero.data(),zero.size());
        gzip.write(nullptr,0,true);
    }
    if(fflush(output.p)!=0 || fsync(fileno(output.p))!=0) throw error(errno,"flush TurboOCI package");
    FILE* closing=output.p; output.p=nullptr;
    if(fclose(closing)!=0) throw error(errno,"close TurboOCI package");
    if(rename(temporary.path.c_str(),output_path.c_str())!=0) throw error(errno,"publish TurboOCI package");
    temporary.path.clear();
}
} // namespace obd::convert
