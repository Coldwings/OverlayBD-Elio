// Sparse writable layer. See sparse_rw.hpp.
#include "format/sparse_rw.hpp"
#include "format/sparse_extent_scan.hpp"
#include "format/sparse_export_plan.hpp"
#include "format/sparse_export_output.hpp"
#include "common/sha256.hpp"

#include "common/crc32c.hpp"
#include "common/errors.hpp"
#include "format/lsmt_format.hpp"

#include <elio/io/file_transfer.hpp>
#include <elio/io/file_operations.hpp>
#include <elio/runtime/spawn_blocking.hpp>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace obd::format {

#ifdef OBD_TEST_HOOKS
std::shared_ptr<std::atomic<int>> SparseRwLayer::open_park_for_test_;
std::atomic<int> SparseRwLayer::coverage_install_fail_stage_for_test_{0};
std::atomic<int> SparseRwLayer::extent_exit_stage_for_test_{0};
#endif

namespace {

constexpr uint64_t kSector = 512;
constexpr uint64_t kMaxSegLen = bytes::segment_mapping::kMaxLength;
constexpr uint64_t kMaxZeroMaskVsize =
    bytes::segment_mapping::kInvalidOffset * kSector;
constexpr size_t kZeroMaskHeaderSize = 32;
constexpr char kZeroMaskMagic[8] = {'O', 'B', 'D', 'S', 'P', 'Z', 'M', '1'};

// Coverage v2 stores four 512-byte sector states per byte. Each logical page
// therefore describes 8 MiB. Two independently checksummed slots let a
// process that dies during publication fall back to the preceding committed
// page instead of guessing from a torn update.
constexpr size_t kCoverageHeaderSize = 4096;
constexpr size_t kCoveragePayloadSize = 4096;
constexpr uint64_t kCoverageSectorsPerPage = kCoveragePayloadSize * 4;
constexpr size_t kCoverageSlotSize = 4160;
constexpr size_t kCoverageSlotBodySize = 4152;
constexpr size_t kCoverageSlotCrcOffset = 24 + kCoveragePayloadSize;
constexpr size_t kCoverageSlotCommitOffset = kCoverageSlotBodySize;
constexpr uint64_t kCoveragePageStride = kCoverageSlotSize * 2;
constexpr char kCoverageMagic[8] = {'O', 'B', 'D', 'S', 'P', 'C', 'V', '2'};
constexpr char kCoverageSlotMagic[8] = {'O', 'B', 'D', 'C', 'V', 'P', '2', '\0'};
constexpr uint32_t kCoverageVersion = 2;
constexpr uint32_t kCoverageFlagXattrIdentity = 1u << 0;
constexpr uint32_t kCoverageFlagLegacyImport = 1u << 1;
constexpr uint32_t kCoverageFlagSnapshotComplete = 1u << 2;
constexpr uint64_t kCoverageCommit = 0xc04d495454454432ULL;
constexpr std::string_view kCoverageIdentityXattr =
    "user.overlaybd.sparse-coverage-id";

enum class CoverageState : uint8_t {
    kInherit = 0,
    kData = 1,
    kZero = 2,
};

template <typename T>
struct OwnedBlockingResult {
    std::optional<T> value;
    std::exception_ptr error;
};

// GCC 12 can give a non-trivial closure captured directly by a co_awaited
// temporary two live owners (the awaitable and the coroutine frame). Transfer
// that closure to the heap before suspension and let a pointer-only closure
// hand it to the blocking worker. The boxed result keeps worker exceptions
// distinct from admission failures, so the latter can reclaim an unsubmitted
// job without racing a job that already took ownership.
template <typename Job>
elio::coro::task<std::invoke_result_t<Job&>> run_owned_blocking(Job* job) {
    using Result = std::invoke_result_t<Job&>;
    static_assert(!std::is_reference_v<Result>);
    static_assert(std::is_nothrow_move_constructible_v<Result>);
    OwnedBlockingResult<Result> outcome;
    try {
        outcome = co_await elio::spawn_blocking(
            [job]() noexcept -> OwnedBlockingResult<Result> {
                std::unique_ptr<Job> owned(job);
                OwnedBlockingResult<Result> result;
                try {
                    result.value.emplace((*owned)());
                } catch (...) {
                    result.error = std::current_exception();
                }
                return result;
            });
    } catch (...) {
        delete job;
        throw;
    }
    if (outcome.error) std::rethrow_exception(outcome.error);
    co_return std::move(*outcome.value);
}

uint64_t coverage_page_count(uint64_t vsize) {
    const uint64_t sectors = vsize / kSector;
    return (sectors + kCoverageSectorsPerPage - 1) /
           kCoverageSectorsPerPage;
}

uint64_t coverage_file_size(uint64_t vsize) {
    return kCoverageHeaderSize +
           coverage_page_count(vsize) * kCoveragePageStride;
}

uint64_t coverage_slot_offset(uint64_t page, uint8_t slot) {
    return kCoverageHeaderSize + page * kCoveragePageStride +
           static_cast<uint64_t>(slot) * kCoverageSlotSize;
}

void set_coverage_state(std::array<uint8_t, kCoveragePayloadSize>& payload,
                        uint64_t sector_in_page, CoverageState state) {
    const size_t byte = static_cast<size_t>(sector_in_page / 4);
    const unsigned shift = static_cast<unsigned>((sector_in_page % 4) * 2);
    payload[byte] = static_cast<uint8_t>(
        (payload[byte] & ~(uint8_t{3} << shift)) |
        (static_cast<uint8_t>(state) << shift));
}

CoverageState get_coverage_state(
    const std::array<uint8_t, kCoveragePayloadSize>& payload,
    uint64_t sector_in_page) {
    const size_t byte = static_cast<size_t>(sector_in_page / 4);
    const unsigned shift = static_cast<unsigned>((sector_in_page % 4) * 2);
    return static_cast<CoverageState>((payload[byte] >> shift) & 3u);
}

struct SyncMutexGuard {
    elio::sync::mutex* mu = nullptr;

    explicit SyncMutexGuard(elio::sync::mutex& m) : mu(&m) {}
    ~SyncMutexGuard() {
        if (mu != nullptr) mu->unlock();
    }

    SyncMutexGuard(const SyncMutexGuard&) = delete;
    SyncMutexGuard& operator=(const SyncMutexGuard&) = delete;
};

struct AdoptActiveOp {};

struct ActiveOpGuard {
    std::atomic<uint32_t>& active;

    explicit ActiveOpGuard(std::atomic<uint32_t>& a, AdoptActiveOp)
        : active(a) {}
    ~ActiveOpGuard() { active.fetch_sub(1, std::memory_order_acq_rel); }

    ActiveOpGuard(const ActiveOpGuard&) = delete;
    ActiveOpGuard& operator=(const ActiveOpGuard&) = delete;
};

struct FdGuard {
    int fd = -1;
    const std::string* unlink_path = nullptr;

    explicit FdGuard(int value, const std::string* path = nullptr)
        : fd(value), unlink_path(path) {}
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    int release() {
        unlink_path = nullptr;
        const int value = fd;
        fd = -1;
        return value;
    }
    ~FdGuard() {
        if (fd >= 0) ::close(fd);
        if (unlink_path != nullptr) ::unlink(unlink_path->c_str());
    }
    int close() {
        if (fd < 0) return 0;
        const int value = fd;
        fd = -1;
        if (::close(value) != 0) return errno;
        return 0;
    }
    void dismiss_unlink() { unlink_path = nullptr; }
};

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

int fsync_dir(const std::string& dir) {
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

std::array<uint8_t, 16> random_coverage_identity() {
    std::array<uint8_t, 16> id {};
    size_t done = 0;
    while (done < id.size()) {
        const ssize_t rc = ::getrandom(id.data() + done, id.size() - done, 0);
        if (rc < 0) {
            if (errno == EINTR) continue;
            throw_errno(errno, "cannot generate sparse coverage identity");
        }
        if (rc == 0) throw_errno(EIO, "cannot generate sparse coverage identity");
        done += static_cast<size_t>(rc);
    }
    return id;
}

enum class XattrIdentityStatus {
    kPresent,
    kMissing,
    kUnsupported,
};

XattrIdentityStatus read_xattr_identity(int fd, std::array<uint8_t, 16>* id) {
    const ssize_t n = ::fgetxattr(fd, kCoverageIdentityXattr.data(), id->data(),
                                  id->size());
    if (n == static_cast<ssize_t>(id->size())) {
        return XattrIdentityStatus::kPresent;
    }
    if (n >= 0) {
        throw format_error("sparse coverage identity has invalid size");
    }
    if (errno == ENODATA) return XattrIdentityStatus::kMissing;
    if (errno == ENOTSUP || errno == EOPNOTSUPP) {
        return XattrIdentityStatus::kUnsupported;
    }
    throw_errno(errno, "cannot read sparse coverage identity");
}

struct CoverageIdentity {
    std::array<uint8_t, 16> value {};
    bool xattr = false;
};

std::array<uint8_t, kCoverageHeaderSize> make_coverage_header(
    uint64_t initial_vsize, const struct stat& data_st,
    const CoverageIdentity& identity, bool legacy_import) {
    std::array<uint8_t, kCoverageHeaderSize> raw {};
    std::memcpy(raw.data(), kCoverageMagic, sizeof(kCoverageMagic));
    bytes::store_u32_le(raw.data() + 8, kCoverageVersion);
    bytes::store_u32_le(raw.data() + 12, kCoverageHeaderSize);
    bytes::store_u32_le(raw.data() + 16, kSector);
    uint32_t flags = identity.xattr ? kCoverageFlagXattrIdentity : 0;
    if (legacy_import) flags |= kCoverageFlagLegacyImport;
    bytes::store_u32_le(raw.data() + 20, flags);
    bytes::store_u64_le(raw.data() + 24, initial_vsize);
    bytes::store_u64_le(raw.data() + 32,
                        static_cast<uint64_t>(data_st.st_dev));
    bytes::store_u64_le(raw.data() + 40,
                        static_cast<uint64_t>(data_st.st_ino));
    std::memcpy(raw.data() + 48, identity.value.data(), identity.value.size());
    bytes::store_u32_le(raw.data() + 64,
                        crc32::crc32c(raw.data(), 64));
    return raw;
}

struct CoverageHeaderBinding {
    uint32_t flags = 0;
    std::array<uint8_t, 16> identity {};
};

CoverageHeaderBinding read_coverage_binding(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw_errno(errno, "cannot open sparse coverage " + path);
    FdGuard guard(fd);
    std::array<uint8_t, kCoverageHeaderSize> header {};
    const int rc = pread_all_sync(fd, header.data(), header.size(), 0);
    if (rc != 0) throw_errno(rc, "cannot read sparse coverage " + path);
    if (std::memcmp(header.data(), kCoverageMagic, sizeof(kCoverageMagic)) !=
            0 ||
        bytes::load_u32_le(header.data() + 8) != kCoverageVersion ||
        bytes::load_u32_le(header.data() + 12) != kCoverageHeaderSize ||
        bytes::load_u32_le(header.data() + 16) != kSector ||
        bytes::load_u32_le(header.data() + 64) !=
            crc32::crc32c(header.data(), 64)) {
        throw format_error("sparse coverage has invalid header: " + path);
    }
    CoverageHeaderBinding binding;
    binding.flags = bytes::load_u32_le(header.data() + 20);
    if ((binding.flags & ~(kCoverageFlagXattrIdentity |
                           kCoverageFlagLegacyImport |
                           kCoverageFlagSnapshotComplete)) != 0) {
        throw format_error("sparse coverage has unsupported flags: " + path);
    }
    std::memcpy(binding.identity.data(), header.data() + 48,
                binding.identity.size());
    return binding;
}

std::array<uint8_t, kCoverageSlotSize> make_coverage_slot(
    uint64_t page, uint64_t generation,
    const std::array<uint8_t, kCoveragePayloadSize>& payload) {
    std::array<uint8_t, kCoverageSlotSize> raw {};
    std::memcpy(raw.data(), kCoverageSlotMagic, sizeof(kCoverageSlotMagic));
    bytes::store_u64_le(raw.data() + 8, page);
    bytes::store_u64_le(raw.data() + 16, generation);
    std::memcpy(raw.data() + 24, payload.data(), payload.size());
    bytes::store_u32_le(raw.data() + kCoverageSlotCrcOffset,
                        crc32::crc32c(raw.data(), kCoverageSlotCrcOffset));
    return raw;
}

elio::coro::task<int> pwrite_all_async(int fd, const void* buf, size_t count,
                                      uint64_t offset) {
    const auto result = co_await elio::io::pwrite_exactly(
        fd,
        std::span<const std::byte>(static_cast<const std::byte*>(buf), count),
        offset);
    if (result.error) co_return -result.error.value();
    co_return result.end == elio::io::transfer_end::complete ? 0 : -EIO;
}

std::optional<uint64_t> read_zero_mask_vsize(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return std::nullopt;
        throw_errno(errno, "cannot open sparse zero mask " + path);
    }
    FdGuard guard(fd);

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int e = errno;
        throw_errno(e, "cannot stat sparse zero mask " + path);
    }
    if (st.st_size < static_cast<off_t>(kZeroMaskHeaderSize)) {
        throw format_error("sparse zero mask is truncated: " + path);
    }

    std::array<uint8_t, kZeroMaskHeaderSize> hdr {};
    const int rc = pread_all_sync(fd, hdr.data(), hdr.size(), 0);
    if (rc != 0) {
        throw_errno(rc, "cannot read sparse zero mask " + path);
    }
    const int close_rc = guard.close();
    if (close_rc != 0) {
        throw_errno(close_rc, "cannot close sparse zero mask " + path);
    }
    if (std::memcmp(hdr.data(), kZeroMaskMagic, sizeof(kZeroMaskMagic)) != 0) {
        throw format_error("sparse zero mask has bad magic: " + path);
    }

    const uint64_t stored_vsize = bytes::load_u64_le(hdr.data() + 8);
    if (stored_vsize == 0 || stored_vsize % kSector != 0 ||
        stored_vsize > kMaxZeroMaskVsize) {
        throw format_error("sparse zero mask vsize mismatch: " + path);
    }
    return stored_vsize;
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

void push_coalesced(std::vector<bytes::segment_mapping>& v,
                    bytes::segment_mapping m) {
    if (m.length == 0) return;
    if (!v.empty() && can_coalesce(v.back(), m)) {
        v.back().length = static_cast<uint32_t>(
            std::max(v.back().end(), m.end()) - v.back().offset);
        return;
    }
    v.push_back(m);
}

void push_zero_range(std::vector<bytes::segment_mapping>& v, uint64_t lo,
                     uint64_t hi) {
    uint64_t cur = lo;
    while (cur < hi) {
        const uint64_t n = std::min(hi - cur, kMaxSegLen);
        bytes::segment_mapping m;
        m.offset = cur;
        m.length = static_cast<uint32_t>(n);
        m.moffset = 0;
        m.zeroed = true;
        m.tag = 0;
        push_coalesced(v, m);
        cur += n;
    }
}

std::vector<bytes::segment_mapping> zero_mask_segments_from(
    const std::vector<bytes::segment_mapping>& segments) {
    std::vector<bytes::segment_mapping> zeroes;
    for (const auto& s : segments) {
        if (s.zeroed) zeroes.push_back(s);
    }
    return zeroes;
}

std::vector<bytes::segment_mapping> overlay_zero_masks(
    std::vector<bytes::segment_mapping> live,
    const std::vector<bytes::segment_mapping>& zeroes) {
    std::vector<bytes::segment_mapping> out;
    out.reserve(live.size() + zeroes.size());
    size_t i = 0;
    for (const auto& z : zeroes) {
        const uint64_t z_lo = z.offset;
        const uint64_t z_hi = z.end();
        while (i < live.size() && live[i].end() <= z_lo) {
            push_coalesced(out, live[i++]);
        }
        while (i < live.size() && live[i].offset < z_hi) {
            const auto s = live[i];
            const uint64_t s_end = s.end();
            if (s.offset < z_lo) {
                auto head = s;
                const uint64_t head_end = std::min(z_lo, s_end);
                head.length = static_cast<uint32_t>(head_end - s.offset);
                push_coalesced(out, head);
            }
            if (s_end <= z_hi) {
                ++i;
                continue;
            }
            auto tail = s;
            tail.offset = z_hi;
            if (!tail.zeroed) tail.moffset += z_hi - s.offset;
            tail.length = static_cast<uint32_t>(s_end - z_hi);
            live[i] = tail;
            break;
        }
        push_zero_range(out, z_lo, z_hi);
    }
    while (i < live.size()) push_coalesced(out, live[i++]);
    return out;
}

bool range_is_data(const std::vector<bytes::segment_mapping>& segments,
                   uint64_t lo, uint64_t hi) {
    auto it = std::upper_bound(
        segments.begin(), segments.end(), lo,
        [](uint64_t sector, const bytes::segment_mapping& segment) {
            return sector < segment.offset;
        });
    if (it != segments.begin()) --it;
    uint64_t covered = lo;
    while (it != segments.end() && covered < hi) {
        if (it->zeroed || it->offset > covered || it->end() <= covered)
            return false;
        covered = it->end();
        ++it;
    }
    return covered >= hi;
}

void erase_range_from(std::vector<bytes::segment_mapping>& segments,
                      uint64_t lo, uint64_t hi) {
    if (lo >= hi) return;
    std::vector<bytes::segment_mapping> next;
    next.reserve(segments.size());
    for (const auto& s : segments) {
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
    segments.swap(next);
}

void insert_live_extent_into(std::vector<bytes::segment_mapping>& segments,
                             uint64_t off, uint64_t len) {
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
        insert_sorted(segments, m);
        cur += n;
    }
}

void replace_range_from_target(
    std::vector<bytes::segment_mapping>& base,
    const std::vector<bytes::segment_mapping>& target, uint64_t lo,
    uint64_t hi) {
    erase_range_from(base, lo, hi);
    for (const auto& segment : target) {
        if (segment.end() <= lo) continue;
        if (segment.offset >= hi) break;
        auto clipped = segment;
        const uint64_t clipped_lo = std::max(segment.offset, lo);
        const uint64_t clipped_hi = std::min(segment.end(), hi);
        clipped.offset = clipped_lo;
        clipped.length = static_cast<uint32_t>(clipped_hi - clipped_lo);
        if (!clipped.zeroed) {
            clipped.moffset += clipped_lo - segment.offset;
        }
        insert_sorted(base, clipped);
    }
}

std::array<uint8_t, kCoveragePayloadSize> coverage_payload_from_segments(
    const std::vector<bytes::segment_mapping>& segments, uint64_t page) {
    std::array<uint8_t, kCoveragePayloadSize> payload {};
    const uint64_t page_lo = page * kCoverageSectorsPerPage;
    const uint64_t page_hi = page_lo + kCoverageSectorsPerPage;
    for (const auto& segment : segments) {
        if (segment.end() <= page_lo) continue;
        if (segment.offset >= page_hi) break;
        const uint64_t lo = std::max(segment.offset, page_lo);
        const uint64_t hi = std::min(segment.end(), page_hi);
        const CoverageState state = segment.zeroed ? CoverageState::kZero
                                                   : CoverageState::kData;
        uint64_t first = lo - page_lo;
        const uint64_t end = hi - page_lo;
        while (first < end && first % 4 != 0) {
            set_coverage_state(payload, first++, state);
        }
        // Four adjacent sectors share a byte. Fill aligned ownership runs
        // directly instead of repeatedly masking every sector in a dense page.
        const uint64_t whole_bytes = (end - first) / 4;
        const uint8_t packed = static_cast<uint8_t>(state) * uint8_t{0x55};
        std::fill_n(payload.begin() + first / 4, whole_bytes, packed);
        first += whole_bytes * 4;
        while (first < end) {
            set_coverage_state(payload, first++, state);
        }
    }
    return payload;
}

void append_coverage_run(std::vector<bytes::segment_mapping>& segments,
                         uint64_t lo, uint64_t hi, CoverageState state) {
    while (lo < hi) {
        const uint64_t n = std::min(hi - lo, kMaxSegLen);
        bytes::segment_mapping segment;
        segment.offset = lo;
        segment.length = static_cast<uint32_t>(n);
        segment.moffset = state == CoverageState::kData ? lo : 0;
        segment.zeroed = state == CoverageState::kZero;
        segment.tag = 0;
        push_coalesced(segments, segment);
        lo += n;
    }
}

void decode_coverage_payload(
    const std::array<uint8_t, kCoveragePayloadSize>& payload, uint64_t page,
    uint64_t max_sector, uint64_t data_sector_limit,
    std::vector<bytes::segment_mapping>* segments, const std::string& path) {
    const uint64_t page_lo = page * kCoverageSectorsPerPage;
    const uint64_t page_hi = std::min(page_lo + kCoverageSectorsPerPage,
                                      max_sector);
    const uint64_t physical_page_hi = page_lo + kCoverageSectorsPerPage;
    CoverageState run_state = CoverageState::kInherit;
    uint64_t run_lo = page_lo;
    uint64_t sector = page_lo;
    while (sector < physical_page_hi) {
        const CoverageState state =
            get_coverage_state(payload, sector - page_lo);
        if (static_cast<uint8_t>(state) == 3) {
            throw format_error("sparse coverage has invalid sector state: " +
                               path);
        }
        uint64_t next = sector + 1;
        if ((sector - page_lo) % 4 == 0) {
            const uint8_t packed = static_cast<uint8_t>(state) * uint8_t{0x55};
            // Uniform bytes can be validated as one run, including its far
            // endpoint; mixed bytes still inspect every sector state.
            size_t byte = static_cast<size_t>((sector - page_lo) / 4);
            if (payload[byte] == packed) {
                do {
                    ++byte;
                } while (byte < payload.size() && payload[byte] == packed);
                next = page_lo + byte * 4;
            }
        }
        if (next > page_hi && state != CoverageState::kInherit) {
            throw format_error("sparse coverage extends past vsize: " + path);
        }
        if (state == CoverageState::kData && next > data_sector_limit) {
            throw format_error("sparse coverage DATA extends past data EOF: " +
                               path);
        }
        if (state != run_state) {
            if (run_state != CoverageState::kInherit && run_lo < page_hi) {
                append_coverage_run(*segments, run_lo,
                                    std::min(sector, page_hi), run_state);
            }
            run_state = state;
            run_lo = sector;
        }
        sector = next;
    }
    if (run_state != CoverageState::kInherit && run_lo < page_hi) {
        append_coverage_run(*segments, run_lo,
                            std::min(physical_page_hi, page_hi), run_state);
    }
}

}  // namespace

class SparseRwLayer::View final : public source::BlobSource {
public:
    View(int fd, const std::atomic<uint64_t>& size, std::string label)
        : fd_(fd), size_(&size), label_(std::move(label)) {}

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override {
        const uint64_t bound = size_->load(std::memory_order_acquire);
        if (offset >= bound) co_return 0;
        if (count > bound - offset) count = static_cast<size_t>(bound - offset);
        const auto result = co_await elio::io::pread_exactly(
            fd_, std::span<std::byte>(static_cast<std::byte*>(buf), count), offset);
        if (result.error) co_return -result.error.value();
        co_return static_cast<ssize_t>(result.transferred);
    }

    uint64_t size() const noexcept override {
        return size_->load(std::memory_order_acquire);
    }

    std::string_view label() const noexcept override { return label_; }

private:
    int fd_;
    const std::atomic<uint64_t>* size_;
    std::string label_;
};

void SparseRwLayer::create_coverage_snapshot(bool legacy_import) {
    struct stat data_st {};
    if (::fstat(fd_, &data_st) != 0) {
        throw_errno(errno, "cannot stat sparse layer for coverage");
    }
    CoverageIdentity identity;
    const XattrIdentityStatus identity_status =
        read_xattr_identity(fd_, &identity.value);
    if (identity_status == XattrIdentityStatus::kPresent) {
        throw format_error("sparse coverage identity exists without coverage");
    }
    identity.value = random_coverage_identity();
    identity.xattr = identity_status == XattrIdentityStatus::kMissing;
    const uint64_t cur_vsize = vsize_.load(std::memory_order_acquire);
    auto header =
        make_coverage_header(cur_vsize, data_st, identity, legacy_import);

    const std::string tmp = coverage_path_ + ".tmp";
    const int fd = ::open(tmp.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC,
                          0644);
    if (fd < 0) {
        throw_errno(errno, "cannot create sparse coverage " + tmp);
    }
    FdGuard guard(fd, &tmp);

#ifdef OBD_TEST_HOOKS
    if (coverage_install_fail_stage_for_test_.load(std::memory_order_acquire) == -5)
        ::_exit(87);  // Exec-only process-interruption fixture, before header.
#endif
    int rc = pwrite_all_sync(fd, header.data(), header.size(), 0);
    const uint64_t target_size = coverage_file_size(cur_vsize);
    if (rc == 0 && ::ftruncate(fd, static_cast<off_t>(target_size)) != 0) {
        rc = errno;
    }
#ifdef OBD_TEST_HOOKS
    if (rc == 0 && coverage_install_fail_stage_for_test_.load(std::memory_order_acquire) == -3)
        ::_exit(87);  // Valid initial header, no complete-snapshot proof.
#endif

    std::set<uint64_t> pages;
    for (const auto& segment : segments_) {
        if (segment.length == 0) continue;
        const uint64_t first = segment.offset / kCoverageSectorsPerPage;
        const uint64_t last = (segment.end() - 1) / kCoverageSectorsPerPage;
        for (uint64_t page = first; page <= last; ++page) pages.insert(page);
    }
    for (const uint64_t page : pages) {
        if (rc != 0) break;
        const auto payload = coverage_payload_from_segments(segments_, page);
        auto record = make_coverage_slot(page, 1, payload);
        const uint64_t offset = coverage_slot_offset(page, 0);
        rc = pwrite_all_sync(fd, record.data(), kCoverageSlotBodySize, offset);
        if (rc == 0) {
            bytes::store_u64_le(record.data() + kCoverageSlotCommitOffset,
                                kCoverageCommit);
            rc = pwrite_all_sync(fd,
                                 record.data() + kCoverageSlotCommitOffset,
                                 sizeof(uint64_t),
                                 offset + kCoverageSlotCommitOffset);
        }
#ifdef OBD_TEST_HOOKS
        if (rc == 0 && coverage_install_fail_stage_for_test_.load(std::memory_order_acquire) == -4)
            ::_exit(87);  // Stop after the first imported page.
#endif
    }
    if (rc == 0) {
        // An ordinary valid header and sparse length cannot prove that all
        // imported DATA pages were written. Publish this proof only after
        // the complete initial snapshot, before any identity installation.
        bytes::store_u32_le(header.data() + 20,
            bytes::load_u32_le(header.data() + 20) |
                kCoverageFlagSnapshotComplete);
        bytes::store_u32_le(header.data() + 64,
                            crc32::crc32c(header.data(), 64));
        rc = pwrite_all_sync(fd, header.data(), header.size(), 0);
    }
    if (rc == 0 && ::fsync(fd) != 0) rc = errno;
    if (rc == 0) rc = guard.close();
    if (rc != 0) {
        throw_errno(rc, "cannot publish sparse coverage " + tmp);
    }

    // Validate the complete temporary artifact before installing the durable
    // identity marker. load_coverage owns a separate descriptor afterwards.
    load_coverage(tmp, static_cast<uint64_t>(data_st.st_size),
                  CoverageLoadMode::kInstalling);
    // From here onward tmp is the only recovery artifact. Preserve it if
    // marker installation, inode sync, rename, or directory sync fails.
    guard.dismiss_unlink();
#ifdef OBD_TEST_HOOKS
    if (coverage_install_fail_stage_for_test_.load(std::memory_order_acquire) == -6)
        ::_exit(87);  // Complete snapshot exists before data-file expansion.
#endif
    // A non-sparse filesystem may materialize this expansion as zero-filled
    // DATA. Freeze the pre-expansion ownership first so an interrupted fresh
    // creation or legacy grow never imports those new bytes as guest writes.
    if (::ftruncate(fd_, static_cast<off_t>(cur_vsize)) != 0) {
        throw_errno(errno, "cannot size sparse layer before coverage install");
    }
#ifdef OBD_TEST_HOOKS
    if (coverage_install_fail_stage_for_test_.load(std::memory_order_acquire) == -7)
        ::_exit(87);  // Recovery must use the complete snapshot, not extents.
#endif
    if (identity.xattr) {
        if (::fsetxattr(fd_, kCoverageIdentityXattr.data(),
                       identity.value.data(), identity.value.size(),
                       XATTR_CREATE) != 0) {
            throw_errno(errno, "cannot persist sparse coverage identity");
        }
    }
#ifdef OBD_TEST_HOOKS
    int expected_install_failure = 1;
    if (coverage_install_fail_stage_for_test_.compare_exchange_strong(
            expected_install_failure, 0, std::memory_order_acq_rel)) {
        throw_errno(EIO, "injected sparse coverage data sync failure");
    }
#endif
    // The marker and any preceding grow must be durable before the sidecar
    // name becomes authoritative.
    if (::fsync(fd_) != 0) {
        throw_errno(errno, "cannot sync sparse coverage identity");
    }
#ifdef OBD_TEST_HOOKS
    expected_install_failure = 2;
    if (coverage_install_fail_stage_for_test_.compare_exchange_strong(
            expected_install_failure, 0, std::memory_order_acq_rel)) {
        throw_errno(EIO, "injected sparse coverage rename failure");
    }
#endif
    if (::rename(tmp.c_str(), coverage_path_.c_str()) != 0) {
        throw_errno(errno, "cannot install sparse coverage " + coverage_path_);
    }
    const int drc = fsync_parent_dir(coverage_path_);
    if (drc != 0) {
        throw_errno(drc, "cannot sync sparse coverage directory " +
                             coverage_path_);
    }
    (void)guard.release();
    coverage_dirty_ = false;
}

void SparseRwLayer::load_coverage(const std::string& artifact_path,
                                  uint64_t data_size, CoverageLoadMode mode) {
    const bool installing = mode == CoverageLoadMode::kInstalling;
    const int fd = ::open(artifact_path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        throw_errno(errno, "cannot open sparse coverage " + artifact_path);
    }
    FdGuard guard(fd);
    struct stat coverage_st {};
    struct stat data_st {};
    if (::fstat(fd, &coverage_st) != 0 || ::fstat(fd_, &data_st) != 0) {
        throw_errno(errno, "cannot stat sparse coverage " + artifact_path);
    }
    if (coverage_st.st_size < static_cast<off_t>(kCoverageHeaderSize)) {
        throw format_error("sparse coverage is truncated: " + artifact_path);
    }

    std::array<uint8_t, kCoverageHeaderSize> header {};
    int rc = pread_all_sync(fd, header.data(), header.size(), 0);
    if (rc != 0) {
        throw_errno(rc, "cannot read sparse coverage " + artifact_path);
    }
    if (std::memcmp(header.data(), kCoverageMagic, sizeof(kCoverageMagic)) !=
            0 ||
        bytes::load_u32_le(header.data() + 8) != kCoverageVersion ||
        bytes::load_u32_le(header.data() + 12) != kCoverageHeaderSize ||
        bytes::load_u32_le(header.data() + 16) != kSector ||
        bytes::load_u32_le(header.data() + 64) !=
            crc32::crc32c(header.data(), 64)) {
        throw format_error("sparse coverage has invalid header: " +
                           artifact_path);
    }
    const uint32_t flags = bytes::load_u32_le(header.data() + 20);
    if ((flags & ~(kCoverageFlagXattrIdentity |
                   kCoverageFlagLegacyImport |
                   kCoverageFlagSnapshotComplete)) != 0) {
        throw format_error("sparse coverage has unsupported flags: " +
                           artifact_path);
    }
    if ((flags & kCoverageFlagSnapshotComplete) == 0) {
        throw format_error("sparse coverage initial snapshot is incomplete: " +
                           artifact_path);
    }
    const uint64_t initial_vsize = bytes::load_u64_le(header.data() + 24);
    const uint64_t cur_vsize = vsize_.load(std::memory_order_acquire);
    if (initial_vsize == 0 || initial_vsize % kSector != 0 ||
        initial_vsize > cur_vsize || initial_vsize > kMaxZeroMaskVsize) {
        throw format_error("sparse coverage vsize mismatch: " + artifact_path);
    }
    if ((flags & kCoverageFlagXattrIdentity) != 0) {
        std::array<uint8_t, 16> identity {};
        const XattrIdentityStatus status = read_xattr_identity(fd_, &identity);
        if (status == XattrIdentityStatus::kPresent) {
            if (std::memcmp(identity.data(), header.data() + 48,
                            identity.size()) == 0) {
                // A matching durable marker supports marker-preserving copy
                // or replacement even when device/inode changed.
            } else {
                throw format_error("sparse coverage data identity mismatch: " +
                                   artifact_path);
            }
        } else if (!installing || status != XattrIdentityStatus::kMissing ||
                   bytes::load_u64_le(header.data() + 32) !=
                       static_cast<uint64_t>(data_st.st_dev) ||
                   bytes::load_u64_le(header.data() + 40) !=
                       static_cast<uint64_t>(data_st.st_ino)) {
            // Until the marker is installed, the snapshot must still belong
            // to the original inode. A temporary name grants no identity.
            throw format_error("sparse coverage data identity mismatch: " +
                               artifact_path);
        }
    } else if (bytes::load_u64_le(header.data() + 32) !=
                   static_cast<uint64_t>(data_st.st_dev) ||
               bytes::load_u64_le(header.data() + 40) !=
                   static_cast<uint64_t>(data_st.st_ino)) {
        throw format_error("sparse coverage data identity mismatch: " +
                           artifact_path);
    }
    // An installing snapshot may precede expansion of its still-bound data
    // inode. Every DATA sector is nevertheless checked against the original
    // EOF below. Authoritative files retain the strict truncation rejection.
    if (!installing && data_size < initial_vsize) {
        throw format_error("sparse coverage DATA file is shorter than its "
                           "initial vsize: " + artifact_path);
    }

    const uint64_t original_size = static_cast<uint64_t>(coverage_st.st_size);
    const uint64_t min_size = coverage_file_size(initial_vsize);
    const uint64_t target_size = coverage_file_size(cur_vsize);
    if (original_size < min_size || original_size > target_size ||
        (original_size - kCoverageHeaderSize) % kCoveragePageStride != 0) {
        throw format_error("sparse coverage size mismatch: " + artifact_path);
    }
    const uint64_t page_limit = coverage_page_count(cur_vsize);
    std::set<uint64_t> candidates;
    bool scan_all = false;
    uint64_t pos = kCoverageHeaderSize;
    while (pos < original_size) {
        const off_t data = ::lseek(fd, static_cast<off_t>(pos), SEEK_DATA);
        if (data < 0) {
            if (errno == ENXIO) break;
            if (errno == EINVAL || errno == ENOTSUP) {
                scan_all = true;
                break;
            }
            throw_errno(errno, "cannot enumerate sparse coverage " +
                                   artifact_path);
        }
        const off_t hole = ::lseek(fd, data, SEEK_HOLE);
        if (hole < 0 || hole <= data) {
            if (hole < 0 && (errno == EINVAL || errno == ENOTSUP)) {
                scan_all = true;
                break;
            }
            throw_errno(hole < 0 ? errno : EIO,
                        "cannot enumerate sparse coverage " + artifact_path);
        }
        const uint64_t begin =
            static_cast<uint64_t>(data) <= kCoverageHeaderSize
                ? 0
                : (static_cast<uint64_t>(data) - kCoverageHeaderSize) /
                      kCoveragePageStride;
        const uint64_t bounded_hole =
            std::min<uint64_t>(static_cast<uint64_t>(hole), original_size);
        if (bounded_hole > kCoverageHeaderSize) {
            const uint64_t end =
                (bounded_hole - 1 - kCoverageHeaderSize) /
                kCoveragePageStride;
            for (uint64_t page = begin;
                 page <= end && page < page_limit; ++page) {
                candidates.insert(page);
            }
        }
        pos = bounded_hole;
    }
    if (scan_all) {
        const uint64_t stored_pages =
            (original_size - kCoverageHeaderSize) / kCoveragePageStride;
        for (uint64_t page = 0; page < stored_pages; ++page) {
            candidates.insert(page);
        }
    }

    segments_.clear();
    coverage_pages_.clear();
    const uint64_t max_sector = cur_vsize / kSector;
    for (const uint64_t page : candidates) {
        struct ValidSlot {
            bool valid = false;
            uint64_t generation = 0;
            uint8_t slot = 0;
            std::array<uint8_t, kCoveragePayloadSize> payload {};
        } best;
        bool committed_invalid = false;
        for (uint8_t slot = 0; slot < 2; ++slot) {
            const uint64_t offset = coverage_slot_offset(page, slot);
            if (offset + kCoverageSlotSize > original_size) continue;
            std::array<uint8_t, kCoverageSlotSize> raw {};
            rc = pread_all_sync(fd, raw.data(), raw.size(), offset);
            if (rc != 0) {
                throw_errno(rc, "cannot read sparse coverage page " +
                                    artifact_path);
            }
            if (bytes::load_u64_le(raw.data() + kCoverageSlotCommitOffset) !=
                kCoverageCommit) {
                continue;  // inactive or interrupted publication
            }
            const uint64_t generation = bytes::load_u64_le(raw.data() + 16);
            if (std::memcmp(raw.data(), kCoverageSlotMagic,
                            sizeof(kCoverageSlotMagic)) != 0 ||
                bytes::load_u64_le(raw.data() + 8) != page ||
                generation == 0 ||
                bytes::load_u32_le(raw.data() + kCoverageSlotCrcOffset) !=
                    crc32::crc32c(raw.data(), kCoverageSlotCrcOffset)) {
                // Treat a committed but corrupt candidate as a damaged slot,
                // not as proof that the rollback slot is unusable. Fail closed
                // only when no complete slot for this page survives.
                committed_invalid = true;
                continue;
            }
            if (!best.valid || generation > best.generation) {
                best.valid = true;
                best.generation = generation;
                best.slot = slot;
                std::memcpy(best.payload.data(), raw.data() + 24,
                            best.payload.size());
            } else if (generation == best.generation) {
                throw format_error("sparse coverage page generation conflicts: " +
                                   artifact_path);
            }
        }
        if (!best.valid) {
            if (committed_invalid) {
                throw format_error("sparse coverage page is corrupt: " +
                                   artifact_path);
            }
            continue;
        }
        coverage_pages_.emplace(
            page, CoveragePageVersion{best.generation, best.slot});
        decode_coverage_payload(best.payload, page, max_sector,
                                data_size / kSector, &segments_,
                                artifact_path);
    }
    coverage_fd_ = guard.release();
    // A completed prior-process operation may exist only in the surviving
    // page cache. Reopen cannot infer that a successful FLUSH persisted it.
    coverage_dirty_ = true;
    // DISCARD can commit exact ZERO before reaching the compatibility sidecar.
    // Reestablish a complete protective mask before a recovered ZERO-to-DATA
    // write can publish only a prefix and retain ZERO in its runtime view.
    if (has_zero_masks_locked()) zero_masks_dirty_ = true;
}

elio::coro::task<int> SparseRwLayer::publish_coverage_locked(
    const std::vector<bytes::segment_mapping>& exact_next,
    const std::vector<bytes::segment_mapping>& runtime_next, uint64_t lo,
    uint64_t hi, uint64_t* committed_hi) {
    *committed_hi = lo;
    if (lo >= hi) co_return 0;
    const uint64_t first = lo / kCoverageSectorsPerPage;
    const uint64_t last = (hi - 1) / kCoverageSectorsPerPage;
    for (uint64_t page = first; page <= last; ++page) {
        // Prepare the map node before the first externally visible write.
        // Once the commit word lands, every remaining operation in this page
        // transition must be non-throwing.
        bool inserted = false;
        decltype(coverage_pages_)::iterator version;
        std::vector<bytes::segment_mapping> prepared_runtime;
        try {
#ifdef OBD_TEST_HOOKS
            if (fail_coverage_map_prepare_page_for_test_ &&
                *fail_coverage_map_prepare_page_for_test_ == page) {
                fail_coverage_map_prepare_page_for_test_.reset();
                throw std::bad_alloc();
            }
#endif
            auto prepared = coverage_pages_.try_emplace(
                page, CoveragePageVersion{});
            version = prepared.first;
            inserted = prepared.second;
#ifdef OBD_TEST_HOOKS
            if (fail_coverage_runtime_prepare_page_for_test_ &&
                *fail_coverage_runtime_prepare_page_for_test_ == page) {
                fail_coverage_runtime_prepare_page_for_test_.reset();
                throw std::bad_alloc();
            }
#endif
            const uint64_t page_lo = page * kCoverageSectorsPerPage;
            const uint64_t page_hi = page_lo + kCoverageSectorsPerPage;
            {
                std::lock_guard lock(meta_mu_);
                prepared_runtime = segments_;
            }
            replace_range_from_target(prepared_runtime, runtime_next,
                                      std::max(lo, page_lo),
                                      std::min(hi, page_hi));
        } catch (const std::bad_alloc&) {
            if (inserted) coverage_pages_.erase(version);
            co_return -ENOMEM;
        }
        struct PreparedMapNode {
            decltype(coverage_pages_)* pages;
            decltype(coverage_pages_)::iterator version;
            bool inserted;
            bool armed = true;
            ~PreparedMapNode() {
                if (armed && inserted) pages->erase(version);
            }
        } prepared_node{&coverage_pages_, version, inserted};
#ifdef OBD_TEST_HOOKS
        if (fail_coverage_page_for_test_ &&
            *fail_coverage_page_for_test_ == page) {
            fail_coverage_page_for_test_.reset();
            co_return -EIO;
        }
#endif
        const uint64_t generation =
            inserted ? 1 : version->second.generation + 1;
        if (generation == 0) {
            co_return -EOVERFLOW;
        }
        const uint8_t slot =
            inserted ? 0 : static_cast<uint8_t>(1 - version->second.slot);
        const auto payload = coverage_payload_from_segments(exact_next, page);
        auto record = make_coverage_slot(page, generation, payload);
        const uint64_t offset = coverage_slot_offset(page, slot);
        const uint64_t empty_commit = 0;
        int rc = 0;
        try {
            rc = co_await pwrite_all_async(
                coverage_fd_, &empty_commit, sizeof(empty_commit),
                offset + kCoverageSlotCommitOffset);
        } catch (const std::bad_alloc&) {
            co_return -ENOMEM;
        }
        if (rc != 0) {
            co_return rc;
        }
        try {
            rc = co_await pwrite_all_async(coverage_fd_, record.data(),
                                          kCoverageSlotBodySize, offset);
        } catch (const std::bad_alloc&) {
            co_return -ENOMEM;
        }
        if (rc != 0) {
            co_return rc;
        }
        std::array<uint8_t, sizeof(uint64_t)> commit {};
        bytes::store_u64_le(commit.data(), kCoverageCommit);
        try {
            rc = co_await pwrite_all_async(
                coverage_fd_, commit.data(), commit.size(),
                offset + kCoverageSlotCommitOffset);
        } catch (const std::bad_alloc&) {
            co_return -ENOMEM;
        }
        if (rc != 0) {
            co_return rc;
        }
        prepared_node.armed = false;
        // Everything after the externally visible commit is noexcept. The
        // vector capacity and map node were prepared before the first write.
        {
            std::lock_guard lock(meta_mu_);
            segments_.swap(prepared_runtime);
            segments_generation_.fetch_add(1, std::memory_order_release);
        }
        version->second = CoveragePageVersion{generation, slot};
        *committed_hi = std::min(
            hi, (page + 1) * kCoverageSectorsPerPage);
        coverage_dirty_ = true;
        ++coverage_generation_;
    }
    co_return 0;
}

std::unique_ptr<SparseRwLayer> SparseRwLayer::open_sync(std::string path,
                                                        uint64_t vsize,
                                                        bool existing_only, bool use_extents) {
    if (!existing_only && (vsize == 0 || vsize % (use_extents ? 4096 : kSector) != 0)) {
        throw error(EINVAL, "sparse layer vsize has invalid alignment");
    }
    if (vsize > kMaxZeroMaskVsize) {
        throw error(EFBIG, "sparse layer vsize exceeds coverage limit");
    }

    bool created = false;
    int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT && !existing_only) {
        fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC,
                    0644);
        if (fd >= 0) {
            created = true;
        } else if (errno == EEXIST) {
            fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        }
    }
    if (fd < 0) throw_errno(errno, "cannot open sparse layer " + path);
    FdGuard opened(fd);
    // Lock before recovery can modify metadata or grow the data file. Every
    // project writer participates, including ordinary image assembly opens.
    // Separate opens through hardlinks contend on this same inode.
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int e = errno;
        throw_errno(e == EWOULDBLOCK ? EBUSY : e,
                    "sparse layer is already owned: " + path);
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int e = errno;
        throw_errno(e, "cannot stat sparse layer " + path);
    }
    if (!S_ISREG(st.st_mode)) {
        throw error(EINVAL, "sparse layer must be a regular file: " + path);
    }
    if (st.st_size < 0 || static_cast<uint64_t>(st.st_size) % kSector != 0) {
        throw format_error("sparse layer size is not sector aligned: " + path);
    }
    const uint64_t original_data_size = static_cast<uint64_t>(st.st_size);
    // Another opener may initialize and release the new inode between our
    // O_EXCL creation and flock. Its completed state is now an existing source;
    // do not remove its coverage as stale metadata from an older inode.
    if (original_data_size != 0) created = false;
    if (existing_only) {
        if (original_data_size == 0)
            throw error(EINVAL, "existing sparse layer has zero size: " + path);
        vsize = original_data_size;
    }
    if (use_extents) {
        // This is an unpublished format replacement. Do not silently interpret
        // old development coverage artifacts as the new extent-owned format.
        for (const auto& suffix : {".coverage", ".coverage.tmp", ".zeroes", ".zeroes.tmp"}) {
            struct stat legacy {};
            if (::lstat((path + suffix).c_str(), &legacy) == 0)
                throw error(EOPNOTSUPP, "recreate legacy Sparse development upper before opening: " + path);
            if (errno != ENOENT) throw_errno(errno, "cannot inspect Sparse upper artifacts");
        }
        validate_sparse_extent_backing(fd);
        const uint64_t full_size = std::max(vsize, original_data_size);
        if (!full_size || full_size % 4096)
            throw error(EINVAL, "extent Sparse size must be 4 KiB aligned");
        if (original_data_size != full_size &&
            ::ftruncate(fd, static_cast<off_t>(full_size)) != 0)
            throw_errno(errno, "cannot size extent Sparse upper");
        auto layer = std::unique_ptr<SparseRwLayer>(new SparseRwLayer);
        layer->vsize_.store(full_size, std::memory_order_release);
        layer->data_path_ = path;
        layer->fd_ = opened.release();
        layer->segments_ = scan_sparse_extents(layer->fd_, full_size);
        layer->extent_mode_ = true;
        layer->ro_ = std::make_unique<SparseRwLayer::View>(
            layer->fd_, layer->vsize_, "sparse-extents:" + path);
        return layer;
    }
    uint64_t effective_vsize = vsize;
    const std::string zero_mask_path = path + ".zeroes";
    effective_vsize = std::max(effective_vsize, original_data_size);
    if (!created) {
        const auto sidecar_vsize = read_zero_mask_vsize(zero_mask_path);
        if (sidecar_vsize) {
            if (*sidecar_vsize > effective_vsize) {
                throw format_error("sparse zero mask vsize mismatch: " +
                                   zero_mask_path);
            }
            effective_vsize = std::max(effective_vsize, *sidecar_vsize);
        }
    }
    if (effective_vsize > kMaxZeroMaskVsize) {
        throw error(EFBIG, "sparse layer vsize exceeds coverage limit");
    }
    auto layer = std::unique_ptr<SparseRwLayer>(new SparseRwLayer);
    layer->vsize_.store(effective_vsize, std::memory_order_release);
    layer->data_path_ = path;
    layer->zero_mask_path_ = zero_mask_path;
    layer->coverage_path_ = path + ".coverage";
    layer->fd_ = opened.release();

    if (created) {
        bool namespace_changed = false;
        for (const std::string& stale :
             {layer->zero_mask_path_, layer->coverage_path_,
              layer->coverage_path_ + ".tmp"}) {
            if (::unlink(stale.c_str()) == 0) {
                namespace_changed = true;
            } else if (errno != ENOENT) {
                const int e = errno;
                throw_errno(e, "cannot remove stale sparse metadata " + stale);
            }
        }
        if (namespace_changed) {
            const int drc = fsync_parent_dir(path);
            if (drc != 0) {
                throw_errno(drc, "cannot sync sparse metadata directory " +
                                     parent_dir_of(path));
            }
        }
    }

    auto exists = [](const std::string& candidate) {
        if (::access(candidate.c_str(), F_OK) == 0) return true;
        if (errno == ENOENT) return false;
        throw_errno(errno, "cannot inspect sparse metadata " + candidate);
    };
    const std::string coverage_tmp = layer->coverage_path_ + ".tmp";
    const bool has_coverage = exists(layer->coverage_path_);
    bool has_coverage_tmp = exists(coverage_tmp);
    if (has_coverage) {
        layer->load_coverage(layer->coverage_path_, original_data_size);
        // Keep accepting the v1 zero-mask artifact. It remains a protective
        // overlay during rolling upgrades and is also the migration source.
        layer->load_zero_masks();
        if (::ftruncate(layer->fd_, static_cast<off_t>(effective_vsize)) != 0) {
            throw_errno(errno, "cannot size sparse layer " + path);
        }
        if (::ftruncate(layer->coverage_fd_,
                        static_cast<off_t>(
                            coverage_file_size(effective_vsize))) != 0) {
            throw_errno(errno, "cannot grow sparse coverage " +
                                   layer->coverage_path_);
        }
        if (has_coverage_tmp) {
            if (::unlink(coverage_tmp.c_str()) != 0) {
                throw_errno(errno, "cannot remove obsolete sparse coverage " +
                                       coverage_tmp);
            }
            const int drc = fsync_parent_dir(coverage_tmp);
            if (drc != 0) {
                throw_errno(drc, "cannot sync sparse coverage directory");
            }
        }
    } else {
        std::array<uint8_t, 16> installed_identity {};
        const XattrIdentityStatus installed_status =
            read_xattr_identity(layer->fd_, &installed_identity);
        if (has_coverage_tmp &&
            installed_status != XattrIdentityStatus::kPresent) {
            struct stat temporary_st {};
            if (::stat(coverage_tmp.c_str(), &temporary_st) != 0) {
                throw_errno(errno, "cannot stat sparse coverage temporary");
            }
            const bool incomplete = temporary_st.st_size <
                    static_cast<off_t>(kCoverageHeaderSize) ||
                (read_coverage_binding(coverage_tmp).flags &
                    kCoverageFlagSnapshotComplete) == 0;
            if (incomplete) {
                // No marker was installed and the initial snapshot never
                // completed. Its absent pages are not evidence of INHERIT;
                // rebuild from the still-authoritative legacy data/zeroes.
                if (::unlink(coverage_tmp.c_str()) != 0) {
                    throw_errno(errno, "cannot restart sparse coverage import");
                }
                const int drc = fsync_parent_dir(coverage_tmp);
                if (drc != 0) throw_errno(drc, "cannot sync sparse coverage directory");
                has_coverage_tmp = false;
            }
        }
        if (has_coverage_tmp) {
            // A temporary artifact is recoverable only after complete format,
            // slot and EOF validation. Its name remains non-authoritative.
            layer->load_coverage(coverage_tmp, original_data_size,
                                  CoverageLoadMode::kInstalling);
            const CoverageHeaderBinding binding =
                read_coverage_binding(coverage_tmp);
            // A complete temporary might have survived process exit before
            // its creator's fsync. Persist it before making its marker durable.
            if (::fsync(layer->coverage_fd_) != 0) {
                throw_errno(errno, "cannot sync sparse coverage temporary");
            }
            if (::ftruncate(layer->fd_,
                            static_cast<off_t>(effective_vsize)) != 0) {
                throw_errno(errno, "cannot size sparse layer " + path);
            }
            if ((binding.flags & kCoverageFlagXattrIdentity) != 0) {
                if (installed_status == XattrIdentityStatus::kPresent) {
                    if (installed_identity != binding.identity) {
                        throw format_error(
                            "sparse coverage temporary identity mismatch: " +
                            coverage_tmp);
                    }
                } else if (installed_status == XattrIdentityStatus::kMissing) {
                    if (::fsetxattr(layer->fd_,
                                   kCoverageIdentityXattr.data(),
                                   binding.identity.data(),
                                   binding.identity.size(), XATTR_CREATE) != 0) {
                        throw_errno(errno,
                                    "cannot resume sparse coverage identity");
                    }
                } else {
                    throw format_error(
                        "sparse coverage temporary requires xattrs: " +
                        coverage_tmp);
                }
            } else if (installed_status == XattrIdentityStatus::kPresent) {
                throw format_error(
                    "sparse coverage fallback conflicts with data identity: " +
                    coverage_tmp);
            }
            if (::fsync(layer->fd_) != 0) {
                throw_errno(errno, "cannot sync sparse coverage identity");
            }
            if (::rename(coverage_tmp.c_str(),
                         layer->coverage_path_.c_str()) != 0) {
                throw_errno(errno, "cannot resume sparse coverage install");
            }
            const int drc = fsync_parent_dir(layer->coverage_path_);
            if (drc != 0) {
                throw_errno(drc, "cannot sync sparse coverage directory");
            }
            layer->load_zero_masks();
            if (::ftruncate(layer->coverage_fd_,
                            static_cast<off_t>(
                                coverage_file_size(effective_vsize))) != 0) {
                throw_errno(errno, "cannot grow sparse coverage " +
                                       layer->coverage_path_);
            }
        } else {
            if (installed_status == XattrIdentityStatus::kPresent) {
                throw format_error(
                    "sparse coverage identity exists but coverage is missing: " +
                    layer->coverage_path_);
            }
            // One-time legacy import. SEEK_DATA/SEEK_HOLE may overstate
            // ownership at filesystem-block granularity; freezing that view
            // is the only available migration for a v1 upper. Scan only its
            // original EOF; expansion is ordered after the complete snapshot.
            uint64_t pos = 0;
            while (!created && pos < original_data_size) {
                const off_t data =
                    ::lseek(fd, static_cast<off_t>(pos), SEEK_DATA);
                if (data < 0) {
                    if (errno == ENXIO) break;
                    throw_errno(errno, "SEEK_DATA failed on " + path);
                }
                const off_t hole = ::lseek(fd, data, SEEK_HOLE);
                if (hole < 0) {
                    throw_errno(errno, "SEEK_HOLE failed on " + path);
                }
                const uint64_t d = static_cast<uint64_t>(data);
                const uint64_t h =
                    std::min(static_cast<uint64_t>(hole), original_data_size);
                const uint64_t so = d / kSector;
                const uint64_t se = (h + kSector - 1) / kSector;
                if (se > so) layer->insert_live_extent(so, se - so);
                pos = h;
            }
            layer->load_zero_masks();
            layer->create_coverage_snapshot(!created);
        }
    }
    layer->ro_ = std::make_unique<SparseRwLayer::View>(
        fd, layer->vsize_, "sparse-rw:" + path);
    return layer;
}

#ifdef OBD_TEST_HOOKS
elio::coro::task<std::unique_ptr<SparseRwLayer>> SparseRwLayer::open_legacy_for_test(
    const std::string& path, uint64_t vsize) {
#ifdef OBD_TEST_HOOKS
    auto park = open_park_for_test_;
#endif
    auto job = [path = std::string(path), vsize
#ifdef OBD_TEST_HOOKS
                , park = std::move(park)
#endif
               ] {
#ifdef OBD_TEST_HOOKS
        if (park) {
            park->store(1, std::memory_order_release);
            park->notify_all();
            while (park->load(std::memory_order_acquire) == 1) {
                park->wait(1, std::memory_order_acquire);
            }
        }
#endif
        auto layer = SparseRwLayer::open_sync(path, vsize);
#ifdef OBD_TEST_HOOKS
        if (park) {
            park->store(3, std::memory_order_release);
            park->notify_all();
        }
#endif
        return layer;
    };
    using Job = decltype(job);
    auto layer = co_await run_owned_blocking(new Job(std::move(job)));
    co_return std::move(layer);
}

#endif

elio::coro::task<std::unique_ptr<SparseRwLayer>> SparseRwLayer::open_existing(
    std::string path) {
    auto job = [path = std::move(path)] {
        bool legacy = false;
        for (const auto& suffix : {".coverage", ".coverage.tmp", ".zeroes", ".zeroes.tmp"}) {
            struct stat st {};
            if (::lstat((path + suffix).c_str(), &st) == 0) legacy = true;
            else if (errno != ENOENT) throw_errno(errno, "cannot inspect Sparse export artifacts");
        }
        return SparseRwLayer::open_sync(path, 0, true, !legacy);
    };
    using Job = decltype(job);
    auto layer = co_await run_owned_blocking(new Job(std::move(job)));
    co_return std::move(layer);
}

elio::coro::task<SparseExportResult> SparseRwLayer::export_file(
    std::string source_path, std::string destination,
    std::string parent_uuid, std::string user_tag, uint64_t virtual_size) {
    if (source_path.find('\0') != std::string::npos || destination.find('\0') != std::string::npos)
        throw error(EINVAL, "sparse export: invalid pathname");
    auto source = co_await open_existing(source_path);
    if (virtual_size == 0) virtual_size = source->virtual_size();
    if (virtual_size < source->virtual_size() ||
        virtual_size % (source->extent_mode_ ? 4096 : 512) != 0 ||
        virtual_size / 512 > bytes::segment_mapping::kMaxOffset)
        throw error(EINVAL, "sparse export: virtual size override must be aligned and grow-only");
    auto prepare = [&] {
        return sparse_export_detail::Output::prepare(
            source_path, source->fd_, source->coverage_fd_, destination);
    };
    using Prepare = decltype(prepare);
    auto output = co_await run_owned_blocking(new Prepare(std::move(prepare)));
    SparseExportResult result;
    std::exception_ptr failure;
    try {
        const int checkpointed = co_await source->checkpoint();
        if (checkpointed != 0) throw error(-checkpointed, "sparse export: checkpoint source");
        const auto ownership = source->segments_snapshot();
        auto encoded = co_await sparse_export_detail::write_delta(source->data_source(),
            output->fd(), virtual_size, ownership, std::move(parent_uuid), std::move(user_tag));
        result.uuid = std::move(encoded.uuid);
        result.size = encoded.file_bytes;
        common::Sha256 digest;
        std::vector<std::byte> buffer(1 << 20), existing(buffer.size());
        for (uint64_t offset = 0; offset < result.size;) {
            const auto count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), result.size - offset));
            const auto read = co_await elio::io::pread_exactly(output->fd(),
                std::span(buffer).first(count), offset);
            if (read.error) throw error(read.error.value(), "sparse export: digest read");
            if (read.end != elio::io::transfer_end::complete)
                throw error(EIO, "sparse export: incomplete digest read");
            digest.update(buffer.data(), count);
            offset += count;
        }
        result.sha256 = digest.final_hex();
        auto publish = [active = output.get()] { return active->publish(); };
        using Publish = decltype(publish);
        const bool published = co_await run_owned_blocking(new Publish(std::move(publish)));
        if (!published) {
            for (uint64_t offset = 0; offset < result.size;) {
                const auto count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), result.size - offset));
                const auto staged = co_await elio::io::pread_exactly(output->fd(),
                    std::span(buffer).first(count), offset);
                const auto prior = co_await elio::io::pread_exactly(output->existing_fd(),
                    std::span(existing).first(count), offset);
                if (staged.error || prior.error || staged.end != elio::io::transfer_end::complete ||
                    prior.end != elio::io::transfer_end::complete)
                    throw error(EIO, "sparse export: cannot verify existing output");
                if (!std::equal(buffer.begin(), buffer.begin() + count, existing.begin()))
                    throw error(EEXIST, "sparse export: different output already exists");
                offset += count;
            }
            const auto synced = co_await elio::io::sync_file(output->existing_fd(),
                elio::io::file_sync_mode::data_and_metadata);
            if (!synced) throw error(synced.error_value(), "sparse export: sync existing output");
        }
        auto sync_directory = [active = output.get()] { active->sync_directory(); return 0; };
        using Sync = decltype(sync_directory);
        co_await run_owned_blocking(new Sync(std::move(sync_directory)));
    } catch (...) { failure = std::current_exception(); }
    // Cleanup normally runs off workers too. A pre-publication failure removes
    // only our private staging name; a published output is never removed.
    auto cleanup = [owned = std::move(output)]() mutable { owned.reset(); return 0; };
    using Cleanup = decltype(cleanup);
    co_await run_owned_blocking(new Cleanup(std::move(cleanup)));
    if (failure) std::rethrow_exception(failure);
    co_return result;
}

SparseRwLayer::~SparseRwLayer() {
    // Elio can defer the actual close on io_uring. End exclusive ownership now,
    // after the caller has drained operations, so a subsequent open need not
    // wait for an unrelated worker's close submission to reach the kernel.
    if (fd_ >= 0) {
        while (::flock(fd_, LOCK_UN) != 0 && errno == EINTR) {}
    }
    if (coverage_fd_ >= 0) elio::io::close_fd_for_destructor(coverage_fd_);
    if (fd_ >= 0) elio::io::close_fd_for_destructor(fd_);
}

std::vector<bytes::segment_mapping> SparseRwLayer::segments_snapshot() const {
    std::lock_guard lock(meta_mu_);
    if (!extent_view_valid_) throw error(EIO, "Sparse extent view requires repair");
    return segments_;
}

source::BlobSource& SparseRwLayer::data_source() {
    return *ro_;
}

int SparseRwLayer::begin_data_op(uint64_t offset, uint64_t len) {
    std::lock_guard lock(state_gate_mu_);
    const Lifecycle state = lifecycle_.load(std::memory_order_acquire);
    if (state != Lifecycle::kOpen) {
        return state == Lifecycle::kCheckpointed ? -EROFS : -EBUSY;
    }
    const uint64_t cur_vsize = vsize_.load(std::memory_order_acquire);
    if (offset > cur_vsize || len > cur_vsize - offset) return -EINVAL;
    active_ops_.fetch_add(1, std::memory_order_acq_rel);
    return 0;
}

int SparseRwLayer::begin_flush_op(bool* already_checkpointed) {
    *already_checkpointed = false;
    std::lock_guard lock(state_gate_mu_);
    const Lifecycle state = lifecycle_.load(std::memory_order_acquire);
    if (state == Lifecycle::kCheckpointed) {
        *already_checkpointed = true;
        return 0;
    }
    if (state != Lifecycle::kOpen) return -EBUSY;
    active_ops_.fetch_add(1, std::memory_order_acq_rel);
    return 0;
}

int SparseRwLayer::begin_grow_op() {
    std::lock_guard lock(state_gate_mu_);
    const Lifecycle state = lifecycle_.load(std::memory_order_acquire);
    if (state != Lifecycle::kOpen) {
        return state == Lifecycle::kCheckpointed ? -EROFS : -EBUSY;
    }
    if (active_ops_.load(std::memory_order_acquire) != 0) return -EBUSY;
    lifecycle_.store(Lifecycle::kGrowing, std::memory_order_release);
    return 0;
}

int SparseRwLayer::begin_checkpoint_op() {
    std::lock_guard lock(state_gate_mu_);
    const Lifecycle state = lifecycle_.load(std::memory_order_acquire);
    if (state != Lifecycle::kOpen) {
        return state == Lifecycle::kCheckpointed ? -EROFS : -EBUSY;
    }
    if (active_ops_.load(std::memory_order_acquire) != 0) return -EBUSY;
    lifecycle_.store(Lifecycle::kCheckpointing, std::memory_order_release);
    return 0;
}

void SparseRwLayer::end_data_op() {
    active_ops_.fetch_sub(1, std::memory_order_acq_rel);
}

void SparseRwLayer::end_grow_op() {
    std::lock_guard lock(state_gate_mu_);
    lifecycle_.store(Lifecycle::kOpen, std::memory_order_release);
}

void SparseRwLayer::end_checkpoint_op(Lifecycle next) {
    std::lock_guard lock(state_gate_mu_);
    lifecycle_.store(next, std::memory_order_release);
}

bool SparseRwLayer::dirty_zero_masks_need_data_first() const {
    std::lock_guard lock(meta_mu_);
    return zero_masks_dirty_ && zero_masks_data_first_;
}

bool SparseRwLayer::has_zero_masks_locked() const {
    return std::any_of(segments_.begin(), segments_.end(),
                       [](const auto& s) { return s.zeroed; });
}

bool SparseRwLayer::range_intersects_zero_mask_locked(uint64_t lo,
                                                      uint64_t hi) const {
    return std::any_of(segments_.begin(), segments_.end(),
                       [lo, hi](const auto& s) {
                           return s.zeroed && s.offset < hi && s.end() > lo;
                       });
}

std::vector<bytes::segment_mapping>
SparseRwLayer::zero_mask_segments_locked() const {
    return zero_mask_segments_from(segments_);
}

void SparseRwLayer::erase_range(uint64_t lo, uint64_t hi) {
    erase_range_from(segments_, lo, hi);
}

void SparseRwLayer::insert_live_extent(uint64_t off, uint64_t len) {
    insert_live_extent_into(segments_, off, len);
}

ZeroMaskPersistResult persist_zero_masks_to_path(
    const std::string& zero_mask_path,
    const std::vector<bytes::segment_mapping>& zeroes,
    uint64_t vsize
#ifdef OBD_TEST_HOOKS
    , bool fail_dirsync_after_commit = false
#endif
    ) {
    try {
        if (zeroes.size() > lsmt::kMaxRoIndexSize) return {-EFBIG, false};
        if (!zeroes.empty() && vsize > kMaxZeroMaskVsize) {
            return {-EFBIG, false};
        }

        // Finish every allocation before unlink/rename changes the recovered
        // namespace. Once that commit happens, this routine must be able to
        // report namespace_committed even under memory pressure.
        const std::string parent_dir = parent_dir_of(zero_mask_path);
        if (zeroes.empty()) {
            bool namespace_committed = false;
            if (::unlink(zero_mask_path.c_str()) == 0) {
                namespace_committed = true;
#ifdef OBD_TEST_HOOKS
                if (fail_dirsync_after_commit) return {-EIO, true};
#endif
                const int drc = fsync_dir(parent_dir);
                if (drc != 0) return {-drc, namespace_committed};
            } else if (errno != ENOENT) {
                return {-errno, namespace_committed};
            } else {
                namespace_committed = true;
#ifdef OBD_TEST_HOOKS
                if (fail_dirsync_after_commit) return {-EIO, true};
#endif
                const int drc = fsync_dir(parent_dir);
                if (drc != 0) return {-drc, namespace_committed};
            }
            return {0, namespace_committed};
        }

        const std::string tmp = zero_mask_path + ".tmp";
        std::vector<uint8_t> raw(
            kZeroMaskHeaderSize +
                zeroes.size() * bytes::segment_mapping::kEncodedSize,
            0);
        std::memcpy(raw.data(), kZeroMaskMagic, sizeof(kZeroMaskMagic));
        bytes::store_u64_le(raw.data() + 8, vsize);
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

        const int fd = ::open(tmp.c_str(),
                              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) return {-errno, false};
        FdGuard guard(fd, &tmp);
        int rc = pwrite_all_sync(fd, raw.data(), raw.size(), 0);
        if (rc == 0 && ::fsync(fd) != 0) rc = errno;
        const int close_rc = guard.close();
        if (close_rc != 0 && rc == 0) rc = close_rc;
        if (rc != 0) return {-rc, false};
        if (::rename(tmp.c_str(), zero_mask_path.c_str()) != 0) {
            return {-errno, false};
        }
        guard.dismiss_unlink();
        const bool namespace_committed = true;
#ifdef OBD_TEST_HOOKS
        if (fail_dirsync_after_commit) return {-EIO, true};
#endif
        const int drc = fsync_dir(parent_dir);
        if (drc != 0) return {-drc, namespace_committed};
        return {0, namespace_committed};
    } catch (const std::bad_alloc&) {
        return {-ENOMEM, false};
    } catch (const std::length_error&) {
        return {-EFBIG, false};
    }
}

struct ZeroMaskPersistJob {
    std::string path;
    std::vector<bytes::segment_mapping> zeroes;
    uint64_t vsize = 0;
    bool fail_dirsync_after_commit = false;

    ZeroMaskPersistResult operator()() const {
        return persist_zero_masks_to_path(path, zeroes, vsize
#ifdef OBD_TEST_HOOKS
                                          , fail_dirsync_after_commit
#endif
                                          );
    }
};

template <typename Zeroes>
ZeroMaskPersistJob* make_zero_mask_persist_job(
    const std::string& path, Zeroes&& zeroes, uint64_t vsize,
    bool fail_dirsync_after_commit, int* error) noexcept {
    void* storage = ::operator new(sizeof(ZeroMaskPersistJob), std::nothrow);
    if (storage == nullptr) {
        *error = -ENOMEM;
        return nullptr;
    }
    try {
        return ::new (storage) ZeroMaskPersistJob{
            path, std::forward<Zeroes>(zeroes), vsize,
            fail_dirsync_after_commit};
    } catch (const std::bad_alloc&) {
        ::operator delete(storage);
        *error = -ENOMEM;
        return nullptr;
    } catch (const std::length_error&) {
        ::operator delete(storage);
        *error = -EFBIG;
        return nullptr;
    }
}

ZeroMaskPersistResult SparseRwLayer::persist_zero_masks(
    const std::vector<bytes::segment_mapping>& zeroes,
    uint64_t vsize) {
    // A same-generation replacement can rename successfully and then fail
    // directory synchronization. Its previous success must not suppress retry.
    {
        std::lock_guard lock(meta_mu_);
        published_zero_masks_generation_.reset();
    }
    return persist_zero_masks_to_path(zero_mask_path_, zeroes, vsize
#ifdef OBD_TEST_HOOKS
        , std::exchange(fail_zero_mask_dirsync_after_commit_for_test_, false)
#endif
    );
}

int SparseRwLayer::sync_data_file() const {
    if (::fdatasync(fd_) != 0) return -errno;
    return 0;
}

elio::coro::task<int> SparseRwLayer::publish_data_first_zero_masks_locked(
    std::vector<bytes::segment_mapping> exact_next,
    std::vector<bytes::segment_mapping> next_zeroes) {
    const uint64_t publish_vsize = vsize_.load(std::memory_order_acquire);
#ifdef OBD_TEST_HOOKS
    const bool fail_dirsync = fail_zero_mask_dirsync_after_commit_for_test_;
    fail_zero_mask_dirsync_after_commit_for_test_ = false;
#else
    constexpr bool fail_dirsync = false;
#endif

    // Exact coverage already says DATA while the compatibility sidecar and
    // live view still say ZERO. Record the complete desired transition before
    // any further fallible operation so sync, allocation, or pool-admission
    // failure remains retryable by flush/grow and blocks unrelated writes.
    uint64_t generation = 0;
    {
        std::lock_guard lock(meta_mu_);
        pending_zero_masks_.swap(next_zeroes);
        pending_data_segments_.swap(exact_next);
        zero_masks_pending_view_ = true;
        zero_masks_dirty_ = true;
        zero_masks_data_first_ = true;
        generation = ++zero_masks_generation_;
    }

    const auto status = co_await elio::io::sync_file(
        fd_, elio::io::file_sync_mode::data_only);
    if (!status) co_return -status.error_value();

    int prepare_error = 0;
    ZeroMaskPersistJob* job = nullptr;
    {
        std::lock_guard lock(meta_mu_);
        job = make_zero_mask_persist_job(
            zero_mask_path_, pending_zero_masks_, publish_vsize, fail_dirsync,
            &prepare_error);
    }
    if (job == nullptr) co_return prepare_error;

    ZeroMaskPersistResult result;
    try {
        result = co_await run_owned_blocking(job);
    } catch (const std::bad_alloc&) {
        co_return -ENOMEM;
    }
    {
        std::lock_guard lock(meta_mu_);
        if (result.namespace_committed) {
            // rename/unlink already changed the view that a concurrent crash
            // would recover. This swap cannot allocate or throw.
            if (zero_masks_generation_ == generation &&
                zero_masks_pending_view_) {
                segments_.swap(pending_data_segments_);
                segments_generation_.fetch_add(1, std::memory_order_release);
                pending_zero_masks_.clear();
                pending_data_segments_.clear();
                zero_masks_pending_view_ = false;
            }
        }
        if (result.rc == 0 && zero_masks_generation_ == generation) {
            zero_masks_dirty_ = false;
            zero_masks_data_first_ = false;
            pending_zero_masks_.clear();
            pending_data_segments_.clear();
            zero_masks_pending_view_ = false;
        }
    }
    co_return result.rc;
}

void SparseRwLayer::load_zero_masks() {
    const int fd = ::open(zero_mask_path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return;
        throw_errno(errno, "cannot open sparse zero mask " + zero_mask_path_);
    }
    FdGuard guard(fd);

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int e = errno;
        throw_errno(e, "cannot stat sparse zero mask " + zero_mask_path_);
    }
    if (st.st_size < static_cast<off_t>(kZeroMaskHeaderSize)) {
        throw format_error("sparse zero mask is truncated: " + zero_mask_path_);
    }

    std::array<uint8_t, kZeroMaskHeaderSize> hdr {};
    int rc = pread_all_sync(fd, hdr.data(), hdr.size(), 0);
    if (rc != 0) {
        throw_errno(rc, "cannot read sparse zero mask " + zero_mask_path_);
    }
    if (std::memcmp(hdr.data(), kZeroMaskMagic, sizeof(kZeroMaskMagic)) != 0) {
        throw format_error("sparse zero mask has bad magic: " + zero_mask_path_);
    }

    const uint64_t stored_vsize = bytes::load_u64_le(hdr.data() + 8);
    const uint64_t cur_vsize = vsize_.load(std::memory_order_acquire);
    if (stored_vsize == 0 || stored_vsize % kSector != 0 ||
        stored_vsize > kMaxZeroMaskVsize ||
        stored_vsize > cur_vsize) {
        throw format_error("sparse zero mask vsize mismatch: " +
                           zero_mask_path_);
    }
    if (stored_vsize != cur_vsize) zero_masks_dirty_ = true;

    const uint64_t count = bytes::load_u64_le(hdr.data() + 16);
    if (count > lsmt::kMaxRoIndexSize) {
        throw format_error("sparse zero mask size exceeds maximum: " +
                           zero_mask_path_);
    }
    const uint64_t max_count =
        (std::numeric_limits<uint64_t>::max() - kZeroMaskHeaderSize) /
        bytes::segment_mapping::kEncodedSize;
    if (count > max_count) {
        throw format_error("sparse zero mask size mismatch: " + zero_mask_path_);
    }
    const uint64_t entry_bytes =
        count * bytes::segment_mapping::kEncodedSize;
    if (static_cast<uint64_t>(st.st_size) != kZeroMaskHeaderSize + entry_bytes) {
        throw format_error("sparse zero mask size mismatch: " + zero_mask_path_);
    }

    std::vector<uint8_t> raw(entry_bytes);
    if (entry_bytes != 0) {
        rc = pread_all_sync(fd, raw.data(), raw.size(), kZeroMaskHeaderSize);
        if (rc != 0) {
            throw_errno(rc, "cannot read sparse zero mask entries " +
                                zero_mask_path_);
        }
    }
    (void)guard.close();

    const uint64_t stored_max_sector = stored_vsize / kSector;
    const uint64_t cur_max_sector = cur_vsize / kSector;
    std::vector<bytes::segment_mapping> zeroes;
    zeroes.reserve(static_cast<size_t>(count));
    uint64_t prev_end = 0;
    for (uint64_t i = 0; i < count; ++i) {
        auto z = bytes::load_segment_le(
            raw.data() + i * bytes::segment_mapping::kEncodedSize);
        if (!z.zeroed || z.length == 0 ||
            z.offset == bytes::segment_mapping::kInvalidOffset ||
            z.offset < prev_end || z.offset > stored_max_sector ||
            z.length > stored_max_sector - z.offset) {
            throw format_error("sparse zero mask has invalid segment: " +
                               zero_mask_path_);
        }
        const uint64_t z_end = z.offset + z.length;
        if (z.offset < cur_max_sector) {
            const uint64_t clipped_end = std::min(z_end, cur_max_sector);
            z.length = static_cast<uint32_t>(clipped_end - z.offset);
            z.moffset = 0;
            z.zeroed = true;
            z.tag = 0;
            zeroes.push_back(z);
        }
        prev_end = z_end;
    }
    segments_ = overlay_zero_masks(std::move(segments_), zeroes);
}

int SparseRwLayer::grow(uint64_t vsize) {
    // D3 grow-only vsize extension. BLOCKING (ftruncate): callers must
    // run this off an Elio worker via elio::spawn_blocking — the device
    // resize executor does. Equal is an idempotent no-op (retried grows);
    // smaller is a shrink and is rejected.
    const int gate = begin_grow_op();
    if (gate != 0) return gate;
    struct GrowResetGuard {
        SparseRwLayer* self;
        bool armed = true;
        ~GrowResetGuard() {
            if (armed) self->end_grow_op();
        }
    } reset{this};
    if (extent_mode_) {
        const uint64_t cur = virtual_size();
        if (!vsize || vsize % 4096 || vsize < cur) return -EINVAL;
        if (vsize > kMaxZeroMaskVsize) return -EFBIG;
        if (vsize == cur) return 0;
        if (::ftruncate(fd_, static_cast<off_t>(vsize)) != 0) return -errno;
        vsize_.store(vsize, std::memory_order_release);
        return 0;
    }
#ifndef OBD_TEST_HOOKS
    return -EROFS;
#endif
    int rc = 0;
    if (vsize == 0 || vsize % kSector != 0) rc = -EINVAL;
    const uint64_t cur = vsize_.load(std::memory_order_acquire);
    if (rc == 0 && vsize < cur) {
        rc = -EINVAL;  // grow-only (equal = no-op)
    } else if (rc == 0 && vsize > cur) {
        std::vector<bytes::segment_mapping> zeroes;
        uint64_t generation = 0;
        bool dirty = false;
        bool data_first = false;
        bool pending_view = false;
        try {
            std::lock_guard lock(meta_mu_);
            dirty = zero_masks_dirty_;
            data_first = zero_masks_data_first_;
            pending_view = zero_masks_pending_view_;
            if (dirty) {
                zeroes = pending_view ? pending_zero_masks_
                                      : zero_mask_segments_locked();
                generation = zero_masks_generation_;
            }
        } catch (const std::bad_alloc&) {
            return -ENOMEM;
        }
        if (vsize > kMaxZeroMaskVsize) {
            rc = -EFBIG;
        }
        if (rc == 0 && dirty) {
            if (data_first) rc = sync_data_file();
            ZeroMaskPersistResult persisted;
            if (rc == 0) {
                persisted = persist_zero_masks(zeroes, cur);
                rc = persisted.rc;
            }
            if (persisted.namespace_committed && pending_view) {
                std::lock_guard lock(meta_mu_);
                if (zero_masks_generation_ == generation &&
                    zero_masks_pending_view_) {
                    segments_.swap(pending_data_segments_);
                    segments_generation_.fetch_add(1,
                                                   std::memory_order_release);
                    pending_zero_masks_.clear();
                    pending_data_segments_.clear();
                    zero_masks_pending_view_ = false;
                }
            }
            if (rc == 0 && !data_first) rc = sync_data_file();
            if (rc == 0) {
                std::lock_guard lock(meta_mu_);
                if (zero_masks_generation_ == generation) {
                    zero_masks_dirty_ = false;
                    zero_masks_data_first_ = false;
                    pending_zero_masks_.clear();
                    pending_data_segments_.clear();
                    zero_masks_pending_view_ = false;
                }
            }
        }
    }
    if (rc == 0 && vsize > cur) {
        if (::ftruncate(fd_, static_cast<off_t>(vsize)) != 0) {
            rc = -errno;
        } else if (::fdatasync(fd_) != 0) {
            const int sync_errno = errno;
            if (::ftruncate(fd_, static_cast<off_t>(cur)) != 0) {
                rc = -errno;
            } else {
                (void)::fdatasync(fd_);
                rc = -sync_errno;
            }
        }
    }
    if (rc == 0 && vsize > cur &&
        ::ftruncate(coverage_fd_,
                    static_cast<off_t>(coverage_file_size(vsize))) != 0) {
        const int coverage_errno = errno;
        if (::ftruncate(fd_, static_cast<off_t>(cur)) != 0) {
            rc = -errno;
        } else {
            (void)::fdatasync(fd_);
            rc = -coverage_errno;
        }
    }
    if (rc == 0 && vsize > cur) {
        vsize_.store(vsize, std::memory_order_release);
        std::lock_guard lock(meta_mu_);
        if (has_zero_masks_locked() || zero_masks_dirty_) {
            zero_masks_dirty_ = true;
            zero_masks_data_first_ = false;
            ++zero_masks_generation_;
        }
    }
    reset.armed = false;
    end_grow_op();
    return rc;
}

elio::coro::task<ssize_t> SparseRwLayer::pwrite(const void* buf, size_t count,
                                                uint64_t offset) {
    if (extent_mode_) co_return co_await pwrite_with_view(buf, count, offset, *ro_);
#ifndef OBD_TEST_HOOKS
    co_return -EROFS;
#else
    const int gate = begin_data_op(offset, count);
    if (gate != 0) co_return gate;
    ActiveOpGuard active(active_ops_, AdoptActiveOp{});
    if (offset % kSector != 0 || count % kSector != 0 || count == 0) {
        co_return -EINVAL;
    }
    try {
        co_await op_mu_.lock();
        SyncMutexGuard op_guard(op_mu_);
        const uint64_t request_lo = offset / kSector;
        const uint64_t request_hi = request_lo + count / kSector;
        bool flush_before_write = false;
        {
            std::lock_guard lock(meta_mu_);
            flush_before_write =
                zero_masks_dirty_ &&
                (zero_masks_data_first_ ||
                 range_intersects_zero_mask_locked(request_lo, request_hi));
        }
        if (flush_before_write) {
            const int rc = co_await flush_locked();
            if (rc != 0) co_return rc;
        }
        bool already_data = false;
        {
            std::lock_guard lock(meta_mu_);
            already_data = range_is_data(segments_, request_lo, request_hi);
        }
        // op_mu_ keeps ownership stable through the backing write. Rewriting
        // existing DATA needs no coverage publication or mapping generation bump;
        // pending ZERO transitions were settled above and FLUSH still syncs data.
        const uint8_t* p = static_cast<const uint8_t*>(buf);
        size_t done = 0;
        int write_error = 0;
        while (done < count && write_error == 0) {
            const size_t piece = std::min(
                count - done,
                static_cast<size_t>(bytes::segment_mapping::kMaxLength) * kSector);
            // elio io backend: positional write, never blocks the worker.
            // Publish full-sector prefixes as they land so an error after a
            // positive short write cannot leave the sparse index behind the file.
            size_t written = 0;
            size_t indexed = 0;
            while (written < piece) {
                const auto result = co_await elio::io::pwrite_some(
                    fd_, std::as_bytes(std::span(p + done + written, piece - written)),
                    offset + done + written);
                if (result.error) {
                    write_error = -result.error.value();
                    break;
                }
                if (result.transferred == 0) {
                    write_error = -EIO;
                    break;
                }
                written += result.transferred;
                if (written > piece) {
                    write_error = -EIO;
                    break;
                }

                const size_t indexable = (written / kSector) * kSector;
                if (indexable > indexed && !already_data) {
#ifdef OBD_TEST_HOOKS
                    if (fail_pwrite_prepare_after_bytes_for_test_ &&
                        done >= *fail_pwrite_prepare_after_bytes_for_test_) {
                        fail_pwrite_prepare_after_bytes_for_test_.reset();
                        throw std::bad_alloc();
                    }
#endif
                    const uint64_t lo = (offset + done + indexed) / kSector;
                    const uint64_t hi = (offset + done + indexable) / kSector;
                    std::vector<bytes::segment_mapping> next;
                    std::vector<bytes::segment_mapping> runtime_next;
                    std::vector<bytes::segment_mapping> next_zeroes;
                    bool erased_zero = false;
                    {
                        std::lock_guard lock(meta_mu_);
                        erased_zero = range_intersects_zero_mask_locked(lo, hi);
                        next = segments_;
                    }
                    erase_range_from(next, lo, hi);
                    insert_live_extent_into(next, lo, hi - lo);
                    runtime_next = next;
                    if (erased_zero) {
                        std::vector<bytes::segment_mapping> old_zeroes;
                        {
                            std::lock_guard lock(meta_mu_);
                            old_zeroes = zero_mask_segments_locked();
                        }
                        runtime_next = overlay_zero_masks(std::move(runtime_next),
                                                          old_zeroes);
                        next_zeroes = zero_mask_segments_from(next);
                    }
                    uint64_t committed_hi = lo;
                    const int coverage_rc = co_await publish_coverage_locked(
                        next, runtime_next, lo, hi, &committed_hi);
                    if (coverage_rc != 0) {
                        write_error = coverage_rc;
                        break;
                    }
                    if (erased_zero) {
                        const int rc = co_await publish_data_first_zero_masks_locked(
                            std::move(next), std::move(next_zeroes));
                        if (rc != 0) {
                            write_error = rc;
                            break;
                        }
                    }
                    indexed = indexable;
                }
            }
            done += piece;
        }
        if (write_error != 0) co_return write_error;
        co_return static_cast<ssize_t>(count);
    } catch (const std::bad_alloc&) {
        co_return -ENOMEM;
    } catch (const std::length_error&) {
        co_return -EFBIG;
    }
#endif
}

elio::coro::task<ssize_t> SparseRwLayer::pread(void* buf, size_t count,
                                               uint64_t offset) {
    {
        std::lock_guard lock(meta_mu_);
        if (!extent_view_valid_) co_return -EIO;
    }
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
        bytes::segment_mapping hit {};
        bool has_hit = false;
        uint64_t covered_end = pos;
        uint64_t next = offset + count;
        {
            std::lock_guard lock(meta_mu_);
            for (const auto& s : segments_) {
                if (s.offset * kSector > pos) {
                    next = std::min(next, s.offset * kSector);
                    break;
                }
                if (s.end() * kSector > pos) {
                    hit = s;
                    has_hit = true;
                    covered_end = s.end() * kSector;
                    break;
                }
            }
        }
        if (has_hit) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(
                covered_end - pos, count - done));
            if (hit.zeroed) {
                std::memset(out + done, 0, n);
            } else {
                const auto result = co_await elio::io::pread_exactly(
                    fd_, std::as_writable_bytes(std::span(out + done, n)), pos);
                if (result.error) co_return -result.error.value();
                if (result.end != elio::io::transfer_end::complete) co_return -EIO;
            }
            done += n;
        } else {
            // Zero-fill up to the next extent or the request end.
            const size_t n = static_cast<size_t>(next - pos);
            std::memset(out + done, 0, n);
            done += n;
        }
    }
    co_return static_cast<ssize_t>(done);
}

elio::coro::task<int> SparseRwLayer::discard(uint64_t offset, uint64_t len) {
    if (extent_mode_) co_return co_await discard_with_view(offset, len, *ro_);
#ifndef OBD_TEST_HOOKS
    co_return -EROFS;
#else
    const int gate = begin_data_op(offset, len);
    if (gate != 0) co_return gate;
    ActiveOpGuard active(active_ops_, AdoptActiveOp{});
    if (offset % kSector != 0 || len % kSector != 0 || len == 0) {
        co_return -EINVAL;
    }
    try {
        co_await op_mu_.lock();
        SyncMutexGuard op_guard(op_mu_);
        if (dirty_zero_masks_need_data_first()) {
            const int rc = co_await flush_locked();
            if (rc != 0) co_return rc;
        }
        const uint64_t lo = offset / kSector;
        const uint64_t hi = lo + len / kSector;
        const uint64_t cur_vsize = vsize_.load(std::memory_order_acquire);

        // Publish exact ZERO coverage before the compatibility zero-mask and
        // before punching the sparse file. A process crash can therefore expose
        // either the preceding committed coverage page or the complete ZERO
        // transition, never filesystem-allocation guesses.
        std::vector<bytes::segment_mapping> next_segments;
        std::vector<bytes::segment_mapping> next_zeroes;
        {
            std::lock_guard lock(meta_mu_);
            std::vector<bytes::segment_mapping> added_zero;
            push_zero_range(added_zero, lo, hi);
            next_segments = overlay_zero_masks(segments_, added_zero);
            next_zeroes = zero_mask_segments_from(next_segments);
        }
        struct ZeroMaskSnapshot {
            std::vector<bytes::segment_mapping> zeroes;
            uint64_t vsize;
        } snapshot{next_zeroes, cur_vsize};
        uint64_t committed_hi = lo;
        const int coverage_rc = co_await publish_coverage_locked(
            next_segments, next_segments, lo, hi, &committed_hi);
        if (coverage_rc != 0) {
            if (committed_hi > lo) {
                std::lock_guard lock(meta_mu_);
                zero_masks_dirty_ = true;
                zero_masks_data_first_ = false;
                ++zero_masks_generation_;
            }
            co_return coverage_rc;
        }
        {
            std::lock_guard lock(meta_mu_);
            zero_masks_dirty_ = true;
            zero_masks_data_first_ = false;
            ++zero_masks_generation_;
        }
        int prepare_error = 0;
        ZeroMaskPersistJob* job = make_zero_mask_persist_job(
            zero_mask_path_, std::move(snapshot.zeroes), snapshot.vsize, false,
            &prepare_error);
        if (job == nullptr) co_return prepare_error;
        ZeroMaskPersistResult prc;
        try {
            prc = co_await run_owned_blocking(job);
        } catch (const std::bad_alloc&) {
            co_return -ENOMEM;
        }
        if (prc.rc != 0) co_return prc.rc;
        {
            std::lock_guard lock(meta_mu_);
            published_zero_masks_generation_ = zero_masks_generation_;
        }

        // Real deallocation: the blocks return to the filesystem and the range
        // reads back as zeroes from this layer. The sidecar already carries the
        // semantic mask, so the punch is no longer the first durable state change.
        // If punch-hole is unsupported or otherwise fails after the mask commit,
        // discard still succeeded semantically; only physical space reclamation is
        // skipped for this request.
        const auto reclaim = co_await elio::io::allocate_file_range(
            fd_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, offset, len);
        (void)reclaim;
        co_return 0;
    } catch (const std::bad_alloc&) {
        co_return -ENOMEM;
    } catch (const std::length_error&) {
        co_return -EFBIG;
    }
#endif
}

elio::coro::task<int> SparseRwLayer::flush() {
    bool already_checkpointed = false;
    const int gate = begin_flush_op(&already_checkpointed);
    if (gate != 0) co_return gate;
    if (already_checkpointed) co_return 0;
    ActiveOpGuard active(active_ops_, AdoptActiveOp{});
    co_await op_mu_.lock();
    SyncMutexGuard op_guard(op_mu_);
    co_return co_await flush_locked();
}

elio::coro::task<int> SparseRwLayer::flush_locked() {
    if (extent_mode_) {
        const auto synced = co_await elio::io::sync_file(fd_,
            elio::io::file_sync_mode::data_and_metadata);
        if (!synced) co_return -synced.error_value();
        auto job = [this] { return fsync_parent_dir(data_path_); };
        using Job = decltype(job);
        co_return -(co_await run_owned_blocking(new Job(std::move(job))));
    }
    std::vector<bytes::segment_mapping> zeroes;
    uint64_t generation = 0;
    uint64_t flush_vsize = 0;
    bool dirty = false;
    bool data_first = false;
    bool pending_view = false;
    bool already_published = false;
    try {
        std::lock_guard lock(meta_mu_);
        dirty = zero_masks_dirty_;
        data_first = zero_masks_data_first_;
        pending_view = zero_masks_pending_view_;
        if (dirty) {
            zeroes = pending_view ? pending_zero_masks_
                                  : zero_mask_segments_locked();
            generation = zero_masks_generation_;
            already_published = !pending_view &&
                published_zero_masks_generation_ == generation;
            flush_vsize = vsize_.load(std::memory_order_acquire);
        }
    } catch (const std::bad_alloc&) {
        co_return -ENOMEM;
    }
    if (dirty) {
        if (data_first) {
            const auto status = co_await elio::io::sync_file(fd_, elio::io::file_sync_mode::data_only);
            if (!status) co_return -status.error_value();
        }
        ZeroMaskPersistResult persisted{0, true};
        if (!already_published) {
            int prepare_error = 0;
            ZeroMaskPersistJob* job = make_zero_mask_persist_job(
                zero_mask_path_, std::move(zeroes), flush_vsize, false,
                &prepare_error);
            if (job == nullptr) co_return prepare_error;
            try {
                persisted = co_await run_owned_blocking(job);
            } catch (const std::bad_alloc&) {
                co_return -ENOMEM;
            }
        }
        if (persisted.namespace_committed && pending_view) {
            std::lock_guard lock(meta_mu_);
            if (zero_masks_generation_ == generation &&
                zero_masks_pending_view_) {
                segments_.swap(pending_data_segments_);
                segments_generation_.fetch_add(1, std::memory_order_release);
                pending_zero_masks_.clear();
                pending_data_segments_.clear();
                zero_masks_pending_view_ = false;
            }
        }
        if (persisted.rc != 0) co_return persisted.rc;
        if (!data_first) {
            const auto status = co_await elio::io::sync_file(fd_, elio::io::file_sync_mode::data_only);
            if (!status) co_return -status.error_value();
        }
        {
            std::lock_guard lock(meta_mu_);
            if (zero_masks_generation_ == generation) {
                zero_masks_dirty_ = false;
                zero_masks_data_first_ = false;
                pending_zero_masks_.clear();
                pending_data_segments_.clear();
                zero_masks_pending_view_ = false;
            }
        }
    } else {
        const auto status = co_await elio::io::sync_file(fd_, elio::io::file_sync_mode::data_only);
        if (!status) co_return -status.error_value();
    }
    if (coverage_dirty_) {
        // Guest FLUSH orders backing bytes before the authoritative ownership
        // map. After this point a durable DATA bit can never refer to data that
        // the same flush left unsynchronized.
        const uint64_t generation = coverage_generation_;
        const auto status = co_await elio::io::sync_file(
            coverage_fd_, elio::io::file_sync_mode::data_only);
        if (!status) co_return -status.error_value();
        if (coverage_generation_ == generation) coverage_dirty_ = false;
    }
    co_return 0;
}

elio::coro::task<int> SparseRwLayer::checkpoint() {
    const int gate = begin_checkpoint_op();
    if (gate != 0) co_return gate;
    struct ResetGuard {
        SparseRwLayer* self;
        bool armed = true;
        ~ResetGuard() {
            if (armed) {
                self->end_checkpoint_op(Lifecycle::kOpen);
            }
        }
    } reset{this};
    co_await op_mu_.lock();
    SyncMutexGuard op_guard(op_mu_);
    const int rc = co_await flush_locked();
    end_checkpoint_op(rc == 0 ? Lifecycle::kCheckpointed : Lifecycle::kOpen);
    reset.armed = false;
    co_return rc;
}


std::optional<uint64_t> SparseRwLayer::segments_revision() const noexcept {
    std::lock_guard lock(meta_mu_);
    const uint64_t generation = segments_generation_.load(std::memory_order_acquire);
    if (!extent_mode_ || !extent_view_valid_ || generation == UINT64_MAX) return std::nullopt;
    return generation;
}

void SparseRwLayer::advance_extent_generation() {
    const uint64_t generation = segments_generation_.load(std::memory_order_relaxed);
    if (generation != UINT64_MAX)
        segments_generation_.store(generation + 1, std::memory_order_release);
}

elio::coro::task<std::unique_ptr<SparseRwLayer>> SparseRwLayer::open(
    const std::string& path, uint64_t vsize) {
    auto job = [path = std::string(path), vsize] {
        return open_sync(path, vsize, false, true);
    };
    using Job = decltype(job);
    co_return co_await run_owned_blocking(new Job(std::move(job)));
}

elio::coro::task<int> SparseRwLayer::refresh_extents() {
    try {
        auto job = [this] { return scan_sparse_extents(fd_, virtual_size()); };
        using Job = decltype(job);
        auto next = co_await run_owned_blocking(new Job(std::move(job)));
        std::lock_guard lock(meta_mu_);
        segments_.swap(next);
        extent_view_valid_ = true;
        advance_extent_generation();
        co_return 0;
    } catch (const error& e) {
        std::lock_guard lock(meta_mu_);
        extent_view_valid_ = false;
        co_return -e.errno_value();
    } catch (const std::bad_alloc&) {
        std::lock_guard lock(meta_mu_);
        extent_view_valid_ = false;
        co_return -ENOMEM;
    }
}

elio::coro::task<ssize_t> SparseRwLayer::pwrite_with_view(
    const void* buf, size_t count, uint64_t offset, source::BlobSource& view) {
    if (!extent_mode_) {
            co_return co_await pwrite(buf, count, offset);
    }
    co_return co_await write_extents(buf, count, offset, view, false);
}

elio::coro::task<int> SparseRwLayer::discard_with_view(
    uint64_t offset, uint64_t len, source::BlobSource& view) {
    if (!extent_mode_) {
            co_return co_await discard(offset, len);
    }
    if (len > static_cast<uint64_t>(std::numeric_limits<ssize_t>::max())) co_return -EINVAL;
    const auto result = co_await write_extents(nullptr, static_cast<size_t>(len), offset, view, true);
    co_return result < 0 ? static_cast<int>(result) : 0;
}

elio::coro::task<ssize_t> SparseRwLayer::write_extents(
    const void* buffer, size_t count, uint64_t offset, source::BlobSource& view, bool zero) {
    const int gate = begin_data_op(offset, count);
    if (gate != 0) co_return gate;
    ActiveOpGuard active(active_ops_, AdoptActiveOp{});
    if (!count || count > static_cast<size_t>(std::numeric_limits<ssize_t>::max()) ||
        offset % kSector || count % kSector) co_return -EINVAL;
    co_await op_mu_.lock();
    SyncMutexGuard guard(op_mu_);
    try {
        // generic_write_checks can truncate a buffered request at RLIMIT_FSIZE.
        // Reject that reachable short-write path before any block mutation.
        struct rlimit limit {};
        if (::getrlimit(RLIMIT_FSIZE, &limit) != 0) co_return -errno;
        const uint64_t materialized_end = (offset + count + 4095) / 4096 * 4096;
        if (limit.rlim_cur != RLIM_INFINITY && materialized_end > limit.rlim_cur)
            co_return -EFBIG;
        bool valid;
        {
            std::lock_guard lock(meta_mu_);
            valid = extent_view_valid_;
        }
        if (!valid) {
            const int repaired = co_await refresh_extents();
            if (repaired != 0) co_return repaired;
        }
        bool unchanged_data = false;
        {
            std::lock_guard lock(meta_mu_);
            unchanged_data = !zero && range_is_data(segments_, offset / kSector,
                                                   (offset + count) / kSector);
        }
        if (unchanged_data) {
            const auto write = co_await elio::io::pwrite_exactly(fd_,
                std::span<const std::byte>(static_cast<const std::byte*>(buffer), count), offset);
            if (write.error) co_return -write.error.value();
            if (write.end != elio::io::transfer_end::complete) co_return -EIO;
            co_return static_cast<ssize_t>(count);
        }
        struct Block {
            std::unique_ptr<std::byte, decltype(&std::free)> storage{
                static_cast<std::byte*>(std::aligned_alloc(4096, 4096)), &std::free};
            std::byte* data() const { return storage.get(); }
            constexpr size_t size() const { return 4096; }
        } block;
        if (!block.data()) co_return -ENOMEM;
        const auto* input = static_cast<const std::byte*>(buffer);
        size_t done = 0;
        while (done < count) {
            const uint64_t position = offset + done;
            const uint64_t base = position / block.size() * block.size();
            const size_t within = static_cast<size_t>(position - base);
            const size_t n = std::min(count - done, block.size() - within);
            const bool whole = within == 0 && n == block.size();
            bool owned = false;
            std::vector<bytes::segment_mapping> next;
            {
                std::lock_guard lock(meta_mu_);
                uint64_t cursor = base / kSector;
                for (const auto& s : segments_) {
                    if (s.end() <= cursor) continue;
                    if (s.offset > cursor) break;
                    cursor = s.end();
                    if (cursor >= (base + block.size()) / kSector) {
                        owned = true;
                        break;
                    }
                }
                next = segments_;
            }
            const bool copyup = !whole && !owned;
            const uint64_t write_offset = copyup ? base : position;
            const size_t write_count = copyup ? block.size() : n;
            if (copyup) {
                const auto read = co_await view.pread(block.data(), block.size(), base);
                if (read < 0) co_return read;
                if (read != static_cast<ssize_t>(block.size())) co_return -EIO;
            }
            if (zero) std::fill_n(block.data() + (copyup ? within : 0), n, std::byte{});
            else std::memcpy(block.data() + (copyup ? within : 0), input + done, n);
            const uint64_t lo = write_offset / kSector;
            const uint64_t hi = (write_offset + write_count) / kSector;
            erase_range_from(next, lo, hi);
            if (zero && whole) push_zero_range(next, lo, hi);
            else insert_live_extent_into(next, lo, hi - lo);
            // Prepare every allocating index change before modifying payload.
            std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) {
                return a.offset < b.offset;
            });
            {
                std::lock_guard lock(meta_mu_);
                extent_view_valid_ = false;
            }
#ifdef OBD_TEST_HOOKS
            if (extent_exit_stage_for_test_.load() == 1) ::_exit(91);
#endif
            int failure = 0;
            bool allocated_zero = false;
            if (zero && whole) {
                const auto result = co_await elio::io::allocate_file_range(
                    fd_, FALLOC_FL_ZERO_RANGE | FALLOC_FL_KEEP_SIZE, base, block.size());
                if (result) allocated_zero = true;
                else if (result.error_value() != EOPNOTSUPP && result.error_value() != ENOSYS)
                    failure = -result.error_value();
            }
            if (!allocated_zero && !failure) {
                const auto write = co_await elio::io::pwrite_exactly(
                    fd_, std::span<const std::byte>(block.data(), write_count), write_offset);
                if (write.error) failure = -write.error.value();
                else if (write.end != elio::io::transfer_end::complete) failure = -EIO;
            }
            if (failure) {
                // Preserve the kernel-observable prefix after an error. If the
                // scan fails, future reads/index rebuilds fail closed.
                (void)co_await refresh_extents();
                co_return failure;
            }
#ifdef OBD_TEST_HOOKS
            if (extent_exit_stage_for_test_.load() == 2) ::_exit(92);
#endif
            {
                std::lock_guard lock(meta_mu_);
                segments_.swap(next);
                extent_view_valid_ = true;
                advance_extent_generation();
            }
            done += n;
        }
        co_return static_cast<ssize_t>(count);
    } catch (const error& e) {
        co_return -e.errno_value();
    } catch (const std::bad_alloc&) {
        co_return -ENOMEM;
    }
}

}  // namespace obd::format
