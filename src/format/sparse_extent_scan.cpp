#include "format/sparse_extent_scan.hpp"

#include "common/errors.hpp"

#include <linux/fiemap.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <fcntl.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <memory>

namespace obd::format {
namespace {
constexpr uint64_t kSector = 512;
constexpr uint32_t kBatch = 128;

void append(std::vector<bytes::segment_mapping>& out, uint64_t lo,
            uint64_t hi, bool zero) {
    if (lo % kSector || hi % kSector || hi < lo)
        throw error(EIO, "Sparse extent is not sector aligned");
    lo /= kSector;
    hi /= kSector;
    while (lo < hi) {
        if (!out.empty() && out.back().end() == lo &&
            out.back().zeroed == zero &&
            out.back().length < bytes::segment_mapping::kMaxLength) {
            auto& last = out.back();
            const auto n = std::min<uint64_t>(hi - lo,
                bytes::segment_mapping::kMaxLength - last.length);
            last.length += static_cast<uint32_t>(n);
            lo += n;
        } else {
            const auto n = std::min<uint64_t>(hi - lo,
                                            bytes::segment_mapping::kMaxLength);
            out.push_back({lo, static_cast<uint32_t>(n), zero ? 0 : lo, zero, 0});
            lo += n;
        }
    }
}

}  // namespace

void validate_sparse_extent_backing(int fd) {
    struct statfs fs {};
    if (::fstatfs(fd, &fs) != 0)
        throw error(errno, "Sparse backing geometry query failed");
    if (fs.f_bsize != 4096 || ::sysconf(_SC_PAGESIZE) != 4096)
        throw error(EOPNOTSUPP, "extent Sparse requires 4 KiB filesystem blocks and host pages");
    struct fsxattr attr {};
    if (::ioctl(fd, FS_IOC_FSGETXATTR, &attr) != 0) {
        if (errno != EOPNOTSUPP && errno != ENOTTY)
            throw error(errno, "Sparse allocation policy query failed");
        return;
    }
    // Inherited XFS extent hints can turn untouched holes into UNWRITTEN
    // neighbors on an ordinary write, which would acquire ZERO ownership.
    // Realtime allocation also has a separately configured allocation unit.
    if (attr.fsx_extsize > 4096 || (attr.fsx_xflags & FS_XFLAG_REALTIME))
        throw error(EOPNOTSUPP, "Sparse backing allocation policy exceeds its 4 KiB ownership unit");
}

std::vector<bytes::segment_mapping> scan_sparse_extents(int fd, uint64_t size) {
    validate_sparse_extent_backing(fd);
    if (!size || size % 4096 ||
        size / kSector >= bytes::segment_mapping::kInvalidOffset ||
        size > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
        throw error(EINVAL, "Sparse extent size must be positive and 4 KiB aligned");
    struct stat st {};
    if (::fstat(fd, &st) != 0) throw error(errno, "Sparse extent fstat failed");
    if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
        static_cast<uint64_t>(st.st_size) != size)
        throw error(EINVAL, "Sparse backing file must cover exactly its virtual size");
    const size_t allocation = sizeof(fiemap) + kBatch * sizeof(fiemap_extent);
    std::unique_ptr<fiemap, decltype(&std::free)> map(
        static_cast<fiemap*>(std::calloc(1, allocation)), &std::free);
    if (!map) throw std::bad_alloc();
    std::vector<bytes::segment_mapping> out;
    uint64_t cursor = 0;
    while (cursor < size) {
        map->fm_start = cursor;
        map->fm_length = size - cursor;
        // Drain buffered data once before interpreting UNWRITTEN. The caller
        // excludes mutations throughout pagination, so later batches need no
        // repeated whole-file writeback. This is a recovery operation, not a
        // per-write publication barrier.
        map->fm_flags = cursor == 0 ? FIEMAP_FLAG_SYNC : 0;
        map->fm_extent_count = kBatch;
        map->fm_mapped_extents = 0;
        if (::ioctl(fd, FS_IOC_FIEMAP, map.get()) != 0)
            throw error(errno, "Sparse FIEMAP failed");
        if (map->fm_mapped_extents > kBatch)
            throw error(EIO, "Sparse FIEMAP returned too many extents");
        if (!map->fm_mapped_extents) break;
        bool last = false;
        for (uint32_t i = 0; i < map->fm_mapped_extents; ++i) {
            const auto& e = map->fm_extents[i];
            if (!e.fe_length || e.fe_logical > UINT64_MAX - e.fe_length)
                throw error(EIO, "Sparse FIEMAP extent overflow");
            const uint64_t end = e.fe_logical + e.fe_length;
            if (end <= cursor || (i && e.fe_logical < cursor))
                throw error(EIO, "Sparse FIEMAP extent overlap or no progress");
            const uint64_t lo = std::max<uint64_t>(cursor, e.fe_logical);
            const uint64_t hi = std::min<uint64_t>(size, end);
            if (lo < hi) {
                if ((e.fe_flags & (FIEMAP_EXTENT_UNWRITTEN | FIEMAP_EXTENT_DELALLOC)) ==
                    (FIEMAP_EXTENT_UNWRITTEN | FIEMAP_EXTENT_DELALLOC))
                    throw error(EIO, "Sparse FIEMAP returned conflicting ownership flags");
                append(out, lo, hi, (e.fe_flags & FIEMAP_EXTENT_UNWRITTEN) != 0);
            }
            cursor = end;
            last = (e.fe_flags & FIEMAP_EXTENT_LAST) != 0;
        }
        if (last) break;
    }
    return out;
}
}  // namespace obd::format
