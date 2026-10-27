#include "format/hybrid_log_state.hpp"
#include "format/hybrid_format.hpp"
#include "format/writer.hpp"
#include "common/crc32c.hpp"
#include "common/errors.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace obd::format::hybrid {
namespace {
#ifdef OBD_TEST_HOOKS
std::atomic<int> fault_stage{-1};
#endif
void observe(int stage) {
#ifdef OBD_TEST_HOOKS
    if (fault_stage.load() == stage)
        throw error(EIO, "hybrid: injected log state publication failure");
#else
    (void)stage;
#endif
}
constexpr size_t kSize = 128;
constexpr uint32_t kStable = 1, kReplacing = 2, kInstalling = 3;
constexpr uint8_t kMagic[8] = {'O','B','D','H','Y','L','0','1'};
struct State {
    uint32_t kind = kStable;
    LogStamp first{}, second{};
};
using Record = std::array<uint8_t, kSize>;
void bound(uint64_t bytes) {
    if (bytes < lsmt::kSpace || (bytes - lsmt::kSpace) % 16 != 0 ||
        (bytes - lsmt::kSpace) / 16 > kMaxLogRecords)
        throw format_error("hybrid: invalid log state length");
}
uint64_t file_size(int fd) {
    struct stat status{};
    if (::fstat(fd, &status) != 0) throw_errno(errno, "hybrid: log state stat");
    if (!S_ISREG(status.st_mode) || status.st_size < 0)
        throw format_error("hybrid: invalid log state file");
    return static_cast<uint64_t>(status.st_size);
}
uint32_t prefix_crc(int fd, uint64_t bytes) {
    bound(bytes);
    std::array<uint8_t, 64 * 1024> buffer{};
    uint32_t crc = 0;
    for (uint64_t offset = lsmt::kSpace; offset < bytes;) {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), bytes - offset));
        size_t done = 0;
        while (done < count) {
            const auto result = ::pread(fd, buffer.data() + done, count - done,
                                       static_cast<off_t>(offset + done));
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0) throw_errno(result < 0 ? errno : EIO, "hybrid: read log state prefix");
            done += static_cast<size_t>(result);
        }
        crc = crc32::crc32c_extend(buffer.data(), count, crc);
        offset += count;
    }
    return crc;
}
bool matches(int fd, uint64_t size, const LogStamp& stamp) {
    return stamp.bytes <= size && prefix_crc(fd, stamp.bytes) == stamp.crc;
}
void sync(int fd) {
    if (::fsync(fd) != 0) throw_errno(errno, "hybrid: sync log integrity metadata");
}
void encode_stamp(uint8_t* out, const LogStamp& stamp) {
    std::copy(stamp.generation.begin(), stamp.generation.end(), out);
    bytes::store_u64_le(out + 16, stamp.bytes);
    bytes::store_u32_le(out + 24, stamp.crc);
}
LogStamp decode_stamp(const uint8_t* in) {
    LogStamp stamp;
    std::copy_n(in, stamp.generation.size(), stamp.generation.begin());
    stamp.bytes = bytes::load_u64_le(in + 16);
    stamp.crc = bytes::load_u32_le(in + 24);
    bound(stamp.bytes);
    if (std::all_of(stamp.generation.begin(), stamp.generation.end(), [](uint8_t b) { return b == 0; }))
        throw format_error("hybrid: missing log generation");
    for (size_t i = 28; i < 48; ++i)
        if (in[i] != 0) throw format_error("hybrid: unsupported log stamp fields");
    return stamp;
}
Record encode(const State& state) {
    Record out{};
    std::copy(std::begin(kMagic), std::end(kMagic), out.begin());
    bytes::store_u32_le(out.data() + 8, 1);
    bytes::store_u32_le(out.data() + 12, state.kind);
    encode_stamp(out.data() + 32, state.first);
    if (state.kind == kReplacing) encode_stamp(out.data() + 80, state.second);
    bytes::store_u32_le(out.data() + 16, crc32::crc32c(out.data(), out.size()));
    return out;
}
std::optional<State> read_state(int fd) {
    Record raw{};
    const auto count = ::fgetxattr(fd, kLogStateAttribute, raw.data(), raw.size());
    if (count < 0) {
        if (errno == ENODATA || errno == EOPNOTSUPP) return std::nullopt;
        if (errno == ERANGE) throw format_error("hybrid: oversized log integrity metadata");
        throw_errno(errno, "hybrid: read log integrity metadata");
    }
    if (count != static_cast<ssize_t>(raw.size()))
        throw format_error("hybrid: truncated log integrity metadata");
    const auto expected = bytes::load_u32_le(raw.data() + 16);
    bytes::store_u32_le(raw.data() + 16, 0);
    if (std::memcmp(raw.data(), kMagic, sizeof(kMagic)) != 0 ||
        bytes::load_u32_le(raw.data() + 8) != 1 ||
        expected != crc32::crc32c(raw.data(), raw.size()))
        throw format_error("hybrid: invalid log integrity metadata");
    State state;
    state.kind = bytes::load_u32_le(raw.data() + 12);
    if (state.kind != kStable && state.kind != kReplacing && state.kind != kInstalling)
        throw format_error("hybrid: unsupported log integrity state");
    for (size_t i = 20; i < 32; ++i)
        if (raw[i] != 0) throw format_error("hybrid: unsupported log integrity fields");
    state.first = decode_stamp(raw.data() + 32);
    if (state.kind == kReplacing) state.second = decode_stamp(raw.data() + 80);
    else for (size_t i = 80; i < raw.size(); ++i)
        if (raw[i] != 0) throw format_error("hybrid: unexpected alternate log stamp");
    return state;
}
void write_state(int fd, const State& state) {
    const auto raw = encode(state);
    if (::fsetxattr(fd, kLogStateAttribute, raw.data(), raw.size(), 0) != 0)
        throw_errno(errno, "hybrid: writable integrity metadata requires user xattrs");
}
}

#ifdef OBD_TEST_HOOKS
void log_state_fault_for_test(int stage) { fault_stage.store(stage); }
#endif

LogStamp new_log_stamp(int index_fd) {
    LogStamp stamp;
    stamp.bytes = file_size(index_fd);
    bound(stamp.bytes);
    stamp.crc = prefix_crc(index_fd, stamp.bytes);
    const auto uuid = generate_uuid();
    unsigned nibble = 0;
    for (const char c : uuid) {
        if (c == '-') continue;
        const auto value = static_cast<uint8_t>(c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10);
        if (nibble % 2 == 0) stamp.generation[nibble / 2] = static_cast<uint8_t>(value << 4);
        else stamp.generation[nibble / 2] |= value;
        ++nibble;
    }
    return stamp;
}

std::optional<LogStamp> inspect_log_state(int data_fd, int index_fd) {
    const auto anchor = read_state(data_fd), progress = read_state(index_fd);
    if (!anchor && !progress) return std::nullopt; // Unmanaged upstream import.
    if (!anchor) throw format_error("hybrid: orphan index integrity metadata");
    const auto size = file_size(index_fd);
    bound(size);
    if (!progress) {
        if (anchor->kind == kInstalling && matches(index_fd, size, anchor->first))
            return anchor->first;
        throw format_error("hybrid: missing index integrity metadata");
    }
    if (progress->kind != kStable) throw format_error("hybrid: invalid index progress state");
    const auto& latest = progress->first;
    const LogStamp* committed = nullptr;
    if (latest.generation == anchor->first.generation) committed = &anchor->first;
    else if (anchor->kind == kReplacing && latest.generation == anchor->second.generation)
        committed = &anchor->second;
    if (!committed) throw format_error("hybrid: stale index generation");
    if (!matches(index_fd, size, *committed))
        throw format_error("hybrid: committed index prefix mismatch");
    // A progress update follows its complete record write, before IO returns.
    // If metadata reached disk ahead of a later unflushed record, preserve the
    // last FLUSH prefix instead of interpreting an unverified tail.
    if (latest.bytes >= committed->bytes && matches(index_fd, size, latest)) return latest;
    return *committed;
}

void publish_log_progress(int index_fd, const LogStamp& stamp) {
    write_state(index_fd, State{kStable, stamp, {}});
    observe(2);
}
void publish_log_commit(int data_fd, const LogStamp& stamp) {
    write_state(data_fd, State{kStable, stamp, {}});
    observe(3);
    sync(data_fd);
}
LogStamp activate_log_state(int data_fd, int index_fd, int directory_fd,
                           std::optional<LogStamp> recovered) {
    const auto stamp = recovered ? *recovered : new_log_stamp(index_fd);
    const auto size = file_size(index_fd);
    if (!matches(index_fd, size, stamp))
        throw format_error("hybrid: log changed during activation");
    if (size != stamp.bytes && ::ftruncate(index_fd, static_cast<off_t>(stamp.bytes)) != 0)
        throw_errno(errno, "hybrid: remove unconfirmed index tail");
    sync(data_fd);
    sync(index_fd);
    sync(directory_fd);
    if (!recovered) {
        // Import may stop between these xattrs. The installing anchor permits
        // only this verified prefix when the index marker is still absent.
        write_state(data_fd, State{kInstalling, stamp, {}});
        sync(data_fd);
        observe(1);
    }
    publish_log_progress(index_fd, stamp);
    sync(index_fd);
    publish_log_commit(data_fd, stamp);
    return stamp;
}
void prepare_log_replacement(int data_fd, int old_index_fd, int new_index_fd,
                             const LogStamp& old_stamp, const LogStamp& next_stamp) {
    sync(data_fd);
    sync(old_index_fd);
    sync(new_index_fd);
    write_state(data_fd, State{kReplacing, old_stamp, next_stamp});
    sync(data_fd);
}
}  // namespace obd::format::hybrid
