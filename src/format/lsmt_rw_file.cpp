#include "format/lsmt_rw_file.hpp"
#include "common/errors.hpp"
#include "common/crc32c.hpp"
#include "format/writer.hpp"
#include <elio/io/file_operations.hpp>
#include <elio/io/file_transfer.hpp>
#include <elio/runtime/spawn_blocking.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace obd::format::lsmt_rw {
namespace {
constexpr uint64_t kGrowMagic = 0x31574f52474d534cULL;
constexpr size_t kGrowBytes = 16 + 2 * lsmt::kSpace + 4;
constexpr uint64_t kCreateMagic = 0x3154494e494d534cULL;
constexpr size_t kCreateBytes = 8 + lsmt::kSpace + 4;
#ifdef OBD_TEST_HOOKS
std::atomic<int> io_fault{-1};
std::atomic<bool> io_exit{false};
std::atomic<int> grow_fault{-1};
std::atomic<bool> grow_exit{false};
std::atomic<int> create_fault{-1};
std::atomic<bool> create_exit{false};
std::atomic<int> migration_fault{-1};
std::atomic<bool> migration_exit{false};
#endif
bool io_failure(int stage) {
#ifdef OBD_TEST_HOOKS
    if (io_fault.load() == stage) {
        if (io_exit.load() && stage >= 5) ::_exit(120 + stage);
        return true;
    }
#else
    (void)stage;
#endif
    return false;
}
void grow_boundary(int stage) {
#ifdef OBD_TEST_HOOKS
    if (grow_fault.load() == stage) {
        if (grow_exit.load()) ::_exit(80 + stage);
        throw error(EIO, "lsmt rw: injected grow failure");
    }
#else
    (void)stage;
#endif
}
void create_boundary(int stage) {
#ifdef OBD_TEST_HOOKS
    if (create_fault.load() == stage) {
        if (create_exit.load()) ::_exit(100 + stage);
        throw error(EIO, "lsmt rw: injected creation failure");
    }
#else
    (void)stage;
#endif
}
void migration_boundary(int stage) {
#ifdef OBD_TEST_HOOKS
    if (migration_fault.load() == stage) {
        if (migration_exit.load()) ::_exit(110 + stage);
        throw error(EIO, "lsmt rw: injected migration failure");
    }
#else
    (void)stage;
#endif
}
void read_exact(int fd, void* buffer, size_t bytes, uint64_t offset) {
    auto* out = static_cast<uint8_t*>(buffer);
    while (bytes) {
        const auto n = ::pread(fd, out, bytes, static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw error(n < 0 ? errno : EIO, "lsmt rw: read pair");
        out += n; bytes -= static_cast<size_t>(n); offset += static_cast<uint64_t>(n);
    }
}
void write_exact(int fd, const void* buffer, size_t bytes, uint64_t offset) {
    const auto* in = static_cast<const uint8_t*>(buffer);
    while (bytes) {
        const auto n = ::pwrite(fd, in, bytes, static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw error(n < 0 ? errno : EIO, "lsmt rw: write index");
        in += n; bytes -= static_cast<size_t>(n); offset += static_cast<uint64_t>(n);
    }
}
void lock(int fd) {
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0)
        throw error(errno, "lsmt rw: pair already owned");
}
uint64_t file_size(int fd) {
    struct stat st{};
    if (::fstat(fd, &st) != 0) throw error(errno, "lsmt rw: stat pair");
    if (!S_ISREG(st.st_mode) || st.st_size < 0)
        throw format_error("lsmt rw: pair requires regular files");
    return static_cast<uint64_t>(st.st_size);
}

bool sealed_trailer(int fd, uint64_t bytes) {
    if (bytes < 2 * lsmt::kSpace) return false;
    std::array<uint8_t, lsmt::kSpace> raw{};
    read_exact(fd, raw.data(), raw.size(), bytes - raw.size());
    lsmt::HeaderTrailer tail;
    try { tail = lsmt::HeaderTrailer::parse(raw.data()); }
    catch (const format_error&) { return false; }
    if (tail.flags != 6 || tail.version != 1 || tail.sub_version != 1 ||
        tail.index_offset < lsmt::kSpace || tail.index_offset > bytes - lsmt::kSpace ||
        tail.index_size > lsmt::kMaxRoIndexSize ||
        tail.index_size * 16 > bytes - lsmt::kSpace - tail.index_offset ||
        tail.virtual_size % lsmt::kAlignment != 0)
        return false;
    uint64_t previous = 0;
    std::array<uint8_t, 16> record{};
    for (uint64_t i = 0; i < tail.index_size; ++i) {
        read_exact(fd, record.data(), record.size(), tail.index_offset + 16 * i);
        const auto m = bytes::load_segment_le(record.data());
        if (m.offset == bytes::segment_mapping::kInvalidOffset) continue;
        if (m.length == 0 || m.offset < previous ||
            m.end() > tail.virtual_size / lsmt::kAlignment)
            return false;
        const auto end = tail.index_offset / lsmt::kAlignment;
        if (m.moffset < lsmt::kDataStartSector || m.moffset > end ||
            (!m.zeroed && (m.moffset == end || m.length > end - m.moffset)))
            return false;
        previous = m.end();
    }
    return true;
}
struct OpenRequest { std::string data, index; };
struct CreateRequest { std::string data, index; uint64_t size; };
struct CompactRequest { FilePair* pair; std::vector<bytes::segment_mapping> live; };
}

#ifdef OBD_TEST_HOOKS
void io_fault_for_test(int stage, bool exit_process) {
    io_exit.store(exit_process); io_fault.store(stage);
}
void grow_fault_for_test(int stage, bool exit_process) {
    grow_exit.store(exit_process); grow_fault.store(stage);
}
void create_fault_for_test(int stage, bool exit_process) {
    create_exit.store(exit_process); create_fault.store(stage);
}
void migration_fault_for_test(int stage, bool exit_process) {
    migration_exit.store(exit_process); migration_fault.store(stage);
}
#endif

FilePair::~FilePair() {
    for (const int fd : {index_fd_, data_fd_, lock_fd_}) {
        if (fd >= 0) { ::flock(fd, LOCK_UN); ::close(fd); }
    }
    if (directory_fd_ >= 0) ::close(directory_fd_);
}

void FilePair::retire_payload() {
    if (data_fd_ >= 0) { ::flock(data_fd_, LOCK_UN); ::close(data_fd_); data_fd_ = -1; }
}

std::unique_ptr<FilePair> FilePair::lock_workspace(const std::string& data,
                                                const std::string& index) {
    auto pair = std::unique_ptr<FilePair>(new FilePair);
    // This name survives data/index replacement. Never unlink the lock inode.
    pair->lock_fd_ = ::open((data + ".lock").c_str(),
                           O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (pair->lock_fd_ < 0) throw error(errno, "lsmt rw: open ownership lock");
    (void)file_size(pair->lock_fd_);
    lock(pair->lock_fd_);
    const auto parent = std::filesystem::path(index).parent_path();
    pair->directory_fd_ = ::open(parent.empty() ? "." : parent.c_str(),
                                 O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (pair->directory_fd_ < 0) throw error(errno, "lsmt rw: open index directory");
    pair->index_name_ = std::filesystem::path(index).filename().string();
    return pair;
}

std::unique_ptr<FilePair> FilePair::open_sync(const std::string& data,
                                           const std::string& index,
                                           std::unique_ptr<FilePair> pair) {
    if (!pair) pair = lock_workspace(data, index);
    pair->recover_creation(data, index);
    if (pair->data_fd_ < 0) {
        pair->data_fd_ = ::open(data.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (pair->data_fd_ < 0) throw error(errno, "lsmt rw: open existing data");
        lock(pair->data_fd_);
    }
    const auto data_bytes = file_size(pair->data_fd_);
    if (data_bytes < lsmt::kSpace) throw format_error("lsmt rw: truncated data header");
    std::array<uint8_t, lsmt::kSpace> raw{};
    read_exact(pair->data_fd_, raw.data(), raw.size(), 0);
    pair->header_ = lsmt::HeaderTrailer::parse(raw.data());
    if (pair->header_.is_sealed() || sealed_trailer(pair->data_fd_, data_bytes))
        throw error(EALREADY, "lsmt rw: sealed data cannot resume");
    if (pair->index_fd_ < 0) {
        pair->index_fd_ = ::open(index.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (pair->index_fd_ < 0) throw error(errno, "lsmt rw: open existing index");
        lock(pair->index_fd_);
    }
    const auto index_bytes = file_size(pair->index_fd_);
    if (index_bytes < lsmt::kSpace) throw format_error("lsmt rw: truncated index header");
    pair->recover_grow();
    read_exact(pair->index_fd_, raw.data(), raw.size(), 0);
    const auto ih = lsmt::HeaderTrailer::parse(raw.data());
    // A process can stop between short writes. Only an incomplete EOF record
    // is discarded; full records are replayed and validated before any repair.
    pair->index_bytes_ = lsmt::kSpace + (index_bytes - lsmt::kSpace) / 16 * 16;
    const auto complete_data = data_bytes / lsmt::kAlignment * lsmt::kAlignment;
    validate_pair(pair->header_, ih, complete_data, pair->index_bytes_);
    Replay replay(pair->header_.virtual_size, complete_data);
    for (uint64_t off = lsmt::kSpace; off < pair->index_bytes_;) {
        const auto count = static_cast<size_t>(std::min<uint64_t>(raw.size(), pair->index_bytes_ - off));
        read_exact(pair->index_fd_, raw.data(), count, off);
        replay.append(std::span(raw.data(), count));
        off += count;
    }
    pair->mappings_ = replay.finish();
    if (index_bytes != pair->index_bytes_ &&
        ::ftruncate(pair->index_fd_, static_cast<off_t>(pair->index_bytes_)) != 0)
        throw error(errno, "lsmt rw: retire interrupted index suffix");
    // Retain orphan payload; the next allocation starts after the entire file.
    pair->data_bytes_ = (data_bytes + lsmt::kAlignment - 1) / lsmt::kAlignment * lsmt::kAlignment;
    if (data_bytes != pair->data_bytes_ &&
        ::ftruncate(pair->data_fd_, static_cast<off_t>(pair->data_bytes_)) != 0)
        throw error(errno, "lsmt rw: align interrupted payload suffix");
    return pair;
}

elio::coro::task<std::unique_ptr<FilePair>> FilePair::open_existing(
    std::string data_path, std::string index_path) {
    auto request = std::make_shared<OpenRequest>(OpenRequest{std::move(data_path), std::move(index_path)});
    auto operation = [request] { return open_sync(request->data, request->index); };
    co_return co_await elio::spawn_blocking(std::move(operation));
}

bool FilePair::recover_creation(const std::string& data, const std::string& index) {
    const auto intent_name = index_name_ + ".init";
    const int intent = ::openat(directory_fd_, intent_name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (intent < 0) {
        if (errno == ENOENT) return false;
        throw error(errno, "lsmt rw: open creation intent");
    }
    struct Close { int fd; ~Close() { if (fd >= 0) ::close(fd); } } close{intent};
    auto parent = [](const std::string& path) {
        const auto p = std::filesystem::path(path).parent_path();
        return p.empty() ? std::filesystem::path(".") : p;
    };
    if (!std::filesystem::equivalent(parent(data), parent(index)))
        throw format_error("lsmt rw: creation requires colocated artifacts");
    if (file_size(intent) != kCreateBytes) throw format_error("lsmt rw: truncated creation intent");
    std::array<uint8_t, kCreateBytes> raw{};
    read_exact(intent, raw.data(), raw.size(), 0);
    if (bytes::load_u64_le(raw.data()) != kCreateMagic ||
        bytes::load_u32_le(raw.data() + raw.size() - 4) != crc32::crc32c(raw.data(), raw.size() - 4))
        throw format_error("lsmt rw: invalid creation intent checksum");
    const auto dh = lsmt::HeaderTrailer::parse(raw.data() + 8);
    auto ih = dh; ih.flags = 1; ih.index_offset = lsmt::kSpace;
    validate_pair(dh, ih, lsmt::kSpace, lsmt::kSpace);
    const auto data_name = std::filesystem::path(data).filename().string();
    if (data_name == index_name_) throw format_error("lsmt rw: aliased pair names");
    const std::array<std::string, 2> names{data_name, index_name_};
    const std::array<std::string, 2> stages{
        data_name + ".init." + dh.uuid, index_name_ + ".init." + dh.uuid};
    std::array<Close, 2> files{{{-1}, {-1}}};
    std::array<bool, 2> published{};
    std::array<uint8_t, lsmt::kSpace> expected{}, actual{};
    // Validate both complete candidates before publishing either name.
    for (size_t part = 0; part < names.size(); ++part) {
        files[part].fd = ::openat(directory_fd_, names[part].c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        published[part] = files[part].fd >= 0;
        if (files[part].fd < 0 && errno == ENOENT)
            files[part].fd = ::openat(directory_fd_, stages[part].c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (files[part].fd < 0) throw error(errno, "lsmt rw: missing creation artifact");
        lock(files[part].fd);
        if (file_size(files[part].fd) != lsmt::kSpace)
            throw format_error("lsmt rw: creation artifact is not empty");
        read_exact(files[part].fd, actual.data(), actual.size(), 0);
        (part == 0 ? dh : ih).serialize(expected.data());
        if (actual != expected) throw format_error("lsmt rw: creation artifact identity mismatch");
    }
    for (size_t part : {size_t{1}, size_t{0}}) {
        if (!published[part] && ::linkat(directory_fd_, stages[part].c_str(),
                                         directory_fd_, names[part].c_str(), 0) != 0)
            throw error(errno, "lsmt rw: publish new artifact without replacement");
        create_boundary(part == 1 ? 4 : 5);
    }
    if (::fsync(directory_fd_) != 0) throw error(errno, "lsmt rw: sync created pair names");
    if (::unlinkat(directory_fd_, intent_name.c_str(), 0) != 0)
        throw error(errno, "lsmt rw: retire creation intent");
    for (size_t part = 0; part < names.size(); ++part) {
        struct stat stage{}, live{};
        if (::fstat(files[part].fd, &live) != 0) throw error(errno, "lsmt rw: stat created artifact");
        if (::fstatat(directory_fd_, stages[part].c_str(), &stage, AT_SYMLINK_NOFOLLOW) == 0 &&
            stage.st_dev == live.st_dev && stage.st_ino == live.st_ino) {
            if (::unlinkat(directory_fd_, stages[part].c_str(), 0) != 0)
                throw error(errno, "lsmt rw: retire creation staging link");
        }
    }
    create_boundary(6);
    if (::fsync(directory_fd_) != 0) throw error(errno, "lsmt rw: sync creation retirement");
    return true;
}

std::unique_ptr<FilePair> FilePair::create_sync(const std::string& data,
                                              const std::string& index, uint64_t virtual_size) {
    auto pair = lock_workspace(data, index);
    auto parent = [](const std::string& path) {
        const auto p = std::filesystem::path(path).parent_path();
        return p.empty() ? std::filesystem::path(".") : p;
    };
    if (!std::filesystem::equivalent(parent(data), parent(index)))
        throw format_error("lsmt rw: creation requires colocated artifacts");
    if (pair->recover_creation(data, index)) {
        auto recovered = open_sync(data, index, std::move(pair));
        if (recovered->header().virtual_size != virtual_size)
            throw error(EINVAL, "lsmt rw: resumed creation has different capacity");
        return recovered;
    }
    struct stat st{};
    for (const auto& path : {data, index}) {
        if (::lstat(path.c_str(), &st) == 0) throw error(EEXIST, "lsmt rw: creation never replaces existing artifacts");
        if (errno != ENOENT) throw error(errno, "lsmt rw: inspect creation destination");
    }
    lsmt::HeaderTrailer dh;
    dh.flags = 3; dh.virtual_size = virtual_size; dh.uuid = generate_uuid();
    auto ih = dh; ih.flags = 1; ih.index_offset = lsmt::kSpace;
    validate_pair(dh, ih, lsmt::kSpace, lsmt::kSpace);
    const auto data_name = std::filesystem::path(data).filename().string();
    if (data_name == pair->index_name_) throw format_error("lsmt rw: aliased pair names");
    const std::array<std::string, 3> stages{
        data_name + ".init." + dh.uuid,
        pair->index_name_ + ".init." + dh.uuid,
        pair->index_name_ + ".init." + dh.uuid + ".intent"};
    struct Staging {
        int directory; const std::array<std::string, 3>& names;
        std::array<bool, 3> created{}; bool published = false;
        ~Staging() {
            for (size_t i = 0; i < names.size(); ++i)
                if (created[i] && (!published || i == 2)) ::unlinkat(directory, names[i].c_str(), 0);
        }
    } cleanup{pair->directory_fd_, stages};
    std::array<uint8_t, kCreateBytes> intent{};
    bytes::store_u64_le(intent.data(), kCreateMagic); dh.serialize(intent.data() + 8);
    bytes::store_u32_le(intent.data() + intent.size() - 4, crc32::crc32c(intent.data(), intent.size() - 4));
    std::array<uint8_t, lsmt::kSpace> header{};
    for (size_t part = 0; part < stages.size(); ++part) {
        const int fd = ::openat(pair->directory_fd_, stages[part].c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) throw error(errno, "lsmt rw: create staging artifact");
        cleanup.created[part] = true;
        struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
        if (part < 2) {
            (part == 0 ? dh : ih).serialize(header.data());
            write_exact(fd, header.data(), header.size(), 0);
        } else {
            write_exact(fd, intent.data(), intent.size(), 0);
        }
        if (::fdatasync(fd) != 0) throw error(errno, "lsmt rw: sync creation stage");
        if (part < 2) create_boundary(static_cast<int>(part) + 1);
    }
    const auto name = pair->index_name_ + ".init";
    if (::linkat(pair->directory_fd_, stages[2].c_str(), pair->directory_fd_, name.c_str(), 0) != 0)
        throw error(errno, "lsmt rw: publish creation intent");
    cleanup.published = true;
    if (::fsync(pair->directory_fd_) != 0) throw error(errno, "lsmt rw: sync creation intent");
    create_boundary(3);
    pair->recover_creation(data, index);
    return open_sync(data, index, std::move(pair));
}

elio::coro::task<std::unique_ptr<FilePair>> FilePair::create(
    std::string data_path, std::string index_path, uint64_t virtual_size) {
    auto request = std::make_shared<CreateRequest>(CreateRequest{
        std::move(data_path), std::move(index_path), virtual_size});
    auto operation = [request] { return create_sync(request->data, request->index, request->size); };
    co_return co_await elio::spawn_blocking(std::move(operation));
}

std::unique_ptr<FilePair> FilePair::migrate_sync(const std::string& data,
                                               const std::string& index) {
    auto pair = lock_workspace(data, index);
    pair->recover_creation(data, index);
    struct stat existing{};
    if (::lstat(index.c_str(), &existing) == 0)
        return open_sync(data, index, std::move(pair));
    if (errno != ENOENT) throw error(errno, "lsmt rw: inspect migration index");
    pair->data_fd_ = ::open(data.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (pair->data_fd_ < 0) throw error(errno, "lsmt rw: open legacy checkpoint");
    lock(pair->data_fd_);
    const auto data_bytes = file_size(pair->data_fd_);
    if (data_bytes < 2 * lsmt::kSpace || data_bytes % lsmt::kAlignment != 0)
        throw format_error("lsmt rw: no recoverable legacy checkpoint");
    std::array<uint8_t, lsmt::kSpace> raw{};
    read_exact(pair->data_fd_, raw.data(), raw.size(), 0);
    const auto dh = lsmt::HeaderTrailer::parse(raw.data());
    if (dh.is_sealed() || sealed_trailer(pair->data_fd_, data_bytes))
        throw error(EALREADY, "lsmt rw: sealed data cannot migrate to writable");
    read_exact(pair->data_fd_, raw.data(), raw.size(), data_bytes - lsmt::kSpace);
    const auto tail = lsmt::HeaderTrailer::parse(raw.data());
    if (tail.flags != 2 || tail.version != 1 || tail.sub_version != 1 ||
        tail.uuid != dh.uuid || tail.parent_uuid != dh.parent_uuid ||
        tail.virtual_size != dh.virtual_size || tail.index_offset < lsmt::kSpace ||
        tail.index_offset % lsmt::kAlignment != 0 ||
        tail.index_offset > data_bytes - lsmt::kSpace ||
        tail.index_size > lsmt::kMaxRoIndexSize ||
        tail.index_size * 16 > data_bytes - lsmt::kSpace - tail.index_offset)
        throw format_error("lsmt rw: invalid legacy checkpoint trailer");
    auto ih = dh; ih.flags = 1; ih.index_offset = lsmt::kSpace;
    validate_pair(dh, ih, data_bytes, lsmt::kSpace);
    Replay replay(dh.virtual_size, tail.index_offset);
    uint64_t previous = 0;
    for (uint64_t offset = 0; offset < tail.index_size * 16;) {
        const auto count = static_cast<size_t>(std::min<uint64_t>(raw.size(), tail.index_size * 16 - offset));
        read_exact(pair->data_fd_, raw.data(), count, tail.index_offset + offset);
        for (size_t pos = 0; pos < count; pos += 16) {
            const auto m = bytes::load_segment_le(raw.data() + pos);
            if (m.offset == bytes::segment_mapping::kInvalidOffset) continue;
            if (m.offset < previous || (m.zeroed &&
                (m.moffset < lsmt::kDataStartSector || m.moffset > tail.index_offset / lsmt::kAlignment)))
                throw format_error("lsmt rw: invalid legacy checkpoint mapping");
            previous = m.end();
        }
        replay.append(std::span(raw.data(), count)); offset += count;
    }
    const auto live = replay.finish();
    migration_boundary(1);
    const auto temporary = pair->index_name_ + ".migrate." + generate_uuid();
    const int fd = ::openat(pair->directory_fd_, temporary.c_str(),
                            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) throw error(errno, "lsmt rw: create migration index");
    struct Cleanup {
        int fd, directory; const std::string& name;
        ~Cleanup() { if (fd >= 0) ::close(fd); ::unlinkat(directory, name.c_str(), 0); }
    } cleanup{fd, pair->directory_fd_, temporary};
    lock(fd);
    if (::fdatasync(pair->data_fd_) != 0) throw error(errno, "lsmt rw: sync legacy payload");
    ih.serialize(raw.data()); write_exact(fd, raw.data(), raw.size(), 0);
    uint64_t output_offset = lsmt::kSpace;
    size_t buffered = 0;
    for (const auto& m : live) {
        bytes::store_segment_le(raw.data() + buffered, m); buffered += 16;
        if (buffered == raw.size()) {
            write_exact(fd, raw.data(), buffered, output_offset);
            output_offset += buffered; buffered = 0;
        }
    }
    if (buffered) write_exact(fd, raw.data(), buffered, output_offset);
    if (::fdatasync(fd) != 0) throw error(errno, "lsmt rw: sync migrated index");
    migration_boundary(2);
    if (::linkat(pair->directory_fd_, temporary.c_str(), pair->directory_fd_, pair->index_name_.c_str(), 0) != 0)
        throw error(errno, "lsmt rw: publish migrated index without replacement");
    pair->index_fd_ = std::exchange(cleanup.fd, -1);
    migration_boundary(3);
    if (::fsync(pair->directory_fd_) != 0) throw error(errno, "lsmt rw: sync migrated index name");
    migration_boundary(4);
    // No tail truncation: retain the old checkpoint as unreferenced payload.
    // open_sync chooses the full EOF as the next append position.
    return open_sync(data, index, std::move(pair));
}

elio::coro::task<std::unique_ptr<FilePair>> FilePair::migrate_checkpoint(
    std::string data_path, std::string index_path) {
    auto request = std::make_shared<OpenRequest>(OpenRequest{std::move(data_path), std::move(index_path)});
    auto operation = [request] { return migrate_sync(request->data, request->index); };
    co_return co_await elio::spawn_blocking(std::move(operation));
}

elio::coro::task<int> FilePair::append(bytes::segment_mapping mapping) {
    if (poisoned_) co_return -EIO;
    if ((index_bytes_ - lsmt::kSpace) / 16 >= kMaxLogRecords) co_return -EFBIG;
    std::array<uint8_t, 16> raw{};
    bytes::store_segment_le(raw.data(), mapping);
    const bool partial = io_failure(1);
    const auto encoded = std::as_bytes(std::span(raw));
    const auto result = co_await elio::io::pwrite_exactly(
        index_fd_, partial ? encoded.first(8) : encoded, index_bytes_);
    if (partial || result.error || result.end != elio::io::transfer_end::complete) {
        poisoned_ = true;
        co_return result.error ? -result.error.value() : -EIO;
    }
    index_bytes_ += raw.size();
    co_return 0;
}

elio::coro::task<int> FilePair::flush() {
    if (poisoned_) co_return -EIO;
    int stage = 2;
    for (const int fd : {data_fd_, index_fd_}) {
        if (io_failure(stage++)) co_return -EIO;
        const auto status = co_await elio::io::sync_file(fd, elio::io::file_sync_mode::data_only);
        if (!status) co_return -status.error_value();
    }
    if (io_failure(4)) co_return -EIO;
    const int directory = directory_fd_;
    auto operation = [directory] { return ::fsync(directory) == 0 ? 0 : -errno; };
    co_return co_await elio::spawn_blocking(std::move(operation));
}

int FilePair::compact_sync(const std::vector<bytes::segment_mapping>& live) {
    if (poisoned_) return -EIO;
    if (live.size() > lsmt::kMaxRoIndexSize) return -EFBIG;
    const std::string temporary = index_name_ + ".compacting." + generate_uuid();
    const int fd = ::openat(directory_fd_, temporary.c_str(),
                            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -errno;
    struct Cleanup {
        int fd, directory; const std::string& name; bool published = false;
        ~Cleanup() { if (fd >= 0) ::close(fd); if (!published) ::unlinkat(directory, name.c_str(), 0); }
    } cleanup{fd, directory_fd_, temporary};
    try {
        lock(fd);
        Replay replay(header_.virtual_size, file_size(data_fd_));
        std::array<uint8_t, 16> encoded{};
        uint64_t previous = 0;
        for (const auto& m : live) {
            if (m.offset < previous) return -EINVAL;
            previous = m.end();
            bytes::store_segment_le(encoded.data(), m);
            replay.append(encoded);
        }
        (void)replay.finish();
        // Data must precede the compacted log's durable publication.
        if (::fdatasync(data_fd_) != 0) return -errno;
        if (io_failure(5)) return -EIO;
        auto ih = header_; ih.flags = 1; ih.index_offset = lsmt::kSpace; ih.index_size = 0;
        std::array<uint8_t, lsmt::kSpace> raw{}; ih.serialize(raw.data());
        write_exact(fd, raw.data(), raw.size(), 0);
        uint64_t offset = lsmt::kSpace;
        for (const auto& m : live) {
            bytes::store_segment_le(raw.data(), m);
            write_exact(fd, raw.data(), 16, offset); offset += 16;
        }
        if (::fdatasync(fd) != 0) return -errno;
        if (io_failure(6)) return -EIO;
        if (::renameat(directory_fd_, temporary.c_str(), directory_fd_, index_name_.c_str()) != 0)
            return -errno;
        cleanup.published = true;
        ::flock(index_fd_, LOCK_UN); ::close(index_fd_);
        index_fd_ = fd; cleanup.fd = -1; index_bytes_ = offset;
        if (io_failure(7)) return -EIO;
        return ::fsync(directory_fd_) == 0 ? 0 : -errno;
    } catch (const error& e) { return -e.errno_value(); }
}

elio::coro::task<int> FilePair::compact(std::vector<bytes::segment_mapping> live) {
    auto request = std::make_shared<CompactRequest>(CompactRequest{this, std::move(live)});
    auto operation = [request] { return request->pair->compact_sync(request->live); };
    co_return co_await elio::spawn_blocking(std::move(operation));
}

bool FilePair::recover_grow() {
    const auto name = index_name_ + ".grow";
    const int intent = ::openat(directory_fd_, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (intent < 0) {
        if (errno == ENOENT) return false;
        throw error(errno, "lsmt rw: open grow intent");
    }
    struct Close { int fd; ~Close() { ::close(fd); } } close{intent};
    if (file_size(intent) != kGrowBytes) throw format_error("lsmt rw: truncated grow intent");
    std::array<uint8_t, kGrowBytes> raw{};
    read_exact(intent, raw.data(), raw.size(), 0);
    if (bytes::load_u64_le(raw.data()) != kGrowMagic ||
        bytes::load_u32_le(raw.data() + raw.size() - 4) != crc32::crc32c(raw.data(), raw.size() - 4))
        throw format_error("lsmt rw: invalid grow intent checksum");
    const auto old_size = bytes::load_u64_le(raw.data() + 8);
    const auto target = lsmt::HeaderTrailer::parse(raw.data() + 16);
    const auto target_index = lsmt::HeaderTrailer::parse(raw.data() + 16 + lsmt::kSpace);
    if (old_size == 0 || old_size % lsmt::kAlignment != 0 || target.virtual_size <= old_size)
        throw format_error("lsmt rw: invalid grow intent capacity");
    const auto data_size = file_size(data_fd_) / lsmt::kAlignment * lsmt::kAlignment;
    const auto index_size = file_size(index_fd_);
    if (index_size < lsmt::kSpace) throw format_error("lsmt rw: truncated grow index");
    const auto complete_index = lsmt::kSpace + (index_size - lsmt::kSpace) / 16 * 16;
    validate_pair(target, target_index, data_size, complete_index);
    // Only virtual_size can differ during this transaction. Compare every
    // parsed field of both live headers before permitting any rewrite.
    std::array<uint8_t, lsmt::kSpace> current{}, normalized{}, wanted{};
    for (int part = 0; part < 2; ++part) {
        read_exact(part == 0 ? data_fd_ : index_fd_, current.data(), current.size(), 0);
        auto header = lsmt::HeaderTrailer::parse(current.data());
        // A short forward write can leave a prefix of the new capacity and a
        // suffix of the old one. Do not accept arbitrary capacity changes or
        // let an obsolete intent shrink a subsequently grown artifact.
        bool capacity_matches = false;
        for (unsigned prefix = 0; prefix <= 8; ++prefix) {
            const uint64_t mask = prefix == 8 ? UINT64_MAX : ((uint64_t{1} << (8 * prefix)) - 1);
            if (header.virtual_size == ((target.virtual_size & mask) | (old_size & ~mask)))
                capacity_matches = true;
        }
        if (!capacity_matches) throw format_error("lsmt rw: stale grow intent capacity");
        header.virtual_size = target.virtual_size;
        header.serialize(normalized.data());
        (part == 0 ? target : target_index).serialize(wanted.data());
        if (normalized != wanted) throw format_error("lsmt rw: grow intent identity mismatch");
    }
    Replay replay(target.virtual_size, data_size);
    for (uint64_t offset = lsmt::kSpace; offset < complete_index;) {
        const auto count = static_cast<size_t>(std::min<uint64_t>(current.size(), complete_index - offset));
        read_exact(index_fd_, current.data(), count, offset);
        replay.append(std::span(current.data(), count)); offset += count;
    }
    (void)replay.finish();
    // After publication the transaction rolls forward. Preserve target state
    // even when a later directory sync fails, so a same-instance retry works.
    growth_pending_ = true;
    poisoned_ = true;
    header_ = target;
    write_exact(data_fd_, raw.data() + 16, lsmt::kSpace, 0);
    grow_boundary(2);
    write_exact(index_fd_, raw.data() + 16 + lsmt::kSpace, lsmt::kSpace, 0);
    grow_boundary(3);
    if (::fdatasync(data_fd_) != 0 || ::fdatasync(index_fd_) != 0)
        throw error(errno, "lsmt rw: sync grown headers");
    grow_boundary(4);
    if (::unlinkat(directory_fd_, name.c_str(), 0) != 0)
        throw error(errno, "lsmt rw: retire grow intent");
    grow_boundary(5);
    if (::fsync(directory_fd_) != 0) throw error(errno, "lsmt rw: sync grow retirement");
    growth_pending_ = false;
    poisoned_ = false;
    return true;
}

int FilePair::grow(uint64_t virtual_size) {
    if (poisoned_ && !growth_pending_) return -EIO;
    try {
        if (growth_pending_) {
            recover_grow();
            if (::fsync(directory_fd_) != 0) return -errno;
            growth_pending_ = false; poisoned_ = false;
        }
        if (virtual_size == 0 || virtual_size % lsmt::kAlignment != 0 ||
            virtual_size / lsmt::kAlignment > bytes::segment_mapping::kMaxOffset ||
            virtual_size < header_.virtual_size) return -EINVAL;
        if (virtual_size == header_.virtual_size) return 0;
        std::array<uint8_t, kGrowBytes> raw{};
        bytes::store_u64_le(raw.data(), kGrowMagic);
        bytes::store_u64_le(raw.data() + 8, header_.virtual_size);
        auto target = header_; target.virtual_size = virtual_size;
        target.serialize(raw.data() + 16);
        std::array<uint8_t, lsmt::kSpace> current_index{};
        read_exact(index_fd_, current_index.data(), current_index.size(), 0);
        auto ih = lsmt::HeaderTrailer::parse(current_index.data());
        validate_pair(header_, ih, file_size(data_fd_), index_bytes_);
        ih.virtual_size = virtual_size;
        ih.serialize(raw.data() + 16 + lsmt::kSpace);
        bytes::store_u32_le(raw.data() + raw.size() - 4, crc32::crc32c(raw.data(), raw.size() - 4));
        const auto name = index_name_ + ".grow";
        const auto temporary = name + "." + generate_uuid();
        const int fd = ::openat(directory_fd_, temporary.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) return -errno;
        struct Cleanup {
            int fd, directory; const std::string& name;
            ~Cleanup() { ::close(fd); ::unlinkat(directory, name.c_str(), 0); }
        } cleanup{fd, directory_fd_, temporary};
        write_exact(fd, raw.data(), raw.size(), 0);
        if (::fdatasync(fd) != 0) return -errno;
        // linkat provides no-replace publication: never overwrite an unknown
        // transaction left in the workspace.
        if (::linkat(directory_fd_, temporary.c_str(), directory_fd_, name.c_str(), 0) != 0)
            return -errno;
        growth_pending_ = true; poisoned_ = true;
        if (::fsync(directory_fd_) != 0) return -errno;
        grow_boundary(1);
        recover_grow();
        return 0;
    } catch (const error& e) { return -e.errno_value(); }
}
}  // namespace obd::format::lsmt_rw
