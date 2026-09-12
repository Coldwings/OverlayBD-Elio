#include "oci_layer_plan.hpp"

#include "common/errors.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace obd::convert {
namespace {
constexpr uint64_t kExtensionLimit = 16 * 1024 * 1024;
constexpr uint64_t kMetadataLimit = 64 * 1024 * 1024;
constexpr size_t kEntryLimit = 65536;
constexpr size_t kSpanLimit = 1000000;
using Fields = std::map<std::string, std::string>;
[[noreturn]] void bad(const std::string& why) { throw format_error("OCI tar: " + why); }

uint64_t decimal(std::string_view s) {
    if (s.empty()) bad("empty decimal field");
    uint64_t value = 0;
    for (char c : s) {
        if (c < '0' || c > '9' || value > (UINT64_MAX - uint64_t(c - '0')) / 10)
            bad("invalid or overflowing decimal field");
        value = value * 10 + uint64_t(c - '0');
    }
    return value;
}
uint64_t number(const uint8_t* p, size_t n) {
    if (p[0] & 0x80) {
        if (p[0] & 0x40) bad("negative base-256 field");
        uint64_t value = p[0] & 0x3f;
        for (size_t i = 1; i < n; ++i) {
            if (value > (UINT64_MAX - p[i]) / 256) bad("base-256 field overflow");
            value = value * 256 + p[i];
        }
        return value;
    }
    size_t start = 0;
    while (start < n && (p[start] == ' ' || p[start] == 0)) ++start;
    uint64_t value = 0;
    bool tail = false;
    for (size_t i = start; i < n; ++i) {
        if (p[i] == 0 || p[i] == ' ') { tail = true; continue; }
        if (tail || p[i] < '0' || p[i] > '7' || value > (UINT64_MAX - (p[i]-'0')) / 8)
            bad("invalid octal field");
        value = value * 8 + p[i] - '0';
    }
    return value;
}
// ext4 large-inode timestamps encode signed low seconds plus two epoch bits.
constexpr int64_t kMinTime = -2147483648LL;
constexpr int64_t kMaxTime = 15032385535LL;
void set_time(LayerEntry& entry, int64_t seconds, uint32_t nanoseconds = 0) {
    if(seconds < kMinTime || seconds > kMaxTime)
        bad("mtime exceeds large-inode timestamp range");
    entry.mtime_seconds = seconds;
    entry.mtime_nanoseconds = nanoseconds;
}
void pax_time(LayerEntry& entry, std::string_view value) {
    bool negative = false;
    if(!value.empty() && (value.front()=='-' || value.front()=='+')) {
        negative = value.front()=='-';
        value.remove_prefix(1);
    }
    const auto dot = value.find('.');
    const auto whole = decimal(value.substr(0,dot));
    if(whole > uint64_t(kMaxTime)+1) bad("mtime exceeds large-inode timestamp range");
    uint32_t nanos = 0;
    if(dot != std::string_view::npos) {
        const auto fraction = value.substr(dot+1);
        if(fraction.empty()) bad("empty mtime fraction");
        for(size_t i=0;i<fraction.size();++i) {
            const char digit = fraction[i];
            if(digit<'0' || digit>'9') bad("invalid mtime fraction");
            if(i<9) nanos = nanos*10 + uint32_t(digit-'0');
            else if(digit!='0') bad("mtime precision exceeds nanoseconds");
        }
        for(size_t i=fraction.size();i<9;++i) nanos*=10;
    }
    int64_t seconds = static_cast<int64_t>(whole);
    if(negative) {
        seconds = -seconds;
        if(nanos) { --seconds; nanos = 1000000000U-nanos; }
    }
    set_time(entry,seconds,nanos);
}
void header_time(LayerEntry& entry, const uint8_t* p, size_t n) {
    if((p[0]&0xc0)==0xc0) {
        // GNU negative base-256: the marker bit is separate from sign
        // extension. Supported negative seconds fit signed 32 bits.
        for(size_t i=0;i<n-4;++i) if(p[i]!=0xff) bad("negative mtime out of range");
        uint32_t low=0;
        for(size_t i=n-4;i<n;++i) low=(low<<8)|p[i];
        if(!(low&0x80000000U)) bad("negative mtime out of range");
        set_time(entry,static_cast<int64_t>(low)-4294967296LL);
    } else {
        const auto seconds=number(p,n);
        if(seconds>uint64_t(kMaxTime)) bad("mtime exceeds large-inode timestamp range");
        set_time(entry,static_cast<int64_t>(seconds));
    }
}
std::string field(const uint8_t* p, size_t n) {
    const auto* end = static_cast<const uint8_t*>(std::memchr(p, 0, n));
    return std::string(reinterpret_cast<const char*>(p), end ? size_t(end-p) : n);
}
std::string path(std::string value, bool directory = false) {
    if (value.find('\0') != std::string::npos) bad("NUL in pathname");
    size_t prefix = 0;
    while (value.compare(prefix, 2, "./") == 0) prefix += 2;
    value.erase(0, prefix);
    if (!value.empty() && value.front() == '/') bad("absolute archive pathname");
    while (directory && !value.empty() && value.back() == '/') value.pop_back();
    if (value.empty() || value == ".") {
        if (!directory) bad("non-directory names archive root");
        return {};
    }
    size_t pos = 0;
    while (pos < value.size()) {
        const size_t slash = value.find('/', pos);
        const auto part = std::string_view(value).substr(pos, slash == std::string::npos ? value.size()-pos : slash-pos);
        if (part.empty() || part == "." || part == ".." || part.size() > 255)
            bad("invalid pathname component");
        if (slash == std::string::npos) break;
        pos = slash + 1;
        if (pos == value.size()) bad("trailing slash on non-directory");
    }
    return value;
}

class Reader {
public:
    explicit Reader(const std::string& filename) {
        fd_ = ::open(filename.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) throw_errno(errno, "open OCI tar");
        struct stat st {};
        if (::fstat(fd_, &st) != 0) { const int e=errno; ::close(fd_); throw_errno(e,"stat OCI tar"); }
        if (!S_ISREG(st.st_mode) || st.st_size < 0) { ::close(fd_); bad("input is not a regular file"); }
        size = static_cast<uint64_t>(st.st_size);
    }
    ~Reader() { ::close(fd_); }
    void read(void* out, size_t n, uint64_t at) {
        if (at > size || n > size-at) bad("truncated archive");
        size_t done = 0;
        while (done < n) {
            const auto r = ::pread(fd_, static_cast<uint8_t*>(out)+done, n-done, static_cast<off_t>(at+done));
            if (r < 0) { if (errno == EINTR) continue; throw_errno(errno,"read OCI tar"); }
            if (r == 0) bad("archive changed during read");
            done += static_cast<size_t>(r);
        }
    }
    std::string extension(uint64_t at, uint64_t n) {
        if (n > kExtensionLimit) bad("extension exceeds memory bound");
        std::string out(static_cast<size_t>(n), '\0');
        read(out.data(), out.size(), at);
        return out;
    }
    uint64_t size = 0;
private:
    int fd_ = -1;
};

void parse_pax(const std::string& text, Fields& output) {
    size_t pos = 0, records = 0;
    while (pos < text.size()) {
        if (++records > kEntryLimit) bad("too many PAX records");
        const size_t space = text.find(' ', pos);
        if (space == std::string::npos) bad("PAX record missing length");
        const uint64_t n = decimal(std::string_view(text).substr(pos, space-pos));
        if (n > text.size()-pos || n <= space-pos+2) bad("PAX record length out of bounds");
        const size_t end = pos + static_cast<size_t>(n);
        if (text[end-1] != '\n') bad("PAX record missing newline");
        const size_t equals = text.find('=', space+1);
        if (equals == std::string::npos || equals >= end-1 || equals == space+1)
            bad("invalid PAX key/value");
        const auto key = text.substr(space+1, equals-space-1);
        if (key.find('\0') != std::string::npos) bad("NUL in PAX key");
        // 0.0 requires ordered duplicate offset/numbytes records; never
        // collapse these into an ordinary file or a map of last values.
        if (key == "GNU.sparse.offset" || key == "GNU.sparse.numbytes")
            bad("GNU sparse 0.0 is unsupported");
        output[key] = text.substr(equals+1, end-equals-2);
        if (output.size() > kEntryLimit) bad("too many retained PAX records");
        pos = end;
    }
}
uint64_t field_size(const Fields& fields) {
    uint64_t n = 0;
    for (const auto& [k,v] : fields) n += k.size() + v.size();
    return n;
}
void check_header(const std::array<uint8_t,512>& h) {
    uint64_t sum = 0;
    for (size_t i=0; i<h.size(); ++i) sum += i>=148 && i<156 ? uint8_t(' ') : h[i];
    if (sum != number(h.data()+148,8)) bad("header checksum mismatch");
    if (std::memcmp(h.data()+257,"ustar",5) != 0 || (h[262] != 0 && h[262] != ' '))
        bad("missing USTAR/GNU magic");
}
uint64_t padded(uint64_t n) {
    if (n > UINT64_MAX-511) bad("payload size overflow");
    return (n+511)/512*512;
}
uint32_t narrow(uint64_t n) {
    if (n > UINT32_MAX) bad("metadata integer exceeds 32 bits");
    return static_cast<uint32_t>(n);
}
}  // namespace

LayerPlan parse_oci_layer_plan(const std::string& tar_path) {
    Reader reader(tar_path);
    LayerPlan plan;
    Fields global, local;
    std::string long_name, long_link;
    uint64_t cursor = 0, retained = 0;
    size_t spans = 0, headers = 0, xattr_count = 0;
    bool ended = false;
    while (cursor < reader.size) {
        if (++headers > kEntryLimit * 4) bad("too many archive headers");
        std::array<uint8_t,512> h {};
        reader.read(h.data(), h.size(), cursor);
        cursor += h.size();
        if (std::all_of(h.begin(),h.end(),[](auto c){return c==0;})) {
            reader.read(h.data(),h.size(),cursor);
            cursor += h.size();
            if (!std::all_of(h.begin(),h.end(),[](auto c){return c==0;})) bad("missing second end block");
            ended = true;
            break;
        }
        check_header(h);
        const char type = h[156] ? static_cast<char>(h[156]) : '0';
        const uint64_t header_size = number(h.data()+124,12);
        if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
            const auto text = reader.extension(cursor,header_size);
            const uint64_t skip = padded(header_size);
            if (skip > reader.size-cursor) bad("extension padding truncated");
            cursor += skip;
            if (type == 'x') parse_pax(text,local);
            else if (type == 'g') {
                Fields update;
                parse_pax(text,update);
                for (auto& [k,v] : update) { if (v.empty()) global.erase(k); else global[k]=std::move(v); }
                if (global.size()>kEntryLimit) bad("too many global PAX records");
            } else {
                auto value = text;
                const auto nul = value.find('\0');
                if (nul != std::string::npos) {
                    if (!std::all_of(value.begin()+nul,value.end(),[](auto c){return c==0;})) bad("data after GNU long name NUL");
                    value.resize(nul);
                }
                (type=='L' ? long_name : long_link) = std::move(value);
            }
            if (field_size(global)+field_size(local)+long_name.size()+long_link.size() > kMetadataLimit)
                bad("pending extension metadata exceeds bound");
            continue;
        }
        Fields attrs = global;
        for (const auto& [k,v] : local) {
            if (v.empty() && k.rfind("SCHILY.xattr.",0)!=0) attrs.erase(k);
            else attrs[k]=v;
        }
        local.clear();
        LayerEntry entry;
        switch(type) {
        case '0': entry.kind=EntryKind::Regular; break;
        case '5': entry.kind=EntryKind::Directory; break;
        case '2': entry.kind=EntryKind::Symlink; break;
        case '1': entry.kind=EntryKind::Hardlink; break;
        case '3': entry.kind=EntryKind::Character; break;
        case '4': entry.kind=EntryKind::Block; break;
        case '6': entry.kind=EntryKind::Fifo; break;
        default: bad("unsupported entry encoding " + std::string(1,type));
        }
        std::string name = long_name.empty() ? field(h.data(),100) : long_name;
        // GNU headers reuse the prefix area for non-path fields.
        if (long_name.empty() && h[262] == 0) {
            const auto prefix=field(h.data()+345,155);
            if (!prefix.empty()) name=prefix+"/"+name;
        }
        if (attrs.count("path")) name=attrs.at("path");
        entry.path=path(name,entry.kind==EntryKind::Directory);
        entry.link_target=long_link.empty() ? field(h.data()+157,100) : long_link;
        if (attrs.count("linkpath")) entry.link_target=attrs.at("linkpath");
        long_name.clear(); long_link.clear();
        if (entry.kind==EntryKind::Hardlink) entry.link_target=path(entry.link_target);
        if (entry.kind==EntryKind::Symlink && (entry.link_target.empty() || entry.link_target.find('\0')!=std::string::npos))
            bad("invalid symlink target");
        entry.mode=narrow(number(h.data()+100,8));
        if (entry.mode > 07777) bad("mode contains non-permission bits");
        entry.uid=narrow(attrs.count("uid") ? decimal(attrs.at("uid")) : number(h.data()+108,8));
        entry.gid=narrow(attrs.count("gid") ? decimal(attrs.at("gid")) : number(h.data()+116,8));
        if(attrs.count("mtime")) pax_time(entry,attrs.at("mtime"));
        else header_time(entry,h.data()+136,12);
        entry.device_major=narrow(attrs.count("SCHILY.devmajor") ?
            decimal(attrs.at("SCHILY.devmajor")) : number(h.data()+329,8));
        entry.device_minor=narrow(attrs.count("SCHILY.devminor") ?
            decimal(attrs.at("SCHILY.devminor")) : number(h.data()+337,8));
        if ((entry.kind==EntryKind::Character || entry.kind==EntryKind::Block) &&
            (entry.device_major>0xfff || entry.device_minor>0xfffff))
            bad("device number exceeds Linux encoding");
        const uint64_t stored_size=attrs.count("size") ? decimal(attrs.at("size")) : header_size;
        const uint64_t skip=padded(stored_size);
        if (skip > reader.size-cursor) bad("payload extends beyond archive");
        entry.logical_size=stored_size;
        for (const auto& [k,v] : attrs) {
            if (k.rfind("SCHILY.xattr.",0)==0) {
                if (k.size()==13) bad("empty xattr name");
                entry.xattrs[k.substr(13)]=v;
            }
        }
        bool sparse=false;
        for (const auto& [k,v] : attrs) {
            (void)v;
            if (k.rfind("GNU.sparse.",0)==0) sparse=true;
        }
        if (sparse) {
            if (entry.kind!=EntryKind::Regular) bad("sparse metadata on non-regular entry");
            std::vector<uint64_t> numbers;
            uint64_t data_start=cursor;
            uint64_t data_size=stored_size;
            if (attrs.count("GNU.sparse.map")) {
                if ((attrs.count("GNU.sparse.major") && attrs.at("GNU.sparse.major")!="0") ||
                    (attrs.count("GNU.sparse.minor") && attrs.at("GNU.sparse.minor")!="1"))
                    bad("unsupported GNU sparse version");
                const auto& map=attrs.at("GNU.sparse.map");
                size_t pos=0;
                while (pos<map.size()) {
                    const size_t comma=map.find(',',pos);
                    numbers.push_back(decimal(std::string_view(map).substr(pos,comma==std::string::npos ? map.size()-pos : comma-pos)));
                    if (numbers.size()>kSpanLimit*2) bad("too many sparse spans");
                    if (comma==std::string::npos) break;
                    pos=comma+1;
                    if (pos==map.size()) bad("trailing sparse map comma");
                }
                if (!attrs.count("GNU.sparse.size")) bad("missing GNU sparse logical size");
                entry.logical_size=decimal(attrs.at("GNU.sparse.size"));
                if (attrs.count("GNU.sparse.numblocks") && decimal(attrs.at("GNU.sparse.numblocks")) != numbers.size()/2)
                    bad("sparse extent count mismatch");
            } else if (attrs.count("GNU.sparse.major") && attrs.at("GNU.sparse.major")=="1" &&
                       attrs.count("GNU.sparse.minor") && attrs.at("GNU.sparse.minor")=="0") {
                if (!attrs.count("GNU.sparse.realsize")) bad("missing GNU sparse real size");
                entry.logical_size=decimal(attrs.at("GNU.sparse.realsize"));
                // Read map lines through a bounded window; map padding is
                // excluded from the original payload offsets below.
                const auto text=reader.extension(cursor,std::min<uint64_t>(stored_size,kExtensionLimit));
                size_t pos=0;
                auto line=[&]() {
                    const size_t nl=text.find('\n',pos);
                    if (nl==std::string::npos) bad("sparse 1.0 map exceeds bound or is truncated");
                    const uint64_t value=decimal(std::string_view(text).substr(pos,nl-pos));
                    pos=nl+1;
                    return value;
                };
                const uint64_t count=line();
                if (count>kSpanLimit) bad("too many sparse spans");
                for (uint64_t i=0;i<count*2;++i) numbers.push_back(line());
                const uint64_t map_bytes=padded(pos);
                if (map_bytes>stored_size) bad("sparse map exceeds stored payload");
                data_start+=map_bytes;
                data_size-=map_bytes;
            } else bad("unsupported GNU sparse encoding");
            if (attrs.count("GNU.sparse.name")) entry.path=path(attrs.at("GNU.sparse.name"));
            if (numbers.size()%2) bad("odd sparse map field count");
            uint64_t logical_end=0, consumed=0;
            for (size_t i=0;i<numbers.size();i+=2) {
                const uint64_t off=numbers[i], len=numbers[i+1];
                if (off<logical_end || off>entry.logical_size || len>entry.logical_size-off || len>data_size-consumed)
                    bad("invalid sparse extent bounds");
                if (len) entry.payload_spans.push_back({off,data_start+consumed,len});
                consumed+=len;
                logical_end=off+len;
            }
            if (consumed!=data_size) bad("sparse stored size disagrees with extent map");
        } else if (entry.kind==EntryKind::Regular) {
            if (stored_size) entry.payload_spans.push_back({0,cursor,stored_size});
        } else {
            if (stored_size!=0) bad("non-regular entry has a payload");
            entry.logical_size=0;
        }
        cursor+=skip;
        spans+=entry.payload_spans.size();
        if (spans>kSpanLimit) bad("too many retained payload spans");
        xattr_count+=entry.xattrs.size();
        if (xattr_count>kEntryLimit) bad("too many retained xattrs");
        retained+=entry.path.size()+entry.link_target.size()+field_size(entry.xattrs);
        if (retained>kMetadataLimit) bad("retained metadata exceeds bound");
        if (plan.entries.size()+plan.whiteout_removals.size()+plan.opaque_directories.size()>=kEntryLimit)
            bad("too many layer entries");
        const size_t slash=entry.path.rfind('/');
        const std::string basename=entry.path.substr(slash==std::string::npos ? 0 : slash+1);
        const std::string parent=slash==std::string::npos ? "" : entry.path.substr(0,slash);
        if (basename.rfind(".wh.",0)==0) {
            if (entry.kind!=EntryKind::Regular || entry.logical_size!=0) bad("invalid whiteout marker");
            if (basename==".wh..wh..opq") plan.opaque_directories.push_back(parent);
            else {
                const auto leaf=basename.substr(4);
                if (leaf.empty() || leaf=="." || leaf=="..") bad("invalid whiteout target");
                plan.whiteout_removals.push_back(path(parent.empty() ? leaf : parent+"/"+leaf));
            }
        } else plan.entries.push_back(std::move(entry));
    }
    if (!ended) bad("missing end-of-archive blocks");
    std::array<uint8_t,4096> tail {};
    while (cursor<reader.size) {
        const size_t n=static_cast<size_t>(std::min<uint64_t>(tail.size(),reader.size-cursor));
        reader.read(tail.data(),n,cursor);
        if (!std::all_of(tail.begin(),tail.begin()+n,[](auto c){return c==0;}))
            bad("nonzero data after end-of-archive");
        cursor+=n;
    }
    if (!local.empty() || !long_name.empty() || !long_link.empty()) bad("dangling extension header");
    return plan;
}
}  // namespace obd::convert
