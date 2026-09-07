// Sparse writable layer. See sparse_rw.hpp.
#include "format/sparse_rw.hpp"

#include "common/errors.hpp"

#include <elio/io/io_awaitables.hpp>

#include <fcntl.h>
#include <linux/fs.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace obd::format {

namespace {

constexpr uint64_t kSector = 512;

}  // namespace

elio::coro::task<std::unique_ptr<SparseRwLayer>> SparseRwLayer::open(
    const std::string& path, uint64_t vsize) {
    if (vsize == 0 || vsize % kSector != 0) {
        throw error(EINVAL, "sparse layer vsize must be sector aligned");
    }
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) throw_errno(errno, "cannot open sparse layer " + path);
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int e = errno;
        ::close(fd);
        throw_errno(e, "cannot stat sparse layer " + path);
    }
    const bool fresh = st.st_size == 0;
    if (::ftruncate(fd, static_cast<off_t>(vsize)) != 0) {
        const int e = errno;
        ::close(fd);
        throw_errno(e, "cannot size sparse layer " + path);
    }

    auto layer = std::unique_ptr<SparseRwLayer>(new SparseRwLayer);
    layer->fd_ = fd;
    layer->vsize_ = vsize;
    layer->ro_ = co_await source::LocalFileSource::open(path);

    if (!fresh) {
        // Recover written extents from the kernel fiemap.
        uint64_t pos = 0;
        while (pos < vsize) {
            const off_t data =
                ::lseek(fd, static_cast<off_t>(pos), SEEK_DATA);
            if (data < 0) {
                if (errno == ENXIO) break;  // no more data extents
                const int e = errno;
                throw_errno(e, "SEEK_DATA failed on " + path);
            }
            const off_t hole = ::lseek(fd, data, SEEK_HOLE);
            if (hole < 0) {
                const int e = errno;
                throw_errno(e, "SEEK_HOLE failed on " + path);
            }
            const uint64_t d = static_cast<uint64_t>(data);
            const uint64_t h = std::min(static_cast<uint64_t>(hole), vsize);
            // Extent boundaries are not necessarily sector aligned on
            // every filesystem; round outward to whole sectors (we only
            // ever write whole sectors).
            const uint64_t so = d / kSector;
            const uint64_t se = (h + kSector - 1) / kSector;
            if (se > so) layer->insert_extent(so, se - so);
            pos = h;
        }
    }
    co_return layer;
}

void SparseRwLayer::insert_extent(uint64_t off, uint64_t len) {
    if (len == 0) return;
    // Identity mapping: any covered range is one segment {off, len, off}.
    // Merge with all overlapping or directly adjacent segments.
    uint64_t lo = off, hi = off + len;
    std::vector<bytes::segment_mapping> out;
    out.reserve(segments_.size() + 1);
    for (const auto& s : segments_) {
        const uint64_t s_end = s.end();
        if (s_end < lo || s.offset > hi) {
            out.push_back(s);  // disjoint (with a gap), keep
        } else {
            lo = std::min(lo, s.offset);
            hi = std::max(hi, s_end);
        }
    }
    bytes::segment_mapping m;
    m.offset = lo;
    m.length = static_cast<uint32_t>(hi - lo);
    m.moffset = lo;  // identity
    m.zeroed = false;
    m.tag = 0;
    // Insert sorted.
    auto it = out.begin();
    while (it != out.end() && it->offset < m.offset) ++it;
    out.insert(it, m);
    segments_.swap(out);
}

elio::coro::task<ssize_t> SparseRwLayer::pwrite(const void* buf, size_t count,
                                                uint64_t offset) {
    if (offset % kSector != 0 || count % kSector != 0 || count == 0) {
        co_return -EINVAL;
    }
    if (offset + count > vsize_) co_return -EINVAL;
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t done = 0;
    while (done < count) {
        const size_t piece = std::min(
            count - done,
            static_cast<size_t>(bytes::segment_mapping::kMaxLength) * kSector);
        // elio io backend: positional write, never blocks the worker.
        const auto r = co_await elio::io::async_write(
            fd_, p + done, piece, static_cast<off_t>(offset + done));
        if (r.result < 0) co_return r.result;
        if (r.result != static_cast<ssize_t>(piece)) co_return -EIO;
        done += piece;
    }
    insert_extent(offset / kSector, count / kSector);
    co_return static_cast<ssize_t>(count);
}

elio::coro::task<ssize_t> SparseRwLayer::pread(void* buf, size_t count,
                                               uint64_t offset) {
    if (offset % kSector != 0 || count % kSector != 0) co_return -EINVAL;
    if (offset >= vsize_) co_return 0;
    if (count > vsize_ - offset) count = static_cast<size_t>(vsize_ - offset);
    uint8_t* out = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < count) {
        const uint64_t pos = offset + done;
        // Find coverage of pos in the (sorted, merged) extents.
        uint64_t covered_end = pos;
        bool covered = false;
        for (const auto& s : segments_) {
            if (s.offset > pos) break;
            if (s.end() * kSector > pos) {
                covered = true;
                covered_end = s.end() * kSector;
                break;
            }
        }
        if (covered) {
            const size_t n =
                static_cast<size_t>(std::min<uint64_t>(covered_end - pos,
                                                       count - done));
            const auto r = co_await elio::io::async_read(
                fd_, out + done, n, static_cast<off_t>(pos));
            if (r.result < 0) co_return r.result;
            if (r.result != static_cast<ssize_t>(n)) co_return -EIO;
            done += n;
        } else {
            // Zero-fill up to the next extent or the request end.
            uint64_t next = offset + count;
            for (const auto& s : segments_) {
                if (s.offset * kSector > pos) {
                    next = std::min(next, s.offset * kSector);
                    break;
                }
            }
            const size_t n = static_cast<size_t>(next - pos);
            std::memset(out + done, 0, n);
            done += n;
        }
    }
    co_return static_cast<ssize_t>(done);
}

elio::coro::task<int> SparseRwLayer::discard(uint64_t offset, uint64_t len) {
    if (offset % kSector != 0 || len % kSector != 0 || len == 0) {
        co_return -EINVAL;
    }
    if (offset + len > vsize_) co_return -EINVAL;
    // Real deallocation: the blocks return to the filesystem and the range
    // reads back as zeroes. Metadata-only syscall (same duration class as
    // the fdatasync in flush()).
    if (::fallocate(fd_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                    static_cast<off_t>(offset),
                    static_cast<off_t>(len)) != 0) {
        co_return -errno;
    }
    // Drop coverage of [lo,hi) from the extent index (split/trim).
    const uint64_t lo = offset / kSector;
    const uint64_t hi = lo + len / kSector;
    std::vector<bytes::segment_mapping> next;
    next.reserve(segments_.size());
    for (const auto& s : segments_) {
        if (s.end() <= lo || s.offset >= hi) {
            next.push_back(s);
            continue;
        }
        if (s.offset < lo) {
            auto head = s;
            head.length = static_cast<uint32_t>(lo - s.offset);
            next.push_back(head);
        }
        if (s.end() > hi) {
            auto tail = s;
            tail.offset = hi;
            tail.moffset += hi - s.offset;
            tail.length = static_cast<uint32_t>(s.end() - hi);
            next.push_back(tail);
        }
    }
    segments_.swap(next);
    co_return 0;
}

elio::coro::task<int> SparseRwLayer::flush() {
    if (::fdatasync(fd_) != 0) co_return -errno;
    co_return 0;
}

}  // namespace obd::format
