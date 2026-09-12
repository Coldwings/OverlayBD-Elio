// Format writers. See writer.hpp.
#include "format/writer.hpp"

#include "common/errors.hpp"
#include "common/crc32c.hpp"
#include "format/block_codec.hpp"
#include "format/lsmt_format.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits>

#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

namespace obd::format {

namespace {

void full_pwrite(int fd, const void* buf, size_t count, uint64_t offset) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (count > 0) {
        const ssize_t w = ::pwrite(fd, p, count, static_cast<off_t>(offset));
        if (w < 0) {
            if (errno == EINTR) continue;
            throw_errno(errno, "pwrite failed");
        }
        p += w;
        count -= static_cast<size_t>(w);
        offset += static_cast<uint64_t>(w);
    }
}

void full_pread(int fd, void* buf, size_t count, uint64_t offset) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (count > 0) {
        const ssize_t r = ::pread(fd, p, count, static_cast<off_t>(offset));
        if (r < 0) {
            if (errno == EINTR) continue;
            throw_errno(errno, "pread failed");
        }
        if (r == 0) throw_errno(EIO, "unexpected EOF while reading input");
        p += r;
        count -= static_cast<size_t>(r);
        offset += static_cast<uint64_t>(r);
    }
}

int create_out(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) throw_errno(errno, "cannot create " + path);
    return fd;
}

}  // namespace

std::string generate_uuid() {
    uint8_t b[16];
    std::random_device rd;
    for (size_t i = 0; i < sizeof(b); i += 4) {
        const uint32_t v = rd();
        std::memcpy(b + i, &v, std::min<size_t>(4, sizeof(b) - i));
    }
    // RFC-4122-ish version/variant nibbles (cosmetic; upstream treats the
    // field as an opaque string).
    b[6] = static_cast<uint8_t>((b[6] & 0x0f) | 0x40);
    b[8] = static_cast<uint8_t>((b[8] & 0x3f) | 0x80);
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(36);
    for (size_t i = 0; i < sizeof(b); i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
        s.push_back(hex[b[i] >> 4]);
        s.push_back(hex[b[i] & 0xf]);
    }
    return s;
}

void write_lsmt_single_layer(int in_fd, uint64_t in_size,
                             const std::string& out_path,
                             const LsmtWriteOptions& opts) {
    if (in_size == 0 || in_size % lsmt::kAlignment != 0) {
        throw error(EINVAL, "lsmt input size must be a non-zero multiple of 512");
    }
    const uint64_t n_sectors = in_size / lsmt::kAlignment;
    constexpr uint64_t kMaxSegSectors = bytes::segment_mapping::kMaxLength;

    // Build the index: full coverage, one segment per <= 16383-sector chunk,
    // data region starts at sector 8 (byte 4096).
    std::vector<bytes::segment_mapping> segs;
    uint64_t off = 0, moff = lsmt::kDataStartSector;
    while (off < n_sectors) {
        const uint64_t len = std::min(kMaxSegSectors, n_sectors - off);
        bytes::segment_mapping s;
        s.offset = off;
        s.length = static_cast<uint32_t>(len);
        s.moffset = moff;
        s.zeroed = false;
        s.tag = 0;
        segs.push_back(s);
        off += len;
        moff += len;
    }

    const uint64_t index_offset = lsmt::kSpace + in_size;
    const uint64_t index_bytes =
        segs.size() * bytes::segment_mapping::kEncodedSize;
    const uint64_t total = index_offset + index_bytes + lsmt::kSpace;

    lsmt::HeaderTrailer ht;
    ht.flags = 0;
    ht.set_flag_bit(lsmt::kFlagShiftType);    // data file
    ht.set_flag_bit(lsmt::kFlagShiftSealed);  // sealed
    ht.index_offset = index_offset;
    ht.index_size = segs.size();
    ht.virtual_size = in_size;
    ht.uuid = opts.uuid.empty() ? generate_uuid() : opts.uuid;
    ht.parent_uuid = opts.parent_uuid;
    ht.version = 1;
    ht.sub_version = 1;
    ht.user_tag = opts.user_tag;

    const int out_fd = create_out(out_path);
    struct FdGuard {
        int fd;
        ~FdGuard() { ::close(fd); }
    } guard{out_fd};

    // Reserve the header region, then copy the data.
    uint8_t zeros[lsmt::kSpace] = {};
    full_pwrite(out_fd, zeros, sizeof(zeros), 0);
    std::vector<uint8_t> buf(1 << 20);
    for (uint64_t done = 0; done < in_size;) {
        const size_t chunk =
            static_cast<size_t>(std::min<uint64_t>(buf.size(), in_size - done));
        full_pread(in_fd, buf.data(), chunk, done);
        full_pwrite(out_fd, buf.data(), chunk, lsmt::kSpace + done);
        done += chunk;
    }

    // Index region.
    std::vector<uint8_t> index(index_bytes);
    for (size_t i = 0; i < segs.size(); i++) {
        bytes::store_segment_le(
            index.data() + i * bytes::segment_mapping::kEncodedSize, segs[i]);
    }
    full_pwrite(out_fd, index.data(), index_bytes, index_offset);

    // Header at 0, trailer at total-4096.
    uint8_t region[lsmt::kSpace];
    ht.set_flag_bit(lsmt::kFlagShiftHeader);
    ht.serialize(region);
    full_pwrite(out_fd, region, sizeof(region), 0);

    ht.clr_flag_bit(lsmt::kFlagShiftHeader);  // trailer
    ht.serialize(region);
    full_pwrite(out_fd, region, sizeof(region), total - lsmt::kSpace);

    if (::ftruncate(out_fd, static_cast<off_t>(total)) != 0) {
        throw_errno(errno, "ftruncate failed");
    }
}

void write_lsmt_warp_layer(int metadata_fd, uint64_t virtual_size,
                          const std::vector<bytes::segment_mapping>& mappings,
                          const std::string& out_path,
                          const LsmtWriteOptions& opts) {
    using Mapping = bytes::segment_mapping;
    constexpr uint64_t sector = lsmt::kAlignment;
    constexpr uint64_t max_file = std::numeric_limits<off_t>::max();
    if (virtual_size == 0 || virtual_size % sector != 0 ||
        virtual_size / sector > Mapping::kMaxOffset ||
        mappings.size() > lsmt::kMaxRoIndexSize)
        throw format_error("warp writer: invalid virtual size or index count");
    struct stat input_stat {};
    if (::fstat(metadata_fd, &input_stat) != 0)
        throw_errno(errno, "warp writer: stat metadata input");
    const int mode = ::fcntl(metadata_fd, F_GETFL);
    if (mode < 0) throw_errno(errno, "warp writer: inspect metadata input");
    if ((mode & O_ACCMODE) == O_WRONLY || input_stat.st_size < 0)
        throw format_error("warp writer: unreadable metadata input");
    const uint64_t input_sectors = static_cast<uint64_t>(input_stat.st_size) / sector;
    auto segs = mappings;
    uint64_t end = 0;
    uint64_t data_end = lsmt::kSpace;
    bool local = false, remote = false;
    for (auto& m : segs) {
        if (m.tag > 1 || m.length == 0 || m.length > Mapping::kMaxLength ||
            m.offset >= Mapping::kInvalidOffset || m.offset < end ||
            m.offset > virtual_size / sector ||
            m.length > virtual_size / sector - m.offset ||
            m.moffset > Mapping::kMaxMoffset ||
            (!m.zeroed && m.length > Mapping::kMaxMoffset - m.moffset))
            throw format_error("warp writer: invalid mapping");
        end = m.end();
        local |= m.tag == 0;
        remote |= m.tag == 1;
        if (m.tag == 0) {
            if (!m.zeroed && (m.moffset > input_sectors ||
                             m.length > input_sectors - m.moffset))
                throw format_error("warp writer: metadata extent out of range");
            m.moffset = data_end / sector;
            if (!m.zeroed) {
                const uint64_t n = uint64_t(m.length) * sector;
                if (n > max_file - data_end)
                    throw format_error("warp writer: output size overflow");
                data_end += n;
            }
        }
    }
    if (remote && !local)
        throw format_error("warp writer: remote mappings require a metadata tag");
    const uint64_t index_bytes = segs.size() * Mapping::kEncodedSize;
    if (index_bytes + lsmt::kSpace > max_file - data_end)
        throw format_error("warp writer: output index size overflow");
    const uint64_t total = data_end + index_bytes + lsmt::kSpace;
    lsmt::HeaderTrailer ht;
    ht.flags = (1u << lsmt::kFlagShiftType) | (1u << lsmt::kFlagShiftSealed);
    ht.index_offset = data_end;
    ht.index_size = segs.size();
    ht.virtual_size = virtual_size;
    ht.uuid = opts.uuid.empty() ? generate_uuid() : opts.uuid;
    ht.parent_uuid = opts.parent_uuid;
    ht.user_tag = opts.user_tag;
    uint8_t header[lsmt::kSpace], trailer[lsmt::kSpace];
    ht.serialize(trailer);
    ht.set_flag_bit(lsmt::kFlagShiftHeader);
    ht.serialize(header);  // validate option lengths before touching output
    std::vector<uint8_t> index(index_bytes);
    for (size_t i = 0; i < segs.size(); ++i)
        bytes::store_segment_le(index.data() + i * Mapping::kEncodedSize, segs[i]);
    std::vector<uint8_t> buffer(1 << 20);

    // Open without O_TRUNC so accidental aliases cannot destroy the input.
    const int fd = ::open(out_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) throw_errno(errno, "warp writer: open output");
    struct Guard { int fd; ~Guard() { ::close(fd); } } guard{fd};
    struct stat output_stat {};
    if (::fstat(fd, &output_stat) != 0)
        throw_errno(errno, "warp writer: stat output");
    if (input_stat.st_dev == output_stat.st_dev && input_stat.st_ino == output_stat.st_ino)
        throw format_error("warp writer: output aliases metadata input");
    if (::ftruncate(fd, 0) != 0) throw_errno(errno, "warp writer: truncate output");
    full_pwrite(fd, header, sizeof(header), 0);
    for (size_t i = 0; i < segs.size(); ++i) {
        const auto& m = segs[i];
        if (m.tag != 0 || m.zeroed) continue;
        const uint64_t n = uint64_t(m.length) * sector;
        for (uint64_t done = 0; done < n;) {
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(buffer.size(), n - done));
            full_pread(metadata_fd, buffer.data(), chunk, mappings[i].moffset * sector + done);
            full_pwrite(fd, buffer.data(), chunk, m.moffset * sector + done);
            done += chunk;
        }
    }
    full_pwrite(fd, index.data(), index.size(), data_end);
    full_pwrite(fd, trailer, sizeof(trailer), total - lsmt::kSpace);
    if (::ftruncate(fd, static_cast<off_t>(total)) != 0)
        throw_errno(errno, "warp writer: finalize output");
}

void write_zfile(int in_fd, uint64_t in_size, const std::string& out_path,
                 const ZFileWriteOptions& opts) {
    const uint32_t bs = opts.block_size;
    if (bs == 0 || bs > 65536 || (bs & (bs - 1)) != 0) {
        throw error(EINVAL, "zfile block_size must be a power of two <= 65536");
    }
    auto codec = create_block_codec(opts.algo, opts.level);
    if (!codec) {
        throw error(EINVAL, "unsupported zfile compression algo " +
                                std::to_string(opts.algo));
    }

    const int out_fd = create_out(out_path);
    struct FdGuard {
        int fd;
        ~FdGuard() { ::close(fd); }
    } guard{out_fd};

    // Reserve the header region.
    uint8_t zeros[zfile::kSpace] = {};
    full_pwrite(out_fd, zeros, sizeof(zeros), 0);

    std::vector<uint8_t> in_buf(bs);
    std::vector<uint8_t> comp_buf(codec->compress_bound(bs) + 8);
    std::vector<uint32_t> index;
    uint64_t out_pos = zfile::kSpace;
    for (uint64_t done = 0; done < in_size;) {
        const size_t chunk =
            static_cast<size_t>(std::min<uint64_t>(bs, in_size - done));
        full_pread(in_fd, in_buf.data(), chunk, done);
        const int clen =
            codec->compress(in_buf.data(), chunk, comp_buf.data(),
                            comp_buf.size());
        if (clen <= 0) {
            throw error(EIO, "block compression failed at offset " +
                                 std::to_string(done));
        }
        size_t entry = static_cast<size_t>(clen);
        full_pwrite(out_fd, comp_buf.data(), entry, out_pos);
        out_pos += entry;
        if (opts.verify) {
            const uint32_t crc =
                crc32::crc32c_salt(comp_buf.data(), entry);
            uint8_t crcbuf[4];
            bytes::store_u32_le(crcbuf, crc);
            full_pwrite(out_fd, crcbuf, sizeof(crcbuf), out_pos);
            out_pos += sizeof(crcbuf);
            entry += sizeof(crcbuf);
        }
        index.push_back(static_cast<uint32_t>(entry));
        done += chunk;
    }

    const uint64_t index_offset = out_pos;
    const uint64_t index_bytes = index.size() * sizeof(uint32_t);
    std::vector<uint8_t> raw_index(index_bytes);
    for (size_t i = 0; i < index.size(); i++) {
        bytes::store_u32_le(raw_index.data() + i * 4, index[i]);
    }
    if (index_bytes > 0) {
        full_pwrite(out_fd, raw_index.data(), index_bytes, index_offset);
    }

    zfile::HeaderTrailer ht;
    ht.flags = 0;
    ht.set_flag_bit(zfile::kFlagShiftType);    // data file
    ht.set_flag_bit(zfile::kFlagShiftSealed);  // sealed
    if (opts.calc_digest) ht.set_flag_bit(zfile::kFlagShiftCalcDigest);
    ht.index_offset = index_offset;
    ht.index_size = index.size();
    ht.original_file_size = in_size;
    ht.index_crc = crc32::crc32c(raw_index.data(), index_bytes);
    ht.opt.block_size = bs;
    ht.opt.algo = opts.algo;
    ht.opt.level = opts.level;
    ht.opt.use_dict = 0;
    ht.opt.dict_size = 0;
    ht.opt.verify = opts.verify ? 1 : 0;

    uint8_t region[zfile::kSpace];
    ht.set_flag_bit(zfile::kFlagShiftHeader);
    ht.serialize(region);
    if (opts.calc_digest) ht.set_region_digest(region);
    full_pwrite(out_fd, region, sizeof(region), 0);

    ht.clr_flag_bit(zfile::kFlagShiftHeader);  // trailer
    ht.digest = 0;
    ht.serialize(region);
    if (opts.calc_digest) ht.set_region_digest(region);
    const uint64_t trailer_offset = index_offset + index_bytes;
    full_pwrite(out_fd, region, sizeof(region), trailer_offset);

    if (::ftruncate(out_fd,
                    static_cast<off_t>(trailer_offset + zfile::kSpace)) != 0) {
        throw_errno(errno, "ftruncate failed");
    }
}

}  // namespace obd::format
