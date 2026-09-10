// LSMT writable layer. See lsmt_rw.hpp.
#include "format/lsmt_rw.hpp"

#include "common/errors.hpp"
#include "common/sha256.hpp"
#include "format/writer.hpp"  // generate_uuid

#include <elio/io/io_awaitables.hpp>
#include <elio/log/macros.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace obd::format {

/// BlobSource view over the RW fd with a dynamically tracked size
/// (a LocalFileSource pins st_size at open and would hide appended data).
class LsmtRwLayer::View final : public source::BlobSource {
public:
    View(int fd, const std::atomic<uint64_t>* data_bytes, std::string label)
        : fd_(fd), data_bytes_(data_bytes), label_(std::move(label)) {}

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override {
        const uint64_t bound = data_bytes_->load(std::memory_order_acquire);
        if (offset >= bound) co_return 0;
        if (count > bound - offset) {
            count = static_cast<size_t>(bound - offset);
        }
        if (count == 0) co_return 0;
        const auto r = co_await elio::io::async_read(
            fd_, buf, count, static_cast<off_t>(offset));
        co_return r.result;
    }

    uint64_t size() const noexcept override {
        return data_bytes_->load(std::memory_order_acquire);
    }
    std::string_view label() const noexcept override { return label_; }

private:
    int fd_;
    const std::atomic<uint64_t>* data_bytes_;
    std::string label_;
};

LsmtRwLayer::~LsmtRwLayer() {
    // View borrows fd_ and data_bytes_; callers must drain IO before destruction.
    if (fd_ >= 0) ::close(fd_);
}

source::BlobSource& LsmtRwLayer::data_source() { return *view_; }

namespace {

// Cold-path descriptors must also close when allocation/serialization throws.
// For compaction output, unlink the unpublished file during unwinding too.
struct FdGuard {
    int fd;
    const std::string* unlink_path;

    explicit FdGuard(int value, const std::string* path = nullptr)
        : fd(value), unlink_path(path) {}
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    ~FdGuard() {
        ::close(fd);
        if (unlink_path != nullptr) ::unlink(unlink_path->c_str());
    }
};

constexpr uint64_t kSector = 512;
constexpr uint64_t kHeaderSectors = 8;  // 4096B header region
constexpr uint64_t kMaxSegLen = bytes::segment_mapping::kMaxLength;

elio::coro::task<int> write_all(int fd, const void* buf, size_t count,
                                uint64_t offset) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t done = 0;
    while (done < count) {
        const auto r = co_await elio::io::async_write(
            fd, p + done, count - done, static_cast<off_t>(offset + done));
        if (r.result < 0) co_return r.result;
        if (r.result == 0) co_return -EIO;
        done += static_cast<size_t>(r.result);
    }
    co_return 0;
}

elio::coro::task<int> read_all(int fd, void* buf, size_t count,
                               uint64_t offset) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < count) {
        const auto r = co_await elio::io::async_read(
            fd, p + done, count - done, static_cast<off_t>(offset + done));
        if (r.result < 0) co_return r.result;
        if (r.result == 0) co_return -EIO;
        done += static_cast<size_t>(r.result);
    }
    co_return 0;
}

lsmt::HeaderTrailer make_ht(bool header, bool sealed, uint64_t index_offset,
                            uint64_t index_size, uint64_t vsize,
                            const std::string& uuid,
                            const std::string& user_tag) {
    lsmt::HeaderTrailer ht;
    ht.set_flag_bit(lsmt::kFlagShiftType);  // data file
    if (header) ht.set_flag_bit(lsmt::kFlagShiftHeader);
    if (sealed) ht.set_flag_bit(lsmt::kFlagShiftSealed);
    ht.index_offset = index_offset;
    ht.index_size = index_size;
    ht.virtual_size = vsize;
    ht.uuid = uuid;
    ht.user_tag = user_tag;
    return ht;
}

/// Inserts `m` into a sorted disjoint segment list, coalescing adjacent
/// entries with contiguous moffset (capped at the 14-bit length field).
void insert_sorted(std::vector<bytes::segment_mapping>& v,
                   const bytes::segment_mapping& m) {
    auto it = v.begin();
    while (it != v.end() && it->offset < m.offset) ++it;
    it = v.insert(it, m);
    // Coalesce with the previous entry when contiguous.
    if (it != v.begin()) {
        auto prev = it - 1;
        if (prev->end() == it->offset && prev->mend() == it->moffset &&
            !prev->zeroed && !it->zeroed &&
            prev->length + it->length <= kMaxSegLen) {
            prev->length += it->length;
            it = v.erase(it);
            it = prev;  // merged entry; try to absorb the next one too
        }
    }
    // Coalesce with the next entry when contiguous.
    if (it + 1 != v.end()) {
        auto next = it + 1;
        if (it->end() == next->offset && it->mend() == next->moffset &&
            !it->zeroed && !next->zeroed &&
            it->length + next->length <= kMaxSegLen) {
            it->length += next->length;
            v.erase(next);
        }
    }
}

/// ADR-0014 seal determinism: formats the first 32 hex chars of the
/// content digest as a 36-char 8-4-4-4-12 uuid string. Pure function of
/// the digest — no randomness, no clock.
std::string uuid_from_content_digest(const std::string& digest_hex) {
    std::string u = digest_hex.substr(0, 32);
    u.insert(20, 1, '-');
    u.insert(16, 1, '-');
    u.insert(12, 1, '-');
    u.insert(8, 1, '-');
    return u;
}

}  // namespace

elio::coro::task<std::unique_ptr<LsmtRwLayer>> LsmtRwLayer::create(
    const std::string& path, uint64_t vsize) {
    if (vsize == 0 || vsize % kSector != 0) {
        throw error(EINVAL, "lsmt rw vsize must be sector aligned");
    }
    auto layer = std::unique_ptr<LsmtRwLayer>(new LsmtRwLayer);
    layer->fd_ =
        ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    const int fd = layer->fd_;
    if (fd < 0) throw_errno(errno, "cannot create lsmt rw layer " + path);
    layer->vsize_.store(vsize, std::memory_order_release);
    layer->data_end_sector_ = kHeaderSectors;
    layer->uuid_ = generate_uuid();
    layer->path_ = path;

    const auto ht =
        make_ht(/*header=*/true, /*sealed=*/false, 0, 0, vsize, layer->uuid_,
                "");
    uint8_t region[4096];
    std::memset(region, 0, sizeof(region));
    ht.serialize(region);
    if (const int rc = co_await write_all(fd, region, sizeof(region), 0);
        rc != 0) {
        throw_errno(-rc, "cannot write lsmt rw header " + path);
    }
    layer->data_bytes_.store(4096, std::memory_order_release);
    layer->view_ = std::make_unique<LsmtRwLayer::View>(fd, &layer->data_bytes_,
                                                       "lsmt-rw:" + path);
    co_return layer;
}

int LsmtRwLayer::grow(uint64_t vsize) {
    // D3 grow-only vsize extension. BLOCKING (a 4096B header rewrite plus
    // fsync on fd_): callers must run this off an Elio worker via
    // elio::spawn_blocking — the device resize executor does. Grow-only:
    // an equal request is an idempotent no-op (retried grows after a
    // partial kernel failure land here); smaller is a shrink and is
    // rejected.
    if (sealed_.load(std::memory_order_acquire) ||
        checkpointed_.load(std::memory_order_acquire)) {
        return -EROFS;
    }
    if (vsize == 0 || vsize % kSector != 0) return -EINVAL;
    const uint64_t cur = vsize_.load(std::memory_order_acquire);
    if (vsize < cur) return -EINVAL;  // grow-only (equal = no-op)
    if (vsize == cur) return 0;
    // Rebuild the on-disk header (the 4096B region at offset 0) with the
    // new declared size, preserving the uuid/flags exactly as create()
    // wrote them. This keeps the checkpoint-vs-header cross-check and the
    // offline seal (open_checkpointed/seal_file) consistent after a live
    // grow: the graceful-shutdown checkpoint later writes a trailer at
    // the same grown size, and a plain commit then seals that declared
    // size without any --virtual-size override.
    //
    // Failure atomicity: the checkpoint trailer is written from vsize_,
    // and open_checkpointed rejects a header/trailer pair whose virtual
    // sizes disagree — so a header rewritten to M while vsize_ stays N
    // would make the upper UNCOMMITTABLE. Therefore: back the header up
    // first, and on ANY failure restore it and leave vsize_ untouched.
    uint8_t backup[4096];
    if (::pread(fd_, backup, sizeof(backup), 0) !=
        static_cast<ssize_t>(sizeof(backup))) {
        return -EIO;  // cannot guarantee rollback: do not touch anything
    }
    uint8_t region[4096];
    std::memset(region, 0, sizeof(region));
    const auto ht =
        make_ht(/*header=*/true, /*sealed=*/false, 0, 0, vsize, uuid_, "");
    ht.serialize(region);
    // A SHORT write is a hard error: errno may be stale (or 0) after a
    // partial ::pwrite, so it is never trusted here.
    if (::pwrite(fd_, region, sizeof(region), 0) !=
        static_cast<ssize_t>(sizeof(region))) {
        if (::pwrite(fd_, backup, sizeof(backup), 0) !=
            static_cast<ssize_t>(sizeof(backup))) {
            ELIO_LOG_ERROR(
                "lsmt rw grow: header restore FAILED after a short write; "
                "{} may no longer be committable (header inconsistent)",
                path_);
        } else {
            ::fsync(fd_);
        }
        return -EIO;
    }
    if (::fsync(fd_) != 0) {
        const int e = errno;
        if (::pwrite(fd_, backup, sizeof(backup), 0) !=
            static_cast<ssize_t>(sizeof(backup))) {
            ELIO_LOG_ERROR(
                "lsmt rw grow: header restore FAILED after fsync error; "
                "{} may no longer be committable (header inconsistent)",
                path_);
        } else {
            ::fsync(fd_);
        }
        return -e;
    }
    // Header on disk and in-memory state transition together, only after
    // the new header is durable.
    vsize_.store(vsize, std::memory_order_release);
    return 0;
}

elio::coro::task<ssize_t> LsmtRwLayer::pwrite(const void* buf, size_t count,
                                              uint64_t offset) {
    if (sealed_.load(std::memory_order_acquire) ||
        checkpointed_.load(std::memory_order_acquire)) {
        co_return -EROFS;
    }
    if (offset % kSector != 0 || count % kSector != 0 || count == 0) {
        co_return -EINVAL;
    }
    const uint64_t cur_vsize = vsize_.load(std::memory_order_acquire);
    if (offset + count > cur_vsize) co_return -EINVAL;

    const uint8_t* src = static_cast<const uint8_t*>(buf);
    size_t done = 0;
    while (done < count) {
        const size_t piece = std::min(count - done, kMaxSegLen * kSector);
        const uint64_t lo = (offset + done) / kSector;   // sectors
        const uint64_t hi = lo + piece / kSector;

        // Only live coverage owns reusable physical blocks. Zeroed segments
        // have placeholder offsets, so treat them like gaps and append.
        struct Op {
            uint64_t voff;     // virtual start sector
            uint64_t moff;     // target sector in the file
            uint64_t nsector;
            bool inplace;
        };
        std::vector<Op> ops;
        uint64_t cur = lo;
        for (const auto& s : segments_) {
            if (s.offset >= hi) break;
            if (s.zeroed || s.end() <= lo) continue;
            if (s.offset > cur) {
                ops.push_back({cur, data_end_sector_, s.offset - cur, false});
                data_end_sector_ += s.offset - cur;
            }
            const uint64_t cs = std::max(cur, s.offset);
            const uint64_t ce = std::min(s.end(), hi);
            if (ce > cs) {
                ops.push_back({cs, s.moffset + (cs - s.offset), ce - cs,
                               true});
                cur = ce;
            }
        }
        if (cur < hi) {
            ops.push_back({cur, data_end_sector_, hi - cur, false});
            data_end_sector_ += hi - cur;
        }

        // Execute the data writes.
        for (const auto& op : ops) {
            const size_t nbytes = static_cast<size_t>(op.nsector) * kSector;
            const size_t src_off =
                static_cast<size_t>(op.voff * kSector - offset);
            if (const int rc = co_await write_all(
                    fd_, src + src_off, nbytes, op.moff * kSector);
                rc != 0) {
                co_return rc;
            }
        }

        // Update the in-memory index: drop coverage of [lo,hi) from old
        // segments (split/trim), then insert the new subrange segments.
        std::vector<bytes::segment_mapping> next;
        next.reserve(segments_.size() + ops.size());
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
                // Zeroed offsets are placeholders, not physical extents.
                if (!tail.zeroed) tail.moffset += hi - s.offset;
                tail.length = static_cast<uint32_t>(s.end() - hi);
                next.push_back(tail);
            }
        }
        segments_.swap(next);
        for (const auto& op : ops) {
            bytes::segment_mapping m;
            m.offset = op.voff;
            m.length = static_cast<uint32_t>(op.nsector);
            m.moffset = op.moff;
            m.zeroed = false;
            m.tag = 0;
            insert_sorted(segments_, m);
        }
        done += piece;
    }
    data_bytes_.store(data_end_sector_ * kSector,
                      std::memory_order_release);
    co_return static_cast<ssize_t>(count);
}

elio::coro::task<ssize_t> LsmtRwLayer::pread(void* buf, size_t count,
                                             uint64_t offset) {
    if (offset % kSector != 0 || count % kSector != 0) co_return -EINVAL;
    const uint64_t cur_vsize = vsize_.load(std::memory_order_acquire);
    if (offset >= cur_vsize) co_return 0;
    if (count > cur_vsize - offset) count = static_cast<size_t>(cur_vsize - offset);
    uint8_t* out = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < count) {
        const uint64_t pos = offset + done;  // bytes
        const uint64_t sector = pos / kSector;
        const bytes::segment_mapping* hit = nullptr;
        uint64_t next_start = offset + count;
        for (const auto& s : segments_) {
            if (s.offset > sector) {
                next_start = std::min(next_start, s.offset * kSector);
                break;
            }
            if (s.end() > sector) hit = &s;
        }
        if (hit && !hit->zeroed) {
            const uint64_t mbyte =
                (hit->moffset + (sector - hit->offset)) * kSector;
            const size_t n = static_cast<size_t>(
                std::min<uint64_t>(hit->end() * kSector - pos, count - done));
            if (const int rc = co_await read_all(fd_, out + done, n, mbyte);
                rc != 0) {
                co_return rc;
            }
            done += n;
        } else {
            const uint64_t zero_end =
                hit ? hit->end() * kSector : next_start;
            const size_t n = static_cast<size_t>(
                std::min<uint64_t>(zero_end - pos, count - done));
            std::memset(out + done, 0, n);
            done += n;
        }
    }
    co_return static_cast<ssize_t>(done);
}

elio::coro::task<int> LsmtRwLayer::discard(uint64_t offset, uint64_t len) {
    if (sealed_.load(std::memory_order_acquire) ||
        checkpointed_.load(std::memory_order_acquire)) {
        co_return -EROFS;
    }
    if (offset % kSector != 0 || len % kSector != 0 || len == 0) {
        co_return -EINVAL;
    }
    const uint64_t cur_vsize = vsize_.load(std::memory_order_acquire);
    if (offset + len > cur_vsize) co_return -EINVAL;
    const uint64_t lo = offset / kSector;
    const uint64_t hi = lo + len / kSector;

    // Same split/trim as pwrite, but the covering segments are zeroed:
    // no data is written; superseded data blocks become garbage that
    // seal() drops (ADR-0009).
    std::vector<bytes::segment_mapping> next;
    next.reserve(segments_.size() + 1);
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
            // Zeroed offsets are placeholders, not physical extents.
            if (!tail.zeroed) tail.moffset += hi - s.offset;
            tail.length = static_cast<uint32_t>(s.end() - hi);
            next.push_back(tail);
        }
    }
    segments_.swap(next);
    // Insert zeroed segments (capped at the 14-bit length field). moffset
    // is never read for zeroed segments but must stay inside the data
    // region for LsmtLayer validation after seal — kHeaderSectors always
    // qualifies.
    uint64_t cur = lo;
    while (cur < hi) {
        const uint64_t n = std::min(hi - cur, kMaxSegLen);
        bytes::segment_mapping m;
        m.offset = cur;
        m.length = static_cast<uint32_t>(n);
        m.moffset = kHeaderSectors;
        m.zeroed = true;
        m.tag = 0;
        insert_sorted(segments_, m);
        cur += n;
    }
    co_return 0;
}

elio::coro::task<int> LsmtRwLayer::flush() {
    if (::fdatasync(fd_) != 0) co_return -errno;
    co_return 0;
}

elio::coro::task<int> LsmtRwLayer::checkpoint() {
    if (sealed_.load(std::memory_order_acquire) ||
        checkpointed_.load(std::memory_order_acquire)) {
        co_return -EROFS;
    }

    // Appended at the data end: index (SegmentMapping array, padded to
    // 4096B) | unsealed trailer (4096B). The trailer sits in the file's
    // last 4096 bytes, which is how open_checkpointed locates it.
    const uint64_t index_sector =
        (data_end_sector_ + kHeaderSectors - 1) / kHeaderSectors *
        kHeaderSectors;
    const uint64_t index_offset = index_sector * kSector;  // bytes
    const uint64_t index_size = segments_.size();
    const size_t index_bytes =
        segments_.size() * bytes::segment_mapping::kEncodedSize;
    const size_t index_region = (index_bytes + 4095) / 4096 * 4096;

    std::vector<uint8_t> idx(index_region, 0);
    for (size_t i = 0; i < segments_.size(); ++i) {
        bytes::store_segment_le(
            idx.data() + i * bytes::segment_mapping::kEncodedSize,
            segments_[i]);
    }
    int rc = co_await write_all(fd_, idx.data(), idx.size(), index_offset);
    if (rc != 0) co_return rc;

    uint8_t region[4096];
    std::memset(region, 0, sizeof(region));
    const auto trailer = make_ht(/*header=*/false, /*sealed=*/false,
                                 index_offset, index_size,
                                 vsize_.load(std::memory_order_acquire),
                                 uuid_, "");
    trailer.serialize(region);
    rc = co_await write_all(fd_, region, sizeof(region),
                            index_offset + index_region);
    if (rc != 0) co_return rc;
    if (::fdatasync(fd_) != 0) co_return -errno;
    data_bytes_.store(index_offset + index_region + sizeof(region),
                      std::memory_order_release);
    checkpointed_.store(true, std::memory_order_release);
    co_return 0;
}

elio::coro::task<int> LsmtRwLayer::seal(const std::string& user_tag) {
    if (sealed_.load(std::memory_order_acquire)) co_return -EROFS;

    // Compaction: live segments are copied out packed sequentially; the
    // garbage left behind by in-place edits is dropped. The tmp name is
    // per-process so two seals can never interleave writes into the same
    // file (defense in depth; the supervisor additionally serializes
    // commits per device).
    const std::string tmp = path_ + ".sealing." + std::to_string(::getpid());
    const int out_fd =
        ::open(tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out_fd < 0) co_return -errno;
    FdGuard output(out_fd, &tmp);

    int rc = 0;
    std::vector<bytes::segment_mapping> packed;
    uint64_t out_sector = kHeaderSectors;
    std::vector<uint8_t> copy_buf(1 << 20);
    // ADR-0014 seal determinism: the sealed uuid is derived from the
    // content digest sha256(vsize as LE u64 || packed data bytes in
    // segment order || packed index entries as stored). user_tag is
    // caller input and deliberately excluded from the digest.
    // One consistent load: the same value feeds the content digest and
    // the header/trailer writes below (deterministic seal output).
    const uint64_t seal_vsize = vsize_.load(std::memory_order_acquire);
    common::Sha256 content_hash;
    uint8_t vsize_le[8];
    bytes::store_u64_le(vsize_le, seal_vsize);
    content_hash.update(vsize_le, sizeof(vsize_le));
    for (const auto& s : segments_) {
        bytes::segment_mapping m = s;
        // Zeroed segments carry no data, but their moffset must stay inside
        // the data region for the reader's validation; the current packing
        // position always qualifies (ADR-0009).
        m.moffset = out_sector;
        if (!s.zeroed) {
            uint64_t remaining = s.length * kSector;
            uint64_t from = s.moffset * kSector;
            uint64_t to = out_sector * kSector;
            while (remaining > 0) {
                const size_t n = static_cast<size_t>(
                    std::min<uint64_t>(remaining, copy_buf.size()));
                rc = co_await read_all(fd_, copy_buf.data(), n, from);
                if (rc != 0) goto out;
                content_hash.update(copy_buf.data(), n);
                rc = co_await write_all(out_fd, copy_buf.data(), n, to);
                if (rc != 0) goto out;
                remaining -= n;
                from += n;
                to += n;
            }
            out_sector += s.length;
        }
        insert_sorted(packed, m);
    }

    {
        // Layout: header (sectors 0-7) | data | pad to 8 sectors | index |
        // pad to 4096B | trailer (4096B).
        const uint64_t index_sector =
            (out_sector + kHeaderSectors - 1) / kHeaderSectors *
            kHeaderSectors;
        const uint64_t index_offset = index_sector * kSector;  // bytes
        const uint64_t index_size = packed.size();
        const size_t index_bytes = packed.size() * 16;
        const size_t index_region = (index_bytes + 4095) / 4096 * 4096;

        uint8_t region[4096];

        // Index entries.
        std::vector<uint8_t> idx(index_region, 0);
        for (size_t i = 0; i < packed.size(); ++i) {
            bytes::store_segment_le(idx.data() + i * 16, packed[i]);
        }
        // With data and index fixed, the content digest — and therefore
        // the sealed uuid — is determined (ADR-0014). Derive it before the
        // header is written.
        content_hash.update(idx.data(), index_bytes);
        uuid_ = uuid_from_content_digest(content_hash.final_hex());

        std::memset(region, 0, sizeof(region));
        const auto header = make_ht(true, true, index_offset, index_size,
                                    seal_vsize, uuid_, user_tag);
        header.serialize(region);
        rc = co_await write_all(out_fd, region, sizeof(region), 0);
        if (rc != 0) goto out;

        rc = co_await write_all(out_fd, idx.data(), idx.size(),
                                index_offset);
        if (rc != 0) goto out;

        std::memset(region, 0, sizeof(region));
        const auto trailer = make_ht(false, true, index_offset, index_size,
                                     seal_vsize, uuid_, user_tag);
        trailer.serialize(region);
        rc = co_await write_all(out_fd, region, sizeof(region),
                                index_offset + index_region);
        if (rc != 0) goto out;

        if (::fdatasync(out_fd) != 0) {
            rc = -errno;
            goto out;
        }
    }

    if (::rename(tmp.c_str(), path_.c_str()) != 0) {
        rc = -errno;
        goto out;
    }
    output.unlink_path = nullptr;  // rename published the output
    segments_ = std::move(packed);
    sealed_.store(true, std::memory_order_release);
    {
        struct stat st {};
        if (::fstat(fd_, &st) == 0) {
            data_bytes_.store(static_cast<uint64_t>(st.st_size),
                              std::memory_order_release);
        }
    }

out:
    co_return rc;
}

elio::coro::task<std::unique_ptr<LsmtRwLayer>>
LsmtRwLayer::open_checkpointed(const std::string& path, int* error) {
    auto layer = std::unique_ptr<LsmtRwLayer>(new LsmtRwLayer);
    layer->fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    const int fd = layer->fd_;
    if (fd < 0) {
        *error = -errno;
        co_return nullptr;
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        *error = -errno;
        co_return nullptr;
    }
    if (st.st_size < static_cast<off_t>(2 * lsmt::kSpace)) {
        *error = -EINVAL;
        co_return nullptr;
    }

    // The checkpoint trailer is the file's last 4096 bytes.
    const uint64_t trailer_offset =
        static_cast<uint64_t>(st.st_size) - lsmt::kSpace;
    uint8_t region[lsmt::kSpace];
    int rc = co_await read_all(fd, region, sizeof(region), trailer_offset);
    if (rc != 0) {
        *error = rc;
        co_return nullptr;
    }
    lsmt::HeaderTrailer tht;
    try {
        tht = lsmt::HeaderTrailer::parse(region);
    } catch (const std::exception&) {
        *error = -EINVAL;
        co_return nullptr;
    }
    if (!tht.is_trailer() || !tht.is_data_file()) {
        *error = -EINVAL;
        co_return nullptr;
    }
    if (tht.is_sealed()) {
        *error = -EALREADY;
        co_return nullptr;
    }
    if (tht.virtual_size == 0 || tht.virtual_size % kSector != 0 ||
        tht.index_size > lsmt::kMaxRoIndexSize) {
        *error = -EINVAL;
        co_return nullptr;
    }

    // Cross-check the on-disk header (offset 0) against the trailer. A
    // trailer torn mid-write can otherwise parse as a valid but EMPTY
    // checkpoint — silently sealing an empty layer — and a trailer alone
    // is forgeable by the block-device guest. The header is written at
    // create() with a random uuid the guest never sees, so agreement of
    // uuid and virtual_size authenticates the checkpoint.
    rc = co_await read_all(fd, region, sizeof(region), 0);
    if (rc != 0) {
        *error = rc;
        co_return nullptr;
    }
    lsmt::HeaderTrailer hht;
    try {
        hht = lsmt::HeaderTrailer::parse(region);
    } catch (const std::exception&) {
        *error = -EINVAL;
        co_return nullptr;
    }
    if (!hht.is_header() || !hht.is_data_file() || hht.is_sealed() ||
        hht.uuid != tht.uuid || hht.virtual_size != tht.virtual_size) {
        *error = -EINVAL;
        co_return nullptr;
    }

    const uint64_t index_bytes =
        tht.index_size * bytes::segment_mapping::kEncodedSize;
    // Bounds: index_offset must lie inside [kSpace, trailer_offset] BEFORE
    // any subtraction — with index_size == 0 an out-of-range index_offset
    // would otherwise underflow the check below and seal an empty layer
    // instead of rejecting the malformed checkpoint.
    if (tht.index_offset < lsmt::kSpace ||
        tht.index_offset > trailer_offset ||
        index_bytes > trailer_offset - tht.index_offset) {
        *error = -EINVAL;
        co_return nullptr;
    }

    // Load and validate the checkpointed index (LsmtLayer::open rules:
    // drop invalid entries, clear tags, strict ordering, moffset range).
    std::vector<uint8_t> raw(index_bytes);
    if (index_bytes > 0) {
        rc = co_await read_all(fd, raw.data(), index_bytes, tht.index_offset);
        if (rc != 0) {
            *error = rc;
            co_return nullptr;
        }
    }
    std::vector<bytes::segment_mapping> segments;
    segments.reserve(tht.index_size);
    for (uint64_t i = 0; i < tht.index_size; ++i) {
        auto s = bytes::load_segment_le(
            raw.data() + i * bytes::segment_mapping::kEncodedSize);
        if (s.offset == bytes::segment_mapping::kInvalidOffset) continue;
        s.tag = 0;
        segments.push_back(s);
    }
    for (size_t i = 1; i < segments.size(); ++i) {
        if (segments[i - 1].end() > segments[i].offset) {
            *error = -EINVAL;
            co_return nullptr;
        }
    }
    const uint64_t moffset_end = tht.index_offset / kSector;
    for (const auto& m : segments) {
        const bool ok = m.zeroed
                            ? (kHeaderSectors <= m.moffset &&
                               m.moffset <= moffset_end)
                            : (kHeaderSectors <= m.moffset &&
                               m.moffset < moffset_end &&
                               kHeaderSectors < m.mend() &&
                               m.mend() <= moffset_end);
        if (!ok) {
            *error = -EINVAL;
            co_return nullptr;
        }
    }

    layer->vsize_.store(tht.virtual_size,
                             std::memory_order_release);
    layer->data_end_sector_ = tht.index_offset / kSector;
    layer->uuid_ = tht.uuid;
    layer->path_ = path;
    // terminal: no more pwrite/discard
    layer->checkpointed_.store(true, std::memory_order_release);
    layer->segments_ = std::move(segments);
    layer->data_bytes_.store(static_cast<uint64_t>(st.st_size),
                             std::memory_order_release);
    layer->view_ = std::make_unique<LsmtRwLayer::View>(
        fd, &layer->data_bytes_, "lsmt-rw:" + path);
    co_return layer;
}

elio::coro::task<int> LsmtRwLayer::seal_file(const std::string& path,
                                             const std::string& user_tag,
                                             std::string* sha256_hex,
                                             uint64_t* size,
                                             uint64_t virtual_size,
                                             std::string* reject) {
    int err = 0;
    auto layer = co_await open_checkpointed(path, &err);
    if (!layer) co_return err;
    if (virtual_size > 0) {
        // D3 commit re-baseline: grow-only vs the layer's declared size
        // AND its content extent (the highest covered sector end — the
        // checkpoint index is loaded by open_checkpointed). An override
        // smaller than either would re-baseline below existing content
        // or below what the layer already declares (shrink), which is
        // rejected here with a precise reason BEFORE any compaction, so
        // the upper stays committable at the declared size.
        if (virtual_size % kSector != 0) {
            if (reject != nullptr) {
                *reject = "commit virtual_size must be a positive multiple "
                          "of 512 bytes";
            }
            co_return -EINVAL;
        }
        const uint64_t declared = layer->virtual_size();
        if (virtual_size < declared) {
            if (reject != nullptr) {
                *reject = "commit virtual_size " +
                          std::to_string(virtual_size) +
                          " is smaller than the layer's declared size " +
                          std::to_string(declared) +
                          " (grow-only re-baseline)";
            }
            co_return -EINVAL;
        }
        uint64_t content_extent = 0;
        for (const auto& s : layer->segments()) {
            content_extent = std::max(content_extent, s.end() * kSector);
        }
        if (virtual_size < content_extent) {
            if (reject != nullptr) {
                *reject = "commit virtual_size " +
                          std::to_string(virtual_size) +
                          " is smaller than the layer's content extent " +
                          std::to_string(content_extent) +
                          " (grow-only re-baseline)";
            }
            co_return -EINVAL;
        }
        // The seal writes header/trailer and hashes virtual_size from
        // the layer's vsize_ member; re-baseline by overriding it.
        layer->vsize_.store(virtual_size, std::memory_order_release);
    }
    const int rc = co_await layer->seal(user_tag);
    if (rc != 0) co_return rc;
    layer.reset();  // seal renamed over path; drop the stale inode's fd

    // Digest and size of the sealed file: a second streaming pass over a
    // fresh fd (seal is a cold control path; simplicity beats hashing
    // during compaction).
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) co_return -errno;
    FdGuard input(fd);
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        co_return -errno;
    }
    common::Sha256 hash;
    std::vector<uint8_t> buf(1 << 20);
    uint64_t off = 0;
    const uint64_t total = static_cast<uint64_t>(st.st_size);
    while (off < total) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(
            static_cast<uint64_t>(buf.size()), total - off));
        const int rrc = co_await read_all(fd, buf.data(), n, off);
        if (rrc != 0) {
            co_return rrc;
        }
        hash.update(buf.data(), n);
        off += n;
    }
    if (sha256_hex) *sha256_hex = hash.final_hex();
    if (size) *size = total;
    co_return 0;
}

elio::coro::task<int> create_empty_lsmt_layer(const std::string& path,
                                              uint64_t vsize,
                                              const std::string& user_tag) {
    if (vsize == 0 || vsize % kSector != 0) co_return -EINVAL;
    const int fd =
        ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) co_return -errno;

    // ADR-0014 content-digest rule (see seal()): sha256(vsize as LE u64 ||
    // packed data || packed index). An empty layer has no data and no
    // index entries, so the digest is a pure function of vsize — the
    // sealed uuid below is derived from it, making the file bytes a pure
    // function of vsize (identical vsize → identical file).
    common::Sha256 content_hash;
    uint8_t vsize_le[8];
    bytes::store_u64_le(vsize_le, vsize);
    content_hash.update(vsize_le, sizeof(vsize_le));
    const std::string uuid =
        uuid_from_content_digest(content_hash.final_hex());

    // Layout: header region (sectors 0..7) | trailer region (the file's
    // last 4096 bytes) at index_offset = 4096. index_size = 0 with
    // index_offset at the first allowed position keeps every reader bound
    // check (LsmtLayer::open) satisfiable and byte-matches seal()'s output
    // for an upper that never received a write (index_sector = 8).
    constexpr uint64_t index_offset = kHeaderSectors * kSector;  // 4096
    struct UnlinkOnError {
        const std::string& path;
        bool ok = true;
        ~UnlinkOnError() {
            if (!ok) ::unlink(path.c_str());
        }
    } cleanup{path};
    uint8_t region[4096];
    std::memset(region, 0, sizeof(region));
    const auto header =
        make_ht(/*header=*/true, /*sealed=*/true, index_offset,
                /*index_size=*/0, vsize, uuid, user_tag);
    header.serialize(region);
    int rc = co_await write_all(fd, region, sizeof(region), 0);
    if (rc != 0) {
        cleanup.ok = false;
        ::close(fd);
        co_return rc;
    }

    std::memset(region, 0, sizeof(region));
    const auto trailer =
        make_ht(/*header=*/false, /*sealed=*/true, index_offset,
                /*index_size=*/0, vsize, uuid, user_tag);
    trailer.serialize(region);
    rc = co_await write_all(fd, region, sizeof(region), index_offset);
    if (rc != 0) {
        cleanup.ok = false;
        ::close(fd);
        co_return rc;
    }
    if (::fdatasync(fd) != 0) {
        cleanup.ok = false;
        const int e = -errno;
        ::close(fd);
        co_return e;
    }
    ::close(fd);
    co_return 0;
}

}  // namespace obd::format
