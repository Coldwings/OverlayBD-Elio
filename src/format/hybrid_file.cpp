#include "format/hybrid_file.hpp"
#include "format/hybrid_transaction.hpp"

#include "common/errors.hpp"
#include "common/crc32c.hpp"
#include "format/writer.hpp"
#include <elio/io/file_transfer.hpp>
#include <elio/io/file_operations.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/runtime/spawn_blocking.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace obd::format::hybrid {
namespace {
#ifdef OBD_TEST_HOOKS
std::atomic<int> creation_fault_stage{-1};
std::atomic<bool> creation_fault_exits{false};
#endif
void creation_boundary(int stage) {
#ifdef OBD_TEST_HOOKS
    if (creation_fault_stage.load() == stage) {
        if (creation_fault_exits.load()) ::_exit(90 + stage);
        throw error(EIO, "hybrid: injected creation failure");
    }
#else
    (void)stage;
#endif
}

elio::coro::task<void> read_exact(int fd, std::span<uint8_t> bytes, uint64_t offset) {
    const auto result = co_await elio::io::pread_exactly(
        fd, std::as_writable_bytes(bytes), offset);
    if (result.error) throw error(result.error.value(), "hybrid: artifact read failed");
    if (result.end != elio::io::transfer_end::complete)
        throw error(EIO, "hybrid: short artifact read");
}

struct OpenRequest {
    std::string data_path;
    std::string index_path;
    bool writable;
};
struct LogRequest {
    FilePair* pair;
    int operation;
    std::array<uint8_t, 16> record;
};
struct SealRequest {
    FilePair* pair;
    std::string user_tag;
};
struct ReplaceRequest {
    FilePair* pair;
    std::vector<bytes::segment_mapping> mappings;
};
struct ReplaceResult {
    int rc = 0;
    std::exception_ptr error;
};
struct PendingIndex {
    int directory;
    std::string name;
    bool created = false;
    bool published = false;
    ~PendingIndex() {
        if (created && !published) ::unlinkat(directory, name.c_str(), 0);
    }
};
struct CreateRequest {
    std::string directory;
    uint64_t virtual_size;
};
struct Fd {
    Fd() = default;
    explicit Fd(int fd) : value(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int value = -1;
    ~Fd() { if (value >= 0) ::close(value); }
    int release() { return std::exchange(value, -1); }
};
struct PendingDirectory {
    int parent;
    std::string name;
    Fd directory;
    bool created = false;
    bool published = false;
    ~PendingDirectory() {
        if (!created || published) return;
        if (directory.value >= 0) {
            ::unlinkat(directory.value, "data", 0);
            ::unlinkat(directory.value, "index", 0);
        }
        ::unlinkat(parent, name.c_str(), AT_REMOVEDIR);
    }
};
void write_header_sync(int fd, const lsmt::HeaderTrailer& header) {
    std::array<uint8_t, lsmt::kSpace> bytes{};
    header.serialize(bytes.data());
    size_t done = 0;
    while (done < bytes.size()) {
        const auto written = ::pwrite(fd, bytes.data() + done, bytes.size() - done,
                                      static_cast<off_t>(done));
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) throw_errno(written < 0 ? errno : EIO, "hybrid: initialize header");
        done += static_cast<size_t>(written);
    }
    if (::fsync(fd) != 0) throw_errno(errno, "hybrid: sync initialized artifact");
}
struct OpenResult {
    std::unique_ptr<FilePair> pair;
    std::exception_ptr error;
};
}

#ifdef OBD_TEST_HOOKS
std::atomic<int> FilePair::replace_fail_stage_{-1};
void creation_fault_for_test(int stage, bool exit_process) {
    creation_fault_exits.store(exit_process);
    creation_fault_stage.store(stage);
}
#endif

FilePair::~FilePair() {
    // Callers drain operations before destruction. End pair ownership now:
    // Elio's asynchronous close may otherwise keep the locks past destruction.
    for (const int fd : {index_fd_, data_fd_}) {
        if (fd >= 0) {
            while (::flock(fd, LOCK_UN) != 0 && errno == EINTR) {}
        }
    }
    if (index_directory_fd_ >= 0) elio::io::close_fd_for_destructor(index_directory_fd_);
    if (index_fd_ >= 0) elio::io::close_fd_for_destructor(index_fd_);
    if (data_fd_ >= 0) elio::io::close_fd_for_destructor(data_fd_);
}

std::unique_ptr<FilePair> FilePair::open_descriptors(
    const std::string& data_path, const std::string& index_path, bool writable) {
    auto pair = std::unique_ptr<FilePair>(new FilePair);
    pair->writable_ = writable;
    const int flags = (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NONBLOCK;
    pair->data_fd_ = ::open(data_path.c_str(), flags);
    if (pair->data_fd_ < 0) throw_errno(errno, "hybrid: open data artifact");
    if (::flock(pair->data_fd_, (writable ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0)
        throw_errno(errno, "hybrid: data artifact already owned");
    const std::filesystem::path index_location(index_path);
    auto index_parent = index_location.parent_path();
    if (index_parent.empty()) index_parent = ".";
    pair->index_name_ = index_location.filename().string();
    pair->index_directory_fd_ = ::open(index_parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (pair->index_directory_fd_ < 0) throw_errno(errno, "hybrid: open index directory");
    pair->index_fd_ = ::openat(pair->index_directory_fd_, pair->index_name_.c_str(), flags);
    if (pair->index_fd_ < 0) throw_errno(errno, "hybrid: open index artifact");
    if (::flock(pair->index_fd_, (writable ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0)
        throw_errno(errno, "hybrid: index artifact already owned");
    struct stat data{}, index{};
    if (::fstat(pair->data_fd_, &data) != 0 || ::fstat(pair->index_fd_, &index) != 0)
        throw_errno(errno, "hybrid: stat artifact pair");
    if (!S_ISREG(data.st_mode) || !S_ISREG(index.st_mode) ||
        data.st_size < lsmt::kSpace || index.st_size < lsmt::kSpace ||
        (data.st_dev == index.st_dev && data.st_ino == index.st_ino))
        throw format_error("hybrid: invalid artifact pair files");
    (void)recover_transaction(pair->data_fd_, pair->index_fd_, pair->index_directory_fd_,
                              pair->index_name_ + ".txn", writable);
    if (writable) {
        // Also repair the namespace durability of a fully published creation
        // whose final parent sync previously failed.
        if (::fsync(pair->index_directory_fd_) != 0)
            throw_errno(errno, "hybrid: sync opened pair directory");
        Fd parent{::openat(pair->index_directory_fd_, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
        if (parent.value < 0 || ::fsync(parent.value) != 0)
            throw_errno(errno, "hybrid: sync opened pair parent");
    }
    if (::fstat(pair->data_fd_, &data) != 0 || ::fstat(pair->index_fd_, &index) != 0)
        throw_errno(errno, "hybrid: stat recovered pair");
    if (is_sealed_pair(pair->data_fd_, pair->index_fd_))
        throw error(EALREADY, "hybrid: artifact is already sealed");
    pair->data_bytes_ = static_cast<uint64_t>(data.st_size);
    pair->index_bytes_ = static_cast<uint64_t>(index.st_size);
    return pair;
}

std::unique_ptr<FilePair> FilePair::create_sync(
    const std::string& directory, uint64_t virtual_size) {
    if (virtual_size == 0 || virtual_size % lsmt::kAlignment != 0 ||
        virtual_size / lsmt::kAlignment > bytes::segment_mapping::kMaxOffset)
        throw error(EINVAL, "hybrid: invalid creation size");
    const std::filesystem::path destination(directory);
    const auto filename = destination.filename().string();
    if (filename.empty() || filename == "." || filename == "..")
        throw error(EINVAL, "hybrid: invalid pair directory");
    auto parent_path = destination.parent_path();
    if (parent_path.empty()) parent_path = ".";
    Fd parent{::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (parent.value < 0) throw_errno(errno, "hybrid: open pair parent directory");
    PendingDirectory pending{parent.value, ".hybrid-init-" + generate_uuid(), {}, false, false};
    if (::mkdirat(parent.value, pending.name.c_str(), 0700) != 0)
        throw_errno(errno, "hybrid: create staging directory");
    pending.created = true;
    pending.directory.value = ::openat(parent.value, pending.name.c_str(),
                                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (pending.directory.value < 0) throw_errno(errno, "hybrid: open staging directory");
    Fd data{::openat(pending.directory.value, "data", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
    if (data.value < 0) throw_errno(errno, "hybrid: create data artifact");
    Fd index{::openat(pending.directory.value, "index", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
    if (index.value < 0) throw_errno(errno, "hybrid: create index artifact");
    if (::flock(data.value, LOCK_EX | LOCK_NB) != 0 ||
        ::flock(index.value, LOCK_EX | LOCK_NB) != 0)
        throw_errno(errno, "hybrid: lock initialized pair");
    lsmt::HeaderTrailer header;
    header.flags = kHybridFlag | 3u;
    header.virtual_size = virtual_size;
    header.uuid = generate_uuid();
    header.parent_uuid = "00000000-0000-0000-0000-000000000000";
    write_header_sync(data.value, header);
    creation_boundary(1);
    auto index_header = header;
    index_header.flags = kHybridFlag | 1u;
    index_header.index_offset = lsmt::kSpace;
    write_header_sync(index.value, index_header);
    creation_boundary(2);
    if (::fsync(pending.directory.value) != 0)
        throw_errno(errno, "hybrid: sync initialized directory");
    creation_boundary(3);
    // Allocate and initialize the return value before the publication boundary.
    auto pair = std::unique_ptr<FilePair>(new FilePair);
    pair->writable_ = true;
    pair->header_ = std::move(header);
    pair->data_bytes_ = pair->index_bytes_ = lsmt::kSpace;
    pair->index_name_ = "index";
    pair->index_directory_fd_ = ::fcntl(pending.directory.value, F_DUPFD_CLOEXEC, 0);
    if (pair->index_directory_fd_ < 0) throw_errno(errno, "hybrid: retain index directory");
    pair->data_fd_ = data.release();
    pair->index_fd_ = index.release();
    pair->log_stamp_ = activate_log_state(pair->data_fd_, pair->index_fd_,
                                         pair->index_directory_fd_, std::nullopt);
    creation_boundary(4);
    if (::syscall(SYS_renameat2, parent.value, pending.name.c_str(),
                  parent.value, filename.c_str(), RENAME_NOREPLACE) != 0)
        throw_errno(errno, "hybrid: publish initialized directory");
    pending.published = true;
    creation_boundary(5);
    if (::fsync(parent.value) != 0)
        throw_errno(errno, "hybrid: sync pair parent directory");
    creation_boundary(6);
    return pair;
}

int FilePair::grow(uint64_t virtual_size) {
    if (!writable_) return -EROFS;
    if (virtual_size == 0 || virtual_size % 512 != 0 ||
        virtual_size / 512 > bytes::segment_mapping::kMaxOffset ||
        virtual_size < header_.virtual_size) return -EINVAL;
    if (virtual_size == header_.virtual_size) return 0;
    try {
        grow_transaction(data_fd_, index_fd_, index_directory_fd_, index_name_ + ".txn", virtual_size);
        header_.virtual_size = virtual_size;
        return 0;
    } catch (const error& e) { return -e.errno_value(); }
}

int FilePair::seal_sync(const std::string& user_tag) {
    if (!writable_) return -EROFS;
    try {
        seal_transaction(data_fd_, index_fd_, index_directory_fd_, index_name_ + ".txn", user_tag);
        struct stat data{};
        if (::fstat(data_fd_, &data) != 0) return -errno;
        data_bytes_ = static_cast<uint64_t>(data.st_size);
        return 0;
    } catch (const error& e) { return -e.errno_value(); }
}

elio::coro::task<int> FilePair::seal(std::string user_tag) {
    auto* request = new SealRequest{this, std::move(user_tag)};
    ReplaceResult result;
    try {
        result = co_await elio::spawn_blocking([request]() noexcept -> ReplaceResult {
            std::unique_ptr<SealRequest> owned(request);
            ReplaceResult outcome;
            try { outcome.rc = owned->pair->seal_sync(owned->user_tag); }
            catch (...) { outcome.error = std::current_exception(); }
            return outcome;
        });
    } catch (...) {
        delete request;
        throw;
    }
    if (result.error) std::rethrow_exception(result.error);
    co_return result.rc;
}

int FilePair::replace_index_sync(const std::vector<bytes::segment_mapping>& mappings) {
    if (!writable_) return -EROFS;
    if (mappings.size() > lsmt::kMaxRoIndexSize) return -EFBIG;
    struct stat data{}, opened{}, named{};
    if (::fstat(data_fd_, &data) != 0 || ::fstat(index_fd_, &opened) != 0 ||
        ::fstatat(index_directory_fd_, index_name_.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0)
        return -errno;
    if (opened.st_dev != named.st_dev || opened.st_ino != named.st_ino) return -ESTALE;
    Replay verify(header_.virtual_size, static_cast<uint64_t>(data.st_size));
    uint64_t prior_end = 0;
    for (const auto& mapping : mappings) {
        if (mapping.offset < prior_end || mapping.offset > bytes::segment_mapping::kMaxOffset ||
            mapping.length == 0 || mapping.length > bytes::segment_mapping::kMaxLength ||
            mapping.moffset >= kNoPhysicalOffset || mapping.tag != 0)
            return -EINVAL;
        prior_end = mapping.end();
        std::array<uint8_t, 16> raw{};
        bytes::store_segment_le(raw.data(), mapping);
        verify.append(raw);
    }
    for (const auto& mapping : verify.finish()) {
        if (mapping.moffset == kNoPhysicalOffset) return -EINVAL;
    }
    if (::fdatasync(data_fd_) != 0) return -errno;
    PendingIndex pending{index_directory_fd_, ".hybrid-index-" + generate_uuid(), false, false};
    Fd next{::openat(index_directory_fd_, pending.name.c_str(),
                    O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
    if (next.value < 0) return -errno;
    pending.created = true;
    if (::flock(next.value, LOCK_EX | LOCK_NB) != 0) return -errno;
    auto index_header = header_;
    index_header.flags = kHybridFlag | 1u;
    index_header.index_offset = lsmt::kSpace;
    index_header.index_size = 0;
    write_header_sync(next.value, index_header);
    std::array<uint8_t, 64 * 1024> chunk{};
    size_t used = 0;
    uint64_t offset = lsmt::kSpace;
    auto write_chunk = [&]() -> int {
        size_t done = 0;
        while (done < used) {
            const auto written = ::pwrite(next.value, chunk.data() + done, used - done,
                                          static_cast<off_t>(offset + done));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return written < 0 ? -errno : -EIO;
            done += static_cast<size_t>(written);
        }
        offset += used;
        used = 0;
        return 0;
    };
    for (const auto& mapping : mappings) {
        bytes::store_segment_le(chunk.data() + used, mapping);
        used += 16;
        if (used == chunk.size()) {
            const int rc = write_chunk();
            if (rc != 0) return rc;
        }
    }
    if (const int rc = write_chunk(); rc != 0) return rc;
    const auto next_stamp = new_log_stamp(next.value);
    publish_log_progress(next.value, next_stamp);
    // Settle a previous post-rename failure before announcing another pair of
    // generations; otherwise the earlier namespace change could roll back.
    if (::fsync(index_directory_fd_) != 0) return -errno;
    if (!log_stamp_) throw format_error("hybrid: missing active log state");
    prepare_log_replacement(data_fd_, index_fd_, next.value, *log_stamp_, next_stamp);
#ifdef OBD_TEST_HOOKS
    if (replace_fail_stage_.load() == 1) return -EIO;
#endif
    if (::renameat(index_directory_fd_, pending.name.c_str(),
                   index_directory_fd_, index_name_.c_str()) != 0) return -errno;
    pending.published = true;
    // No throwing work may separate publication from descriptor adoption.
    const int old = std::exchange(index_fd_, next.release());
    index_bytes_ = offset;
    log_stamp_ = next_stamp;
    ++index_generation_;
    ::close(old);  // Caller serializes all index operations around this job.
#ifdef OBD_TEST_HOOKS
    if (replace_fail_stage_.load() == 2) return -EIO;
#endif
    if (::fsync(index_directory_fd_) != 0) return -errno;
#ifdef OBD_TEST_HOOKS
    if (replace_fail_stage_.load() == 3) throw error(EIO, "hybrid: injected log anchor failure");
    if (replace_fail_stage_.load() == 4) throw std::bad_alloc();
#endif
    publish_log_commit(data_fd_, *log_stamp_);
    return 0;
}

elio::coro::task<int> FilePair::replace_index(std::vector<bytes::segment_mapping> mappings) {
    auto* request = new ReplaceRequest{this, std::move(mappings)};
    ReplaceResult result;
    try {
        result = co_await elio::spawn_blocking([request]() noexcept -> ReplaceResult {
            std::unique_ptr<ReplaceRequest> owned(request);
            ReplaceResult outcome;
            try { outcome.rc = owned->pair->replace_index_sync(owned->mappings); }
            catch (const error& e) { outcome.rc = -e.errno_value(); }
            catch (...) { outcome.error = std::current_exception(); }
            return outcome;
        });
    } catch (...) {
        delete request;
        throw;
    }
    if (result.error) std::rethrow_exception(result.error);
    co_return result.rc;
}

void FilePair::log_operation_sync(int operation, const std::array<uint8_t, 16>& record) {
    if (operation == 0) {
        log_stamp_ = inspect_log_state(data_fd_, index_fd_);
        if (log_stamp_) index_bytes_ = log_stamp_->bytes;
    } else if (operation == 1) {
        log_stamp_ = activate_log_state(data_fd_, index_fd_, index_directory_fd_, log_stamp_);
        index_bytes_ = log_stamp_->bytes;
    } else {
        if (!log_stamp_) throw format_error("hybrid: missing active log stamp");
        if (operation == 2) {
            auto next = *log_stamp_;
            if ((next.bytes - lsmt::kSpace) / 16 >= kMaxLogRecords)
                throw error(ENOSPC, "hybrid: log progress limit");
            next.bytes += record.size();
            next.crc = crc32::crc32c_extend(record.data(), record.size(), next.crc);
            publish_log_progress(index_fd_, next);
            log_stamp_ = next;
            index_bytes_ = next.bytes;
        } else {
            publish_log_commit(data_fd_, *log_stamp_);
        }
    }
}
elio::coro::task<void> FilePair::log_operation(int operation, std::array<uint8_t, 16> record) {
    auto* request = new LogRequest{this, operation, record};
    std::exception_ptr failure;
    try {
        failure = co_await elio::spawn_blocking([request]() noexcept -> std::exception_ptr {
            std::unique_ptr<LogRequest> owned(request);
            try { owned->pair->log_operation_sync(owned->operation, owned->record); }
            catch (...) { return std::current_exception(); }
            return {};
        });
    } catch (...) {
        delete request;
        throw;
    }
    if (failure) std::rethrow_exception(failure);
}
elio::coro::task<int> FilePair::append_log_progress(std::array<uint8_t, 16> record) {
    try { co_await log_operation(2, record); }
    catch (const error& e) { co_return -e.errno_value(); }
    co_return 0;
}
elio::coro::task<int> FilePair::commit_log_state() {
    try { co_await log_operation(3); }
    catch (const error& e) { co_return -e.errno_value(); }
    co_return 0;
}

elio::coro::task<int> FilePair::sync_index_directory() {
    const auto result = co_await elio::io::sync_file(
        index_directory_fd_, elio::io::file_sync_mode::data_and_metadata);
    co_return result ? 0 : -result.error_value();
}

elio::coro::task<std::unique_ptr<FilePair>> FilePair::create(
    std::string directory, uint64_t virtual_size) {
    auto* request = new CreateRequest{std::move(directory), virtual_size};
    OpenResult opened;
    try {
        opened = co_await elio::spawn_blocking([request]() noexcept -> OpenResult {
            std::unique_ptr<CreateRequest> owned(request);
            OpenResult result;
            try {
                result.pair = FilePair::create_sync(owned->directory, owned->virtual_size);
            } catch (...) { result.error = std::current_exception(); }
            return result;
        });
    } catch (...) {
        delete request;
        throw;
    }
    if (opened.error) std::rethrow_exception(opened.error);
    co_return std::move(opened.pair);
}

elio::coro::task<std::unique_ptr<FilePair>> FilePair::open_existing(
    std::string data_path, std::string index_path, bool writable) {
    // The worker takes ownership only after blocking-pool admission. Capturing
    // one pointer avoids a nontrivial temporary closure spanning co_await on
    // GCC 12. Worker exceptions are returned separately from admission errors.
    auto* request = new OpenRequest{std::move(data_path), std::move(index_path), writable};
    OpenResult opened;
    try {
        opened = co_await elio::spawn_blocking([request]() noexcept -> OpenResult {
            std::unique_ptr<OpenRequest> owned(request);
            OpenResult result;
            try {
                result.pair = FilePair::open_descriptors(
                    owned->data_path, owned->index_path, owned->writable);
            } catch (...) {
                result.error = std::current_exception();
            }
            return result;
        });
    } catch (...) {
        delete request;
        throw;
    }
    if (opened.error) std::rethrow_exception(opened.error);
    auto pair = std::move(opened.pair);
    std::array<uint8_t, lsmt::kSpace> data_header{}, index_header{};
    co_await read_exact(pair->data_fd_, data_header, 0);
    co_await read_exact(pair->index_fd_, index_header, 0);
    pair->header_ = lsmt::HeaderTrailer::parse(data_header.data());
    const auto index = lsmt::HeaderTrailer::parse(index_header.data());
    (void)validate_pair(pair->header_, index, pair->data_bytes_, pair->index_bytes_);
    co_await pair->log_operation(0);
    Replay replay(pair->header_.virtual_size, pair->data_bytes_);
    std::array<uint8_t, 64 * 1024> chunk{};
    uint64_t offset = lsmt::kSpace;
    while (offset < pair->index_bytes_) {
        const auto count = static_cast<size_t>(std::min<uint64_t>(
            chunk.size(), pair->index_bytes_ - offset));
        auto bytes = std::span(chunk).first(count);
        co_await read_exact(pair->index_fd_, bytes, offset);
        replay.append(bytes);
        offset += count;
    }
    pair->mappings_ = replay.finish();
    if (writable) co_await pair->log_operation(1);
    co_return pair;
}

}  // namespace obd::format::hybrid
