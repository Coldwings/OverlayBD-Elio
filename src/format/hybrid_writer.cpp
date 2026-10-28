#include "format/hybrid_writer.hpp"
#include "format/hybrid_io_fault.hpp"
#include "common/errors.hpp"
#include <elio/io/file_transfer.hpp>
#include <elio/io/file_operations.hpp>
#include <elio/io/io_awaitables.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace obd::format::hybrid {
namespace {
#ifdef OBD_TEST_HOOKS
std::atomic<int> io_fault_stage{-1};
#endif
bool io_fault(int stage) {
#ifdef OBD_TEST_HOOKS
    return io_fault_stage.load() == stage;
#else
    (void)stage;
    return false;
#endif
}
using Mapping = bytes::segment_mapping;
using Unlock = elio::sync::unique_lock_guard;
elio::coro::task<int> write_exact(int fd, const void* data, size_t size, uint64_t offset) {
    const auto result = co_await elio::io::pwrite_exactly(fd,
        std::span<const std::byte>(static_cast<const std::byte*>(data), size), offset);
    if (result.error) co_return -result.error.value();
    co_return result.end == elio::io::transfer_end::complete ? 0 : -EIO;
}
elio::coro::task<int> sync(int fd) {
    const auto result = co_await elio::io::sync_file(fd, elio::io::file_sync_mode::data_only);
    co_return result ? 0 : -result.error_value();
}
std::vector<Mapping> replace(const std::vector<Mapping>& old, Mapping update) {
    std::vector<Mapping> next;
    next.reserve(old.size() + 2);
    for (auto m : old) {
        if (m.end() <= update.offset || m.offset >= update.end()) {
            next.push_back(m);
            continue;
        }
        if (m.offset < update.offset) {
            auto left = m;
            left.length = static_cast<uint32_t>(update.offset - m.offset);
            next.push_back(left);
        }
        if (m.end() > update.end()) {
            const auto delta = update.end() - m.offset;
            m.offset += delta;
            m.length -= static_cast<uint32_t>(delta);
            if (m.moffset != kNoPhysicalOffset) m.moffset += delta;
            next.push_back(m);
        }
    }
    next.push_back(update);
    std::sort(next.begin(), next.end(), [](const Mapping& a, const Mapping& b) {
        return a.offset < b.offset;
    });
    if (next.size() > lsmt::kMaxRoIndexSize)
        throw error(ENOSPC, "hybrid: live mapping limit reached");
    return next;
}
}

#ifdef OBD_TEST_HOOKS
void writer_io_fault_for_test(int stage) { io_fault_stage.store(stage); }
#endif

Writer::Writer(std::unique_ptr<FilePair> pair)
    : pair_(std::move(pair)), data_end_(0), index_end_(0) {
    if (!pair_ || !pair_->writable())
        throw error(EINVAL, "hybrid: writer requires an exclusively opened pair");
    virtual_size_.store(pair_->header().virtual_size);
    mappings_ = pair_->mappings();
    data_end_ = pair_->data_bytes();
    index_end_ = pair_->index_bytes();
}

std::vector<Mapping> Writer::snapshot() const {
    std::lock_guard lock(index_mutex_);
    if (failed_) throw error(EIO, "hybrid: uncertain IO requires recovery");
    return mappings_;
}

elio::coro::task<ssize_t> Writer::pread(void* buffer, size_t count, uint64_t offset) {
    if (offset % 512 != 0 || count % 512 != 0) co_return -EINVAL;
    co_await operation_.lock_shared();
    elio::sync::shared_lock_guard unlock{operation_};
#ifdef OBD_TEST_HOOKS
    if (read_hook_for_test) co_await read_hook_for_test();
#endif
    {
        std::lock_guard lock(index_mutex_);
        if (failed_) co_return -EIO;
    }
    if (offset >= virtual_size()) co_return 0;
    count = static_cast<size_t>(std::min<uint64_t>(count, virtual_size() - offset));
    if (count == 0) co_return 0;
    auto* output = static_cast<uint8_t*>(buffer);
    std::memset(output, 0, count);
    const auto start = offset / 512, end = (offset + count) / 512;
    for (const auto& m : mappings_) {
        if (m.end() <= start) continue;
        if (m.offset >= end) break;
        if (m.zeroed) continue;
        const auto first = std::max(start, m.offset), last = std::min(end, m.end());
        const auto size = static_cast<size_t>((last - first) * 512);
        const auto result = co_await elio::io::pread_exactly(pair_->data_fd(),
            std::span<std::byte>(reinterpret_cast<std::byte*>(output + (first - start) * 512), size),
            (m.moffset + first - m.offset) * 512);
        if (result.error) co_return -result.error.value();
        if (result.end != elio::io::transfer_end::complete) co_return -EIO;
    }
    co_return static_cast<ssize_t>(count);
}

elio::coro::task<ssize_t> Writer::pread_physical(void* buffer, size_t count, uint64_t offset) {
    co_await operation_.lock_shared();
    elio::sync::shared_lock_guard unlock{operation_};
#ifdef OBD_TEST_HOOKS
    if (read_hook_for_test) co_await read_hook_for_test();
#endif
    {
        std::lock_guard lock(index_mutex_);
        if (failed_) co_return -EIO;
    }
    const auto size = physical_size();
    if (offset >= size) co_return 0;
    count = static_cast<size_t>(std::min<uint64_t>(count, size - offset));
    if (count == 0) co_return 0;
    const auto result = co_await elio::io::pread_exactly(pair_->data_fd(),
        std::span<std::byte>(static_cast<std::byte*>(buffer), count), offset);
    if (result.error) co_return -result.error.value();
    if (result.end != elio::io::transfer_end::complete) co_return -EIO;
    co_return static_cast<ssize_t>(count);
}

elio::coro::task<ssize_t> Writer::pwrite(const void* buffer, size_t count, uint64_t offset) {
    co_return co_await modify(buffer, count, offset, false);
}
elio::coro::task<int> Writer::discard(uint64_t offset, uint64_t count) {
    if (count > SIZE_MAX) co_return -EINVAL;
    const auto result = co_await modify(nullptr, static_cast<size_t>(count), offset, true);
    co_return result < 0 ? static_cast<int>(result) : 0;
}

elio::coro::task<ssize_t> Writer::modify(
    const void* buffer, size_t count, uint64_t offset, bool zero) {
    if (offset % 512 != 0 || count % 512 != 0 || offset > virtual_size() ||
        count > virtual_size() - offset) co_return -EINVAL;
    co_await operation_.lock();
    Unlock unlock{operation_};
    {
        std::lock_guard lock(index_mutex_);
        if (failed_) co_return -EIO;
    }
    if (sealed_ || checkpointed_) co_return -EROFS;
    size_t done = 0;
    while (done < count) {
        const uint64_t threshold = std::max<uint64_t>(4096, 2 * mappings_.size() + 16);
        if ((index_end_ - lsmt::kSpace) / 16 >= threshold) {
            const int rc = co_await compact_locked();
            if (rc != 0) co_return rc;
        }
        const auto sector = (offset + done) / 512;
        auto it = std::upper_bound(mappings_.begin(), mappings_.end(), sector,
            [](uint64_t s, const Mapping& m) { return s < m.end(); });
        const bool covered = it != mappings_.end() && it->offset <= sector;
        const auto boundary = covered ? it->end() :
            (it == mappings_.end() ? virtual_size() / 512 : it->offset);
        const auto length = static_cast<uint32_t>(std::min<uint64_t>(
            {boundary - sector, (count - done) / 512, Mapping::kMaxLength}));
        const bool backed = covered && it->moffset != kNoPhysicalOffset;
        const uint64_t physical = backed ? it->moffset + sector - it->offset : data_end_ / 512;
        const bool transition = !covered || it->zeroed != zero || !backed;
        if (zero && covered && it->zeroed && backed) {
            done += static_cast<size_t>(length) * 512;
            continue;
        }
        if (transition && (index_end_ - lsmt::kSpace) / 16 >= kMaxLogRecords)
            co_return -ENOSPC;
        const size_t size = static_cast<size_t>(length) * 512;
        if (!backed && data_end_ > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - size)
            co_return -EFBIG;
        Mapping mapping{sector, length, physical, zero, 0};
        // Prepare all fallible mapping allocations before touching payload.
        auto next = transition ? replace(mappings_, mapping) : std::vector<Mapping>{};
        std::vector<uint8_t> zeros;
        if (zero && !backed) zeros.resize(size, 0);
        int rc = 0;
        try {
            if (!zero || !backed) {
                const void* bytes = zero ? zeros.data() : static_cast<const uint8_t*>(buffer) + done;
                const size_t write_size = io_fault(1) ? size / 2 : size;
                rc = co_await write_exact(pair_->data_fd(), bytes, write_size, physical * 512);
                if (rc == 0 && (io_fault(1) || io_fault(2))) rc = -EIO;
            }
            // Completed payload IO precedes mapping publication. Ordinary writes
            // survive process death through the kernel cache; FLUSH establishes
            // host-storage durability without a barrier for every transition.
            if (rc == 0 && transition && io_fault(3)) rc = -EIO;
            if (rc == 0 && transition) {
                std::array<uint8_t, 16> raw{};
                bytes::store_segment_le(raw.data(), mapping);
                const size_t write_size = io_fault(4) ? raw.size() / 2 : raw.size();
                rc = co_await write_exact(pair_->index_fd(), raw.data(), write_size, index_end_);
                if (rc == 0 && (io_fault(4) || io_fault(5))) rc = -EIO;
                if (rc == 0) rc = co_await pair_->append_log_progress(raw);
            }
        } catch (...) {
            std::lock_guard lock(index_mutex_);
            failed_ = true;
            throw;
        }
        if (rc != 0) {
            std::lock_guard lock(index_mutex_);
            failed_ = true;
            co_return rc;
        }
        if (!backed) data_end_ += size;
        if (transition) {
            index_end_ += 16;
            std::lock_guard lock(index_mutex_);
            mappings_.swap(next);
        }
        done += size;
    }
    co_return static_cast<ssize_t>(done);
}

elio::coro::task<int> Writer::compact() {
    co_await operation_.lock();
    Unlock unlock{operation_};
    {
        std::lock_guard lock(index_mutex_);
        if (failed_) co_return -EIO;
    }
    if (sealed_ || checkpointed_) co_return -EROFS;
    co_return co_await compact_locked();
}

elio::coro::task<int> Writer::compact_locked() {
    auto compacted = mappings_;
    // A ZERO marker has no payload ownership. Reserve backing in the staged
    // view, without appending to the old log (which may already be full).
    // Until rename, both the old log and live view still describe ZERO.
    for (auto& mapping : compacted) {
        if (!mapping.zeroed || mapping.moffset != kNoPhysicalOffset) continue;
        const size_t count = static_cast<size_t>(mapping.length) * 512;
        if (data_end_ > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - count)
            co_return -EFBIG;
        std::vector<uint8_t> zeros(count, 0);
        int rc;
        try { rc = co_await write_exact(pair_->data_fd(), zeros.data(), count, data_end_); }
        catch (...) {
            std::lock_guard lock(index_mutex_);
            failed_ = true;
            throw;
        }
        if (rc != 0) {
            std::lock_guard lock(index_mutex_);
            failed_ = true;
            co_return rc;
        }
        mapping.moffset = data_end_ / 512;
        data_end_ += count;
    }
    size_t used = 0;
    for (const auto& mapping : compacted) {
        if (used > 0) {
            auto& previous = compacted[used - 1];
            if (previous.end() == mapping.offset && previous.zeroed == mapping.zeroed &&
                previous.moffset + previous.length == mapping.moffset &&
                mapping.length <= Mapping::kMaxLength - previous.length) {
                previous.length += mapping.length;
                continue;
            }
        }
        compacted[used++] = mapping;
    }
    compacted.resize(used);
    const auto generation = pair_->index_generation();
    int rc = 0;
    std::exception_ptr failure;
    try { rc = co_await pair_->replace_index(compacted); }
    catch (...) { failure = std::current_exception(); }
    if (pair_->index_generation() != generation) {
        index_end_ = pair_->index_bytes();
        std::lock_guard lock(index_mutex_);
        mappings_.swap(compacted);
    }
    if (failure) std::rethrow_exception(failure);
    co_return rc;
}

int Writer::grow(uint64_t virtual_size) {
    if (!operation_.try_lock()) return -EBUSY;
    Unlock unlock{operation_};
    {
        std::lock_guard lock(index_mutex_);
        if (failed_) return -EIO;
    }
    if (sealed_ || checkpointed_) return -EROFS;
    if (virtual_size == 0 || virtual_size % 512 != 0 ||
        virtual_size / 512 > Mapping::kMaxOffset || virtual_size < this->virtual_size())
        return -EINVAL;
    int rc;
    try { rc = pair_->grow(virtual_size); }
    catch (...) {
        std::lock_guard lock(index_mutex_);
        failed_ = true;
        throw;
    }
    if (rc != 0) {
        std::lock_guard lock(index_mutex_);
        failed_ = true;
        return rc;
    }
    virtual_size_.store(virtual_size);
    return 0;
}

elio::coro::task<int> Writer::seal(std::string user_tag) {
    if (user_tag.size() >= 256) co_return -EINVAL;
    co_await operation_.lock();
    Unlock unlock{operation_};
    {
        std::lock_guard lock(index_mutex_);
        if (failed_) co_return -EIO;
    }
    if (sealed_) co_return -EALREADY;
    if (checkpointed_) co_return -EROFS;
    const int compacted = co_await compact_locked();
    if (compacted != 0) co_return compacted;
    int rc;
    try { rc = co_await pair_->seal(std::move(user_tag)); }
    catch (...) {
        std::lock_guard lock(index_mutex_);
        failed_ = true;
        throw;
    }
    if (rc != 0) {
        std::lock_guard lock(index_mutex_);
        failed_ = true;
        co_return rc;
    }
    sealed_ = true;
    co_return 0;
}

elio::coro::task<int> Writer::flush() {
    co_await operation_.lock();
    Unlock unlock{operation_};
    {
        std::lock_guard lock(index_mutex_);
        if (failed_) co_return -EIO;
    }
    co_return co_await flush_locked();
}

elio::coro::task<int> Writer::checkpoint() {
    co_await operation_.lock();
    Unlock unlock{operation_};
    {
        std::lock_guard lock(index_mutex_);
        if (failed_) co_return -EIO;
    }
    if (sealed_ || checkpointed_) co_return -EROFS;
    const int rc = co_await flush_locked();
    if (rc == 0) checkpointed_ = true;
    co_return rc;
}

elio::coro::task<int> Writer::flush_locked() {
    if (io_fault(6)) co_return -EIO;
    int rc = co_await sync(pair_->data_fd());
    if (rc == 0) {
        if (io_fault(7)) co_return -EIO;
        const auto synced = co_await elio::io::sync_file(
            pair_->index_fd(), elio::io::file_sync_mode::data_and_metadata);
        rc = synced ? 0 : -synced.error_value();
    }
    if (rc == 0) {
        if (io_fault(8)) co_return -EIO;
        rc = co_await pair_->sync_index_directory();
    }
    if (rc == 0) rc = co_await pair_->commit_log_state();
    co_return rc;
}
}  // namespace obd::format::hybrid
