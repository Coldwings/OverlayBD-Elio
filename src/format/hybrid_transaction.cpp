#include "format/hybrid_transaction.hpp"
#include "format/hybrid_format.hpp"
#include "format/hybrid_log_state.hpp"
#include "format/writer.hpp"
#include "common/crc32c.hpp"
#include "common/errors.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <iterator>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace obd::format::hybrid {
namespace {
constexpr size_t kRecordSize = 16384;
constexpr size_t kChecksum = 88;
using Header = std::array<uint8_t, lsmt::kSpace>;
using Record = std::array<uint8_t, kRecordSize>;
constexpr uint8_t kMagic[8] = {'O','B','D','H','Y','T','X','1'};
struct Fd {
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int value;
};
struct Pending {
    int directory;
    std::string name;
    bool created = false;
    bool published = false;
    ~Pending() { if (created && !published) ::unlinkat(directory, name.c_str(), 0); }
};
struct Intent {
    uint32_t operation = 0;
    uint64_t data_device = 0, data_inode = 0, index_device = 0, index_inode = 0;
    uint64_t data_bytes = 0, index_bytes = 0;
    uint32_t index_crc = 0;
    Header old_data{}, old_index{}, target{};
};
#ifdef OBD_TEST_HOOKS
std::atomic<int> fault_stage{-1};
std::atomic<bool> fault_exit{false};
#endif
void observe(int stage) {
#ifdef OBD_TEST_HOOKS
    if (fault_stage.load() == stage) {
        if (fault_exit.load()) ::_exit(86);
        throw error(EIO, "hybrid: injected transaction boundary failure");
    }
#else
    (void)stage;
#endif
}
void read_exact(int fd, void* data, size_t size, uint64_t offset) {
    size_t done = 0;
    while (done < size) {
        const auto result = ::pread(fd, static_cast<uint8_t*>(data) + done, size - done,
                                    static_cast<off_t>(offset + done));
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) throw_errno(result < 0 ? errno : EIO, "hybrid: transaction read");
        done += static_cast<size_t>(result);
    }
}
void write_exact(int fd, const void* data, size_t size, uint64_t offset) {
    size_t done = 0;
    while (done < size) {
        const auto result = ::pwrite(fd, static_cast<const uint8_t*>(data) + done, size - done,
                                     static_cast<off_t>(offset + done));
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) throw_errno(result < 0 ? errno : EIO, "hybrid: transaction write");
        done += static_cast<size_t>(result);
    }
}
void sync(int fd) {
    if (::fsync(fd) != 0) throw_errno(errno, "hybrid: transaction sync");
}
uint32_t index_crc(int fd, uint64_t size) {
    if (size < lsmt::kSpace || (size - lsmt::kSpace) % 16 != 0 ||
        (size - lsmt::kSpace) / 16 > kMaxLogRecords)
        throw format_error("hybrid: transaction index bound");
    std::array<uint8_t, 64 * 1024> chunk{};
    uint32_t crc = 0;
    for (uint64_t offset = lsmt::kSpace; offset < size;) {
        const auto count = static_cast<size_t>(std::min<uint64_t>(chunk.size(), size - offset));
        read_exact(fd, chunk.data(), count, offset);
        crc = crc32::crc32c_extend(chunk.data(), count, crc);
        offset += count;
    }
    return crc;
}
Record encode(const Intent& intent) {
    Record result{};
    std::copy(std::begin(kMagic), std::end(kMagic), result.begin());
    bytes::store_u32_le(result.data() + 8, 1);
    bytes::store_u32_le(result.data() + 12, intent.operation);
    bytes::store_u64_le(result.data() + 16, intent.data_device);
    bytes::store_u64_le(result.data() + 24, intent.data_inode);
    bytes::store_u64_le(result.data() + 32, intent.index_device);
    bytes::store_u64_le(result.data() + 40, intent.index_inode);
    bytes::store_u64_le(result.data() + 48, intent.data_bytes);
    bytes::store_u64_le(result.data() + 56, intent.index_bytes);
    bytes::store_u32_le(result.data() + 80, intent.index_crc);
    bytes::store_u32_le(result.data() + 92, kRecordSize);
    std::copy(intent.old_data.begin(), intent.old_data.end(), result.begin() + 4096);
    std::copy(intent.old_index.begin(), intent.old_index.end(), result.begin() + 8192);
    std::copy(intent.target.begin(), intent.target.end(), result.begin() + 12288);
    bytes::store_u32_le(result.data() + kChecksum, crc32::crc32c(result.data(), result.size()));
    return result;
}
Intent decode(Record record) {
    const auto expected = bytes::load_u32_le(record.data() + kChecksum);
    bytes::store_u32_le(record.data() + kChecksum, 0);
    if (std::memcmp(record.data(), kMagic, sizeof(kMagic)) != 0 ||
        bytes::load_u32_le(record.data() + 8) != 1 ||
        bytes::load_u32_le(record.data() + 92) != kRecordSize ||
        expected != crc32::crc32c(record.data(), record.size()))
        throw format_error("hybrid: invalid publication intent");
    for (size_t i = 64; i < 80; ++i)
        if (record[i] != 0) throw format_error("hybrid: unsupported intent fields");
    for (size_t i = 84; i < 88; ++i)
        if (record[i] != 0) throw format_error("hybrid: unsupported intent fields");
    for (size_t i = 96; i < 4096; ++i)
        if (record[i] != 0) throw format_error("hybrid: unsupported intent fields");
    Intent out;
    out.operation = bytes::load_u32_le(record.data() + 12);
    out.data_device = bytes::load_u64_le(record.data() + 16);
    out.data_inode = bytes::load_u64_le(record.data() + 24);
    out.index_device = bytes::load_u64_le(record.data() + 32);
    out.index_inode = bytes::load_u64_le(record.data() + 40);
    out.data_bytes = bytes::load_u64_le(record.data() + 48);
    out.index_bytes = bytes::load_u64_le(record.data() + 56);
    out.index_crc = bytes::load_u32_le(record.data() + 80);
    std::copy_n(record.begin() + 4096, 4096, out.old_data.begin());
    std::copy_n(record.begin() + 8192, 4096, out.old_index.begin());
    std::copy_n(record.begin() + 12288, 4096, out.target.begin());
    if (out.operation != 1 && out.operation != 2)
        throw format_error("hybrid: unsupported publication intent");
    return out;
}
uint64_t trailer_offset(const Intent& intent) {
    const auto count = intent.index_bytes - lsmt::kSpace;
    const auto limit = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (intent.data_bytes > limit - count - 2 * lsmt::kSpace)
        throw format_error("hybrid: sealed artifact size overflow");
    return (intent.data_bytes + count + lsmt::kSpace - 1) / lsmt::kSpace * lsmt::kSpace;
}
bool allowed_header(const Header& current, const Header& old, const Header& target) {
    for (size_t i = 0; i < current.size(); ++i)
        if (current[i] != old[i] && current[i] != target[i]) return false;
    return true;
}
void validate(const Intent& intent, int data_fd, int index_fd) {
    struct stat data{}, index{};
    if (::fstat(data_fd, &data) != 0 || ::fstat(index_fd, &index) != 0)
        throw_errno(errno, "hybrid: transaction artifact stat");
    if (static_cast<uint64_t>(data.st_dev) != intent.data_device ||
        static_cast<uint64_t>(data.st_ino) != intent.data_inode ||
        static_cast<uint64_t>(index.st_dev) != intent.index_device ||
        static_cast<uint64_t>(index.st_ino) != intent.index_inode ||
        static_cast<uint64_t>(index.st_size) != intent.index_bytes)
        throw format_error("hybrid: stale publication intent identity");
    const auto old_data = lsmt::HeaderTrailer::parse(intent.old_data.data());
    const auto old_index = lsmt::HeaderTrailer::parse(intent.old_index.data());
    (void)validate_pair(old_data, old_index, intent.data_bytes, intent.index_bytes);
    const auto target = lsmt::HeaderTrailer::parse(intent.target.data());
    Header current_data{}, current_index{}, expected_index = intent.old_index;
    read_exact(data_fd, current_data.data(), current_data.size(), 0);
    read_exact(index_fd, current_index.data(), current_index.size(), 0);
    if (intent.operation == 1) {
        auto expected_data = intent.old_data;
        if (target.virtual_size <= old_data.virtual_size || target.virtual_size % 512 != 0 ||
            target.virtual_size / 512 > bytes::segment_mapping::kMaxOffset ||
            static_cast<uint64_t>(data.st_size) != intent.data_bytes)
            throw format_error("hybrid: invalid grow intent");
        bytes::store_u64_le(expected_data.data() + 48, target.virtual_size);
        bytes::store_u64_le(expected_index.data() + 48, target.virtual_size);
        if (expected_data != intent.target ||
            !allowed_header(current_data, intent.old_data, expected_data) ||
            !allowed_header(current_index, intent.old_index, expected_index))
            throw format_error("hybrid: grow intent header mismatch");
    } else {
        const auto index_count = (intent.index_bytes - lsmt::kSpace) / 16;
        auto expected = old_data;
        expected.flags = kHybridFlag | 6u;
        expected.index_offset = intent.data_bytes;
        expected.index_size = index_count;
        expected.user_tag = target.user_tag;
        Header expected_target{};
        expected.serialize(expected_target.data());
        if (index_count > lsmt::kMaxRoIndexSize || expected_target != intent.target ||
            current_data != intent.old_data || current_index != intent.old_index ||
            static_cast<uint64_t>(data.st_size) < intent.data_bytes ||
            static_cast<uint64_t>(data.st_size) > trailer_offset(intent) + lsmt::kSpace)
            throw format_error("hybrid: invalid seal intent");
    }
    if (index_crc(index_fd, intent.index_bytes) != intent.index_crc)
        throw format_error("hybrid: transaction index content mismatch");
    if (intent.operation == 2) {
        Replay replay(old_data.virtual_size, intent.data_bytes);
        std::array<uint8_t, 64 * 1024> chunk{};
        uint64_t prior_end = 0;
        for (uint64_t pos = lsmt::kSpace; pos < intent.index_bytes;) {
            const auto size = static_cast<size_t>(std::min<uint64_t>(chunk.size(), intent.index_bytes - pos));
            read_exact(index_fd, chunk.data(), size, pos);
            for (size_t off = 0; off < size; off += 16) {
                const auto m = bytes::load_segment_le(chunk.data() + off);
                if (m.length == 0 || m.offset < prior_end || m.tag != 0 ||
                    m.moffset == kNoPhysicalOffset)
                    throw format_error("hybrid: seal requires a canonical index");
                prior_end = m.end();
            }
            replay.append(std::span(chunk).first(size));
            pos += size;
        }
        for (const auto& m : replay.finish())
            if (m.moffset == kNoPhysicalOffset)
                throw format_error("hybrid: seal index has ambiguous ZERO backing");
    }
}
void finish(const Intent& intent, int data_fd, int index_fd, int directory_fd,
            const std::string& name) {
    validate(intent, data_fd, index_fd);
    if (intent.operation == 1) {
        write_exact(data_fd, intent.target.data(), intent.target.size(), 0);
        sync(data_fd);
        observe(2);
        auto index_header = intent.old_index;
        bytes::store_u64_le(index_header.data() + 48,
            bytes::load_u64_le(intent.target.data() + 48));
        write_exact(index_fd, index_header.data(), index_header.size(), 0);
        sync(index_fd);
        observe(3);
    } else {
        std::array<uint8_t, 64 * 1024> chunk{};
        uint64_t done = 0, count = intent.index_bytes - lsmt::kSpace;
        while (done < count) {
            const auto size = static_cast<size_t>(std::min<uint64_t>(chunk.size(), count - done));
            read_exact(index_fd, chunk.data(), size, lsmt::kSpace + done);
            write_exact(data_fd, chunk.data(), size, intent.data_bytes + done);
            done += size;
        }
        observe(4);
        const auto trailer = trailer_offset(intent);
        Header zeros{};
        const auto padding = static_cast<size_t>(trailer - intent.data_bytes - count);
        if (padding) write_exact(data_fd, zeros.data(), padding, intent.data_bytes + count);
        write_exact(data_fd, intent.target.data(), intent.target.size(), trailer);
        observe(5);
        sync(data_fd);
        observe(6);
    }
    if (::unlinkat(directory_fd, name.c_str(), 0) != 0)
        throw_errno(errno, "hybrid: retire publication intent");
    observe(7);
    sync(directory_fd);
}
Intent prepare(int data_fd, int index_fd, uint32_t operation) {
    struct stat data{}, index{};
    if (::fstat(data_fd, &data) != 0 || ::fstat(index_fd, &index) != 0)
        throw_errno(errno, "hybrid: prepare publication stat");
    Intent intent;
    intent.operation = operation;
    intent.data_device = data.st_dev;
    intent.data_inode = data.st_ino;
    intent.index_device = index.st_dev;
    intent.index_inode = index.st_ino;
    intent.data_bytes = static_cast<uint64_t>(data.st_size);
    intent.index_bytes = static_cast<uint64_t>(index.st_size);
    read_exact(data_fd, intent.old_data.data(), intent.old_data.size(), 0);
    read_exact(index_fd, intent.old_index.data(), intent.old_index.size(), 0);
    (void)validate_pair(lsmt::HeaderTrailer::parse(intent.old_data.data()),
                        lsmt::HeaderTrailer::parse(intent.old_index.data()),
                        intent.data_bytes, intent.index_bytes);
    if (is_sealed_pair(data_fd, index_fd))
        throw error(EALREADY, "hybrid: already sealed");
    intent.index_crc = index_crc(index_fd, intent.index_bytes);
    return intent;
}
void publish(const Intent& intent, int data_fd, int index_fd, int directory_fd,
             const std::string& name) {
    validate(intent, data_fd, index_fd);
    sync(data_fd);
    sync(index_fd);
    const auto record = encode(intent);
    Pending pending{directory_fd, ".hybrid-intent-" + generate_uuid(), false, false};
    Fd fd(::openat(directory_fd, pending.name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (fd.value < 0) throw_errno(errno, "hybrid: create publication intent");
    pending.created = true;
    write_exact(fd.value, record.data(), record.size(), 0);
    sync(fd.value);
    if (::syscall(SYS_renameat2, directory_fd, pending.name.c_str(), directory_fd,
                  name.c_str(), RENAME_NOREPLACE) != 0)
        throw_errno(errno, "hybrid: publish intent");
    pending.published = true;
    sync(directory_fd);
    observe(1);
    finish(intent, data_fd, index_fd, directory_fd, name);
}
}

bool is_sealed_pair(int data_fd, int index_fd) {
    struct stat data{}, index{};
    if (::fstat(data_fd, &data) != 0 || ::fstat(index_fd, &index) != 0)
        throw_errno(errno, "hybrid: inspect sealed pair");
    // Complete appended metadata determines seal state. Local log attributes
    // neither establish a sealed lower nor override a valid sealed trailer.
    if (data.st_size < 2 * lsmt::kSpace) return false;
    const auto data_bytes = static_cast<uint64_t>(data.st_size);
    const auto index_bytes = static_cast<uint64_t>(index.st_size);
    const auto tail_offset = data_bytes - lsmt::kSpace;
    Header tail{};
    read_exact(data_fd, tail.data(), tail.size(), tail_offset);
    lsmt::HeaderTrailer trailer;
    try { trailer = lsmt::HeaderTrailer::parse(tail.data()); }
    catch (const format_error&) { return false; }
    if (trailer.flags != (kHybridFlag | 6u)) return false;

    Header first{}, sidecar{};
    read_exact(data_fd, first.data(), first.size(), 0);
    read_exact(index_fd, sidecar.data(), sidecar.size(), 0);
    const auto header = lsmt::HeaderTrailer::parse(first.data());
    (void)validate_pair(header, lsmt::HeaderTrailer::parse(sidecar.data()), data_bytes, index_bytes);
    // Match the sealed reader: trailer fields are authoritative, and extra
    // padding before the EOF trailer does not make a sealed layer writable.
    if (trailer.version != 1 || trailer.sub_version != 1 ||
        trailer.index_offset < lsmt::kSpace ||
        trailer.index_offset > tail_offset || trailer.index_size > lsmt::kMaxRoIndexSize ||
        trailer.index_size * 16 > tail_offset - trailer.index_offset)
        return false;
    // Validate the self-contained read-only index. A stale or misleading
    // writable sidecar must never turn complete sealed metadata into payload.
    uint64_t previous_end = 0;
    uint64_t remaining = trailer.index_size * 16;
    uint64_t offset = trailer.index_offset;
    const auto payload_sectors = trailer.index_offset / lsmt::kAlignment;
    std::array<uint8_t, 64 * 1024> chunk{};
    while (remaining) {
        const auto count = static_cast<size_t>(std::min<uint64_t>(remaining, chunk.size()));
        read_exact(data_fd, chunk.data(), count, offset);
        for (size_t pos = 0; pos < count; pos += 16) {
            const auto m = bytes::load_segment_le(chunk.data() + pos);
            if (m.offset == bytes::segment_mapping::kInvalidOffset) continue;
            if (m.length == 0 || trailer.virtual_size % lsmt::kAlignment != 0 ||
                m.offset < previous_end ||
                m.end() > trailer.virtual_size / lsmt::kAlignment)
                return false;
            const bool valid_physical = m.zeroed
                ? (m.moffset == kNoPhysicalOffset ||
                   (m.moffset >= lsmt::kDataStartSector && m.moffset <= payload_sectors))
                : (m.moffset >= lsmt::kDataStartSector && m.moffset < payload_sectors &&
                   m.length <= payload_sectors - m.moffset);
            if (!valid_physical) return false;
            previous_end = m.end();
        }
        offset += count;
        remaining -= count;
    }
    return true;
}

#ifdef OBD_TEST_HOOKS
void transaction_fault_for_test(int stage, bool exit_process) {
    fault_exit.store(exit_process);
    fault_stage.store(stage);
}
#endif
bool recover_transaction(int data_fd, int index_fd, int directory_fd,
                         const std::string& name, bool writable) {
    Fd fd(::openat(directory_fd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (fd.value < 0) {
        if (errno == ENOENT) return false;
        throw_errno(errno, "hybrid: open publication intent");
    }
    struct stat status{};
    if (::fstat(fd.value, &status) != 0) throw_errno(errno, "hybrid: stat publication intent");
    if (!S_ISREG(status.st_mode) || status.st_size != kRecordSize)
        throw format_error("hybrid: truncated publication intent");
    Record record{};
    read_exact(fd.value, record.data(), record.size(), 0);
    const auto intent = decode(record);
    if (!writable) throw error(EBUSY, "hybrid: publication recovery requires exclusive ownership");
    finish(intent, data_fd, index_fd, directory_fd, name);
    return true;
}
void grow_transaction(int data_fd, int index_fd, int directory_fd,
                      const std::string& name, uint64_t virtual_size) {
    auto intent = prepare(data_fd, index_fd, 1);
    intent.target = intent.old_data;
    bytes::store_u64_le(intent.target.data() + 48, virtual_size);
    publish(intent, data_fd, index_fd, directory_fd, name);
}
void seal_transaction(int data_fd, int index_fd, int directory_fd,
                      const std::string& name, const std::string& user_tag) {
    auto intent = prepare(data_fd, index_fd, 2);
    auto trailer = lsmt::HeaderTrailer::parse(intent.old_data.data());
    trailer.flags = kHybridFlag | 6u;
    trailer.index_offset = intent.data_bytes;
    trailer.index_size = (intent.index_bytes - lsmt::kSpace) / 16;
    trailer.user_tag = user_tag;
    trailer.serialize(intent.target.data());
    publish(intent, data_fd, index_fd, directory_fd, name);
}
}  // namespace obd::format::hybrid
