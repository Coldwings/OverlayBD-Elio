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
#include <sys/syscall.h>
#include <linux/fs.h>
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

namespace {
class GzipReader {
    FILE* file_;
    z_stream z_{};
    bool active_=false, ended_=false;
    std::array<uint8_t,65536> input_{};
public:
    explicit GzipReader(FILE* file):file_(file) {
        if(inflateInit2(&z_,31)!=Z_OK) throw error(ENOMEM,"initialize TurboOCI gzip reader");
        active_=true;
    }
    ~GzipReader() { if(active_) inflateEnd(&z_); }
    size_t read(void* buffer,size_t count) {
        if(ended_) return 0;
        z_.next_out=static_cast<Bytef*>(buffer); z_.avail_out=static_cast<uInt>(count);
        while(z_.avail_out) {
            if(!z_.avail_in) {
                size_t n=fread(input_.data(),1,input_.size(),file_);
                if(!n) {
                    if(ferror(file_)) throw error(errno ? errno:EIO,"read TurboOCI gzip package");
                    throw format_error("truncated TurboOCI gzip stream");
                }
                z_.next_in=input_.data(); z_.avail_in=static_cast<uInt>(n);
            }
            const auto before_in=z_.avail_in, before_out=z_.avail_out;
            const int status=inflate(&z_,Z_NO_FLUSH);
            if(status==Z_STREAM_END) {
                if(z_.avail_in || fgetc(file_)!=EOF)
                    throw format_error("trailing or concatenated TurboOCI gzip data");
                if(ferror(file_)) throw error(errno ? errno:EIO,"read TurboOCI gzip trailer");
                ended_=true;
                break;
            }
            if(status!=Z_OK && status!=Z_BUF_ERROR)
                throw format_error("invalid TurboOCI gzip stream or trailer");
            if(before_in==z_.avail_in && before_out==z_.avail_out)
                throw format_error("TurboOCI gzip inflater made no progress");
        }
        return count-z_.avail_out;
    }
    void exact(void* buffer,size_t count) {
        if(read(buffer,count)!=count) throw format_error("truncated TurboOCI tar archive");
    }
};
struct PrivateDirectory {
    std::string path;
    ~PrivateDirectory() {
        if(!path.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path,ignored);
        }
    }
};
uint64_t parse_octal(const uint8_t* p,size_t n) {
    uint64_t result=0;
    size_t i=0;
    while(i<n && p[i]==' ') ++i;
    for(;i<n && p[i]>='0' && p[i]<='7';++i) result=result*8+p[i]-'0';
    for(;i<n;++i) if(p[i]!=0 && p[i]!=' ')
        throw format_error("invalid TurboOCI USTAR numeric field");
    return result;
}
bool all_zero(const uint8_t* p,size_t n) {
    return std::all_of(p,p+n,[](uint8_t b){return b==0;});
}
std::string member_name(const std::array<uint8_t,512>& h) {
    if(std::memcmp(h.data()+257,"ustar\0",6)!=0 || std::memcmp(h.data()+263,"00",2)!=0)
        throw format_error("TurboOCI archive requires USTAR headers");
    if(!all_zero(h.data()+345,155) || !all_zero(h.data()+157,100) ||
       (h[156]!='0' && h[156]!=0))
        throw format_error("TurboOCI archive requires unprefixed regular files");
    unsigned sum=0;
    for(size_t i=0;i<h.size();++i) sum+=(i>=148 && i<156) ? ' ':h[i];
    if(parse_octal(h.data()+148,8)!=sum) throw format_error("TurboOCI tar checksum mismatch");
    const auto end=std::find(h.begin(),h.begin()+100,0);
    if(end==h.begin()+100 || !all_zero(&*end,static_cast<size_t>(h.begin()+100-end)))
        throw format_error("invalid TurboOCI tar member name");
    return {reinterpret_cast<const char*>(h.data()),static_cast<size_t>(end-h.begin())};
}
}

ImportedTurboPackage import_turbo_package(const std::string& package_path,
                                          const std::string& output_directory) {
    if(output_directory.empty()) throw error(EINVAL,"empty TurboOCI import directory");
    const std::filesystem::path destination(output_directory);
    if(destination.filename().empty() || destination.filename()=="." || destination.filename()=="..")
        throw error(EINVAL,"invalid TurboOCI import directory");
    struct stat existing{};
    if(lstat(output_directory.c_str(),&existing)==0) throw error(EEXIST,"TurboOCI import destination exists");
    if(errno!=ENOENT) throw error(errno,"inspect TurboOCI import destination");
    File input{fopen(package_path.c_str(),"rb")};
    if(!input.p) throw error(errno,"open TurboOCI package");
    PrivateDirectory temporary{output_directory+".tmp.XXXXXX"};
    std::vector<char> name(temporary.path.begin(),temporary.path.end()); name.push_back(0);
    if(!mkdtemp(name.data())) { temporary.path.clear(); throw error(errno,"create TurboOCI import temporary"); }
    temporary.path=name.data();
    GzipReader gzip(input.p);
    bool metadata=false,marker=false,index=false;
    std::array<uint8_t,512> h{};
    std::array<uint8_t,65536> data{};
    for(;;) {
        gzip.exact(h.data(),h.size());
        if(all_zero(h.data(),h.size())) {
            gzip.exact(h.data(),h.size());
            if(!all_zero(h.data(),h.size())) throw format_error("TurboOCI tar requires two zero terminators");
            // Traditional tar pads the final record. Permit bounded zero-only
            // padding and consume through the gzip trailer to verify its CRC.
            size_t padding=0;
            for(;;) {
                size_t n=gzip.read(data.data(),data.size());
                if(!n) break;
                padding+=n;
                if(padding>1024*1024 || !all_zero(data.data(),n))
                    throw format_error("invalid trailing TurboOCI tar data");
            }
            break;
        }
        const std::string member=member_name(h);
        const uint64_t size=parse_octal(h.data()+124,12);
        bool* seen=nullptr;
        if(member=="ext4.fs.meta") seen=&metadata;
        else if(member==".turbo.ociv1") seen=&marker;
        else if(member=="gzip.meta") seen=&index;
        else throw format_error("unsupported TurboOCI tar member: "+member);
        if(*seen) throw format_error("duplicate TurboOCI tar member: "+member);
        *seen=true;
        if(member==".turbo.ociv1" && size!=0) throw format_error("TurboOCI marker must be empty");
        File output;
        if(member!=".turbo.ociv1") {
            const auto path=std::filesystem::path(temporary.path)/member;
            int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
            if(fd<0) throw error(errno,"create imported TurboOCI member");
            output.p=fdopen(fd,"wb");
            if(!output.p) { int saved=errno; close(fd); throw error(saved,"open imported TurboOCI member"); }
        }
        uint64_t left=size;
        while(left) {
            size_t n=static_cast<size_t>(std::min<uint64_t>(left,data.size()));
            gzip.exact(data.data(),n);
            if(fwrite(data.data(),1,n,output.p)!=n) throw error(errno ? errno:EIO,"write imported TurboOCI member");
            left-=n;
        }
        const size_t padding=(512-size%512)%512;
        if(padding) {
            gzip.exact(data.data(),padding);
            if(!all_zero(data.data(),padding)) throw format_error("nonzero TurboOCI member padding");
        }
        if(output.p) {
            if(fflush(output.p)!=0 || fsync(fileno(output.p))!=0) throw error(errno,"flush imported TurboOCI member");
            FILE* closing=output.p; output.p=nullptr;
            if(fclose(closing)!=0) throw error(errno,"close imported TurboOCI member");
        }
    }
    if(!metadata || !marker) throw format_error("TurboOCI package lacks metadata or marker");
    ImportedTurboPackage result{(destination/"ext4.fs.meta").string(),
                                index ? (destination/"gzip.meta").string():""};
    // NOREPLACE preserves an independently created destination in the race
    // between initial validation and publication; ordinary rename can replace
    // an existing empty directory.
    if(syscall(SYS_renameat2,AT_FDCWD,temporary.path.c_str(),AT_FDCWD,
               output_directory.c_str(),RENAME_NOREPLACE)!=0)
        throw error(errno,"publish imported TurboOCI package");
    temporary.path.clear();
    return result;
}
} // namespace obd::convert
