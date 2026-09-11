// Sparse writable layer. See sparse_rw.hpp.
#include "format/sparse_rw.hpp"

#include "common/errors.hpp"

#include <elio/io/io_awaitables.hpp>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace obd::format {

namespace {

constexpr uint64_t kSector = 512;
constexpr uint64_t kMaxSegLen = bytes::segment_mapping::kMaxLength;
constexpr size_t kZeroMaskHeaderSize = 32;
constexpr char kZeroMaskMagic[8] = {'O', 'B', 'D', 'S', 'P', 'Z', 'M', '1'};

std::string parent_dir_of(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

int fsync_parent_dir(const std::string& path) {
    const std::string dir = parent_dir_of(path);
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return errno;
    int rc = 0;
    if (::fsync(fd) != 0) rc = errno;
    if (::close(fd) != 0 && rc == 0) rc = errno;
    return rc;
}

int pwrite_all_sync(int fd, const void* buf, size_t count, uint64_t offset) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t done = 0;
    while (done < count) {
        const ssize_t r = ::pwrite(fd, p + done, count - done,
                                  static_cast<off_t>(offset + done));
        if (r < 0) {
            if (errno == EINTR) continue;
            return errno;
        }
        if (r == 0) return EIO;
        done += static_cast<size_t>(r);
    }
    return 0;
}

int pread_all_sync(int fd, void* buf, size_t count, uint64_t offset) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < count) {
        const ssize_t r = ::pread(fd, p + done, count - done,
                                 static_cast<off_t>(offset + done));
        if (r < 0) {
            if (errno == EINTR) continue;
            return errno;
        }
        if (r == 0) return EIO;
        done += static_cast<size_t>(r);
    }
    return 0;
}

bool can_coalesce(const bytes::segment_mapping& a,
                  const bytes::segment_mapping& b) {
    if (a.zeroed != b.zeroed || b.offset > a.end()) return false;
    const uint64_t merged_end = std::max(a.end(), b.end());
    const uint64_t merged_len = merged_end - a.offset;
    if (merged_len > kMaxSegLen) return false;
    if (a.zeroed) return true;
    return a.moffset + (b.offset - a.offset) == b.moffset;
}

void coalesce_with_next(std::vector<bytes::segment_mapping>& v,
                        std::vector<bytes::segment_mapping>::iterator it) {
    while (it + 1 != v.end() && can_coalesce(*it, *(it + 1))) {
        auto next = it + 1;
        it->length =
            static_cast<uint32_t>(std::max(it->end(), next->end()) -
                                  it->offset);
        v.erase(next);
    }
}

void insert_sorted(std::vector<bytes::segment_mapping>& v,
                   const bytes::segment_mapping& m) {
    auto it = v.begin();
    while (it != v.end() && it->offset < m.offset) ++it;
    it = v.insert(it, m);
    if (it != v.begin()) {
        auto prev = it - 1;
        if (can_coalesce(*prev, *it)) {
            prev->length =
                static_cast<uint32_t>(std::max(prev->end(), it->end()) -
                                      prev->offset);
            it = v.erase(it);
            it = prev;
        }
    }
    coalesce_with_next(v, it);
}

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
    layer->vsize_.store(vsize, std::memory_order_release);
    layer->zero_mask_path_ = path + ".zeroes";
    layer->ro_ = co_await source::LocalFileSource::open(path);

    if (fresh) {
        if (::unlink(layer->zero_mask_path_.c_str()) == 0) {
            const int drc = fsync_parent_dir(layer->zero_mask_path_);
            if (drc != 0) {
                ::close(fd);
                throw_errno(drc, "cannot sync sparse zero mask directory " +
                                     layer->zero_mask_path_);
            }
        } else if (errno != ENOENT) {
            const int e = errno;
            ::close(fd);
            throw_errno(e, "cannot remove stale sparse zero mask " +
                           layer->zero_mask_path_);
        }
    }

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
            // Extent boundaries are not necessarily sector aligned on every
            // filesystem; round outward to whole sectors (we only ever write
            // whole sectors).
            const uint64_t so = d / kSector;
            const uint64_t se = (h + kSector - 1) / kSector;
            if (se > so) layer->insert_live_extent(so, se - so);
            pos = h;
        }
    }
    layer->load_zero_masks();
    co_return layer;
}

bool SparseRwLayer::has_zero_masks() const {
    return std::any_of(segments_.begin(), segments_.end(),
                       [](const auto& s) { return s.zeroed; });
}

void SparseRwLayer::erase_range(uint64_t lo, uint64_t hi) {
    if (lo >= hi) return;
    std::vector<bytes::segment_mapping> next;
    next.reserve(segments_.size());
    for (const auto& s : segments_) {
        const uint64_t s_end = s.end();
        if (s_end <= lo || s.offset >= hi) {
            next.push_back(s);
            continue;
        }
        if (s.offset < lo) {
            auto head = s;
            head.length = static_cast<uint32_t>(lo - s.offset);
            next.push_back(head);
        }
        if (s_end > hi) {
            auto tail = s;
            tail.offset = hi;
            if (!tail.zeroed) tail.moffset += hi - s.offset;
            tail.length = static_cast<uint32_t>(s_end - hi);
            next.push_back(tail);
        }
    }
    segments_.swap(next);
}

void SparseRwLayer::insert_live_extent(uint64_t off, uint64_t len) {
    uint64_t cur = off;
    const uint64_t hi = off + len;
    while (cur < hi) {
        const uint64_t n = std::min(hi - cur, kMaxSegLen);
        bytes::segment_mapping m;
        m.offset = cur;
        m.length = static_cast<uint32_t>(n);
        m.moffset = cur;
        m.zeroed = false;
        m.tag = 0;
        insert_sorted(segments_, m);
        cur += n;
    }
}

void SparseRwLayer::insert_zero_extent(uint64_t off, uint64_t len) {
    uint64_t cur = off;
    const uint64_t hi = off + len;
    while (cur < hi) {
        const uint64_t n = std::min(hi - cur, kMaxSegLen);
        bytes::segment_mapping m;
        m.offset = cur;
        m.length = static_cast<uint32_t>(n);
        m.moffset = 0;
        m.zeroed = true;
        m.tag = 0;
        insert_sorted(segments_, m);
        cur += n;
    }
}

void SparseRwLayer::insert_zero_gaps(uint64_t off, uint64_t len) {
    uint64_t cur = off;
    const uint64_t hi = off + len;
    std::vector<std::pair<uint64_t, uint64_t>> gaps;
    for (const auto& s : segments_) {
        if (s.end() <= cur) continue;
        if (s.offset >= hi) break;
        if (s.offset > cur) {
            const uint64_t gap_hi = std::min(hi, s.offset);
            gaps.emplace_back(cur, gap_hi);
            cur = gap_hi;
        }
        if (s.end() > cur) cur = std::min(hi, s.end());
        if (cur >= hi) break;
    }
    if (cur < hi) gaps.emplace_back(cur, hi);
    for (const auto& [gap_lo, gap_hi] : gaps) {
        insert_zero_extent(gap_lo, gap_hi - gap_lo);
    }
}

int SparseRwLayer::persist_zero_masks() const {
    std::vector<bytes::segment_mapping> zeroes;
    for (const auto& s : segments_) {
        if (s.zeroed) zeroes.push_back(s);
    }
    if (zeroes.empty()) {
        if (::unlink(zero_mask_path_.c_str()) == 0) {
            const int drc = fsync_parent_dir(zero_mask_path_);
            if (drc != 0) return -drc;
        } else if (errno != ENOENT) {
            return -errno;
        }
        return 0;
    }

    const std::string tmp = zero_mask_path_ + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                          0644);
    if (fd < 0) return -errno;

    std::vector<uint8_t> raw(
        kZeroMaskHeaderSize +
            zeroes.size() * bytes::segment_mapping::kEncodedSize,
        0);
    std::memcpy(raw.data(), kZeroMaskMagic, sizeof(kZeroMaskMagic));
    bytes::store_u64_le(raw.data() + 8, vsize_.load(std::memory_order_acquire));
    bytes::store_u64_le(raw.data() + 16, zeroes.size());
    for (size_t i = 0; i < zeroes.size(); ++i) {
        auto z = zeroes[i];
        z.zeroed = true;
        z.moffset = 0;
        z.tag = 0;
        bytes::store_segment_le(
            raw.data() + kZeroMaskHeaderSize +
                i * bytes::segment_mapping::kEncodedSize,
            z);
    }

    int rc = pwrite_all_sync(fd, raw.data(), raw.size(), 0);
    if (rc == 0 && ::fsync(fd) != 0) rc = errno;
    if (::close(fd) != 0 && rc == 0) rc = errno;
    if (rc != 0) {
        ::unlink(tmp.c_str());
        return -rc;
    }
    if (::rename(tmp.c_str(), zero_mask_path_.c_str()) != 0) {
        const int e = errno;
        ::unlink(tmp.c_str());
        return -e;
    }
    const int drc = fsync_parent_dir(zero_mask_path_);
    if (drc != 0) return -drc;
    return 0;
}

void SparseRwLayer::load_zero_masks() {
    const int fd = ::open(zero_mask_path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return;
        throw_errno(errno, "cannot open sparse zero mask " + zero_mask_path_);
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int e = errno;
        ::close(fd);
        throw_errno(e, "cannot stat sparse zero mask " + zero_mask_path_);
    }
    if (st.st_size < static_cast<off_t>(kZeroMaskHeaderSize)) {
        ::close(fd);
        throw format_error("sparse zero mask is truncated: " + zero_mask_path_);
    }

    std::array<uint8_t, kZeroMaskHeaderSize> hdr {};
    int rc = pread_all_sync(fd, hdr.data(), hdr.size(), 0);
    if (rc != 0) {
        ::close(fd);
        throw_errno(rc, "cannot read sparse zero mask " + zero_mask_path_);
    }
    if (std::memcmp(hdr.data(), kZeroMaskMagic, sizeof(kZeroMaskMagic)) != 0) {
        ::close(fd);
        throw format_error("sparse zero mask has bad magic: " + zero_mask_path_);
    }

    const uint64_t stored_vsize = bytes::load_u64_le(hdr.data() + 8);
    const uint64_t cur_vsize = vsize_.load(std::memory_order_acquire);
    if (stored_vsize != cur_vsize) {
        ::close(fd);
        throw format_error("sparse zero mask vsize mismatch: " +
                           zero_mask_path_);
    }

    const uint64_t count = bytes::load_u64_le(hdr.data() + 16);
    const uint64_t max_count =
        (std::numeric_limits<uint64_t>::max() - kZeroMaskHeaderSize) /
        bytes::segment_mapping::kEncodedSize;
    if (count > max_count) {
        ::close(fd);
        throw format_error("sparse zero mask size mismatch: " + zero_mask_path_);
    }
    const uint64_t entry_bytes =
        count * bytes::segment_mapping::kEncodedSize;
    if (static_cast<uint64_t>(st.st_size) != kZeroMaskHeaderSize + entry_bytes) {
        ::close(fd);
        throw format_error("sparse zero mask size mismatch: " + zero_mask_path_);
    }

    std::vector<uint8_t> raw(entry_bytes);
    if (entry_bytes != 0) {
        rc = pread_all_sync(fd, raw.data(), raw.size(), kZeroMaskHeaderSize);
        if (rc != 0) {
            ::close(fd);
            throw_errno(rc, "cannot read sparse zero mask entries " +
                                zero_mask_path_);
        }
    }
    ::close(fd);

    const uint64_t max_sector = cur_vsize / kSector;
    uint64_t prev_end = 0;
    for (uint64_t i = 0; i < count; ++i) {
        auto z = bytes::load_segment_le(
            raw.data() + i * bytes::segment_mapping::kEncodedSize);
        if (!z.zeroed || z.length == 0 || z.offset < prev_end ||
            z.offset > max_sector || z.length > max_sector - z.offset) {
            throw format_error("sparse zero mask has invalid segment: " +
                               zero_mask_path_);
        }
        const uint64_t z_end = z.offset + z.length;
        insert_zero_gaps(z.offset, z.length);
        prev_end = z_end;
    }
}

int SparseRwLayer::grow(uint64_t vsize) {
    // D3 grow-only vsize extension. BLOCKING (ftruncate): callers must
    // run this off an Elio worker via elio::spawn_blocking — the device
    // resize executor does. Equal is an idempotent no-op (retried grows);
    // smaller is a shrink and is rejected.
    if (vsize == 0 || vsize % kSector != 0) return -EINVAL;
    const uint64_t cur = vsize_.load(std::memory_order_acquire);
    if (vsize < cur) return -EINVAL;  // grow-only (equal = no-op)
    if (vsize == cur) return 0;
    if (::ftruncate(fd_, static_cast<off_t>(vsize)) != 0) return -errno;
    vsize_.store(vsize, std::memory_order_release);
    ro_->set_size_for_sparse_writable(vsize);
    if (has_zero_masks()) {
        const int rc = persist_zero_masks();
        if (rc != 0) {
            (void)::ftruncate(fd_, static_cast<off_t>(cur));
            vsize_.store(cur, std::memory_order_release);
            ro_->set_size_for_sparse_writable(cur);
            return rc;
        }
    }
    return 0;
}

elio::coro::task<ssize_t> SparseRwLayer::pwrite(const void* buf, size_t count,
                                                uint64_t offset) {
    if (offset % kSector != 0 || count % kSector != 0 || count == 0) {
        co_return -EINVAL;
    }
    const uint64_t cur_vsize = vsize_.load(std::memory_order_acquire);
    if (offset > cur_vsize || count > cur_vsize - offset) co_return -EINVAL;
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
    const uint64_t lo = offset / kSector;
    const uint64_t hi = lo + count / kSector;
    const bool had_zero_masks = has_zero_masks();
    erase_range(lo, hi);
    insert_live_extent(lo, hi - lo);
    if (had_zero_masks || has_zero_masks()) {
        const int rc = persist_zero_masks();
        if (rc != 0) co_return rc;
    }
    co_return static_cast<ssize_t>(count);
}

elio::coro::task<ssize_t> SparseRwLayer::pread(void* buf, size_t count,
                                               uint64_t offset) {
    if (offset % kSector != 0 || count % kSector != 0) co_return -EINVAL;
    const uint64_t cur_vsize = vsize_.load(std::memory_order_acquire);
    if (offset >= cur_vsize) co_return 0;
    if (count > cur_vsize - offset) {
        count = static_cast<size_t>(cur_vsize - offset);
    }
    uint8_t* out = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < count) {
        const uint64_t pos = offset + done;
        // Find coverage of pos in the (sorted, merged) extents.
        const bytes::segment_mapping* hit = nullptr;
        uint64_t covered_end = pos;
        for (const auto& s : segments_) {
            if (s.offset * kSector > pos) break;
            if (s.end() * kSector > pos) {
                hit = &s;
                covered_end = s.end() * kSector;
                break;
            }
        }
        if (hit != nullptr) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(
                covered_end - pos, count - done));
            if (hit->zeroed) {
                std::memset(out + done, 0, n);
            } else {
                const auto r = co_await elio::io::async_read(
                    fd_, out + done, n, static_cast<off_t>(pos));
                if (r.result < 0) co_return r.result;
                if (r.result != static_cast<ssize_t>(n)) co_return -EIO;
            }
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
    const uint64_t cur_vsize = vsize_.load(std::memory_order_acquire);
    if (offset > cur_vsize || len > cur_vsize - offset) co_return -EINVAL;
    // Real deallocation: the blocks return to the filesystem and the range
    // reads back as zeroes from this layer. The sidecar zero mask below is
    // what keeps a MergedWritable from falling through to lower layers.
    if (::fallocate(fd_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                    static_cast<off_t>(offset), static_cast<off_t>(len)) != 0) {
        co_return -errno;
    }
    const uint64_t lo = offset / kSector;
    const uint64_t hi = lo + len / kSector;
    erase_range(lo, hi);
    insert_zero_extent(lo, hi - lo);
    const int rc = persist_zero_masks();
    if (rc != 0) co_return rc;
    co_return 0;
}

elio::coro::task<int> SparseRwLayer::flush() {
    if (::fdatasync(fd_) != 0) co_return -errno;
    co_return 0;
}

elio::coro::task<int> SparseRwLayer::checkpoint() {
    co_return 0;  // extents and zero masks are already durable
}

}  // namespace obd::format
