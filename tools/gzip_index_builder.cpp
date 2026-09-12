#include "gzip_index_builder.hpp"
#include "common/crc32c.hpp"
#include "common/errors.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <unistd.h>
#include <sys/stat.h>
#include <vector>
#include <zlib.h>

namespace obd::convert {
namespace {
constexpr size_t header_size=333, window_size=32768, entry_size=29;
constexpr size_t max_entries_bytes=64*1024*1024;
void put(uint8_t* p, uint64_t value, size_t n) {
    for (size_t i=0;i<n;++i) p[i]=static_cast<uint8_t>(value>>(8*i));
}
struct Inflate {
    z_stream z{};
    bool active=false;
    ~Inflate() { if(active) inflateEnd(&z); }
};
struct File {
    FILE* p=nullptr;
    ~File() { if(p) fclose(p); }
};
struct Temporary {
    std::string path;
    ~Temporary() { if(!path.empty()) unlink(path.c_str()); }
};
void write(FILE* f, const uint8_t* p, size_t n) {
    if(fwrite(p,1,n,f)!=n) throw error(errno ? errno:EIO,"write gzip index");
}
std::vector<uint8_t> compress(const uint8_t* p, size_t n) {
    std::vector<uint8_t> out(compressBound(n));
    uLongf size=out.size();
    if(compress2(out.data(),&size,p,n,6)!=Z_OK) throw error(EIO,"compress gzip index");
    out.resize(size);
    return out;
}
}

void build_gzip_index(const std::string& gzip_path, const std::string& index_path,
                      uint32_t span) {
    if(span<65536 || span>INT32_MAX) throw error(EINVAL,"invalid gzip index span");
    File input{fopen(gzip_path.c_str(),"rb")};
    if(!input.p) throw error(errno,"open gzip input");
    struct stat input_stat {}, output_stat {};
    if (fstat(fileno(input.p), &input_stat) != 0)
        throw error(errno, "stat gzip input");
    if (stat(index_path.c_str(), &output_stat) == 0 &&
        input_stat.st_dev == output_stat.st_dev && input_stat.st_ino == output_stat.st_ino)
        throw error(EINVAL, "gzip index output aliases input");
    Temporary temporary{index_path+".tmp.XXXXXX"};
    std::vector<char> name(temporary.path.begin(),temporary.path.end()); name.push_back(0);
    int fd=mkstemp(name.data());
    if(fd<0) { temporary.path.clear(); throw error(errno,"create gzip index temporary"); }
    temporary.path=name.data();
    File output{fdopen(fd,"w+b")};
    if(!output.p) { const int saved=errno; close(fd); throw error(saved,"open gzip index temporary"); }
    std::array<uint8_t,header_size> header{};
    write(output.p,header.data(),header.size());
    Inflate inflater;
    if(inflateInit2(&inflater.z,31)!=Z_OK) throw error(ENOMEM,"initialize gzip inflater");
    inflater.active=true;
    auto& z=inflater.z;
    std::array<uint8_t,65536> input_buffer{};
    std::array<uint8_t,window_size> window{}, dictionary{};
    std::vector<uint8_t> entries;
    uint64_t input_total=0, output_total=0, dictionary_end=header_size, last_output=0;
    z.next_out=window.data(); z.avail_out=window.size();
    for (;;) {
        if(!z.avail_in) {
            size_t n=fread(input_buffer.data(),1,input_buffer.size(),input.p);
            if(!n) {
                if(ferror(input.p)) throw error(errno ? errno:EIO,"read gzip input");
                throw format_error("truncated gzip stream");
            }
            z.next_in=input_buffer.data(); z.avail_in=n;
        }
        if(!z.avail_out) { z.next_out=window.data(); z.avail_out=window.size(); }
        const auto before_in=z.avail_in, before_out=z.avail_out;
        const int status=inflate(&z,Z_BLOCK);
        input_total+=before_in-z.avail_in;
        output_total+=before_out-z.avail_out;
        if(output_total>INT64_MAX || input_total>INT64_MAX) throw format_error("gzip size exceeds ddgzidx limits");
        if(status==Z_STREAM_END) {
            // inflateInit2(31) verifies the member CRC32 and ISIZE. Refuse
            // extra data instead of silently indexing just its first member.
            if(z.avail_in || fgetc(input.p)!=EOF) throw format_error("concatenated or trailing gzip data is unsupported");
            if(ferror(input.p)) throw error(errno ? errno:EIO,"read gzip trailer");
            break;
        }
        if(status!=Z_OK && status!=Z_BUF_ERROR) throw format_error("invalid gzip stream or trailer");
        if(before_in==z.avail_in && before_out==z.avail_out) throw format_error("gzip inflater made no progress");
        if((z.data_type&128) && !(z.data_type&64) &&
           (entries.empty() || output_total-last_output>=span)) {
            if(entries.size()>max_entries_bytes-entry_size) throw error(EFBIG,"gzip index metadata exceeds 64 MiB");
            const size_t left=z.avail_out;
            std::memcpy(dictionary.data(),window.data()+window_size-left,left);
            std::memcpy(dictionary.data()+left,window.data(),window_size-left);
            auto packed=compress(dictionary.data(),dictionary.size());
            const size_t at=entries.size(); entries.resize(at+entry_size);
            put(entries.data()+at,output_total,8); put(entries.data()+at+8,input_total,8);
            put(entries.data()+at+16,dictionary_end,8);
            entries[at+24]=z.data_type&7;
            put(entries.data()+at+25,packed.size(),4);
            write(output.p,packed.data(),packed.size()); dictionary_end+=packed.size();
            last_output=output_total;
        }
    }
    if(entries.empty()) throw format_error("gzip stream has no restart checkpoint");
    auto packed_entries=compress(entries.data(),entries.size());
    if(packed_entries.size()>max_entries_bytes) throw error(EFBIG,"compressed gzip index exceeds 64 MiB");
    write(output.p,packed_entries.data(),packed_entries.size());
    std::memcpy(header.data(),"ddgzidx",7);
    header[8]=1; header[10]=1; header[11]=6;
    put(header.data()+13,span,4); put(header.data()+17,window_size,4);
    put(header.data()+21,entry_size,4); put(header.data()+25,entries.size()/entry_size,8);
    put(header.data()+33,input_total,8); put(header.data()+41,dictionary_end+packed_entries.size(),8);
    put(header.data()+49,output_total,8); put(header.data()+313,dictionary_end,8);
    put(header.data()+321,packed_entries.size(),8);
    put(header.data()+329,crc32::crc32c(header.data(),329),4);
    if(fseeko(output.p,0,SEEK_SET)!=0) throw error(errno,"seek gzip index header");
    write(output.p,header.data(),header.size());
    if(fflush(output.p)!=0 || fsync(fileno(output.p))!=0) throw error(errno,"flush gzip index");
    FILE* closing=output.p; output.p=nullptr;
    if(fclose(closing)!=0) throw error(errno,"close gzip index");
    if(rename(temporary.path.c_str(),index_path.c_str())!=0) throw error(errno,"publish gzip index");
    temporary.path.clear();
}
} // namespace obd::convert
