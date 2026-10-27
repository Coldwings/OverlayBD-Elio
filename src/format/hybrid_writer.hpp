// Serialized payload and mapping-log engine for an already opened Hybrid pair.
#pragma once
#include "format/hybrid_file.hpp"
#include <elio/sync/mutex.hpp>
#include <mutex>

namespace obd::format::hybrid {
class Writer {
public:
    explicit Writer(std::unique_ptr<FilePair> pair);
    elio::coro::task<ssize_t> pread(void* buffer, size_t count, uint64_t offset);
    /// Physical payload view for a merger holding its operation guard.
    elio::coro::task<ssize_t> pread_physical(void* buffer, size_t count, uint64_t offset);
    uint64_t physical_size() const noexcept { return data_end_.load(); }
    elio::coro::task<ssize_t> pwrite(const void* buffer, size_t count, uint64_t offset);
    elio::coro::task<int> discard(uint64_t offset, uint64_t count);
    elio::coro::task<int> flush();
    /// Terminal durability barrier for this instance; reopening permits writes.
    elio::coro::task<int> checkpoint();
    elio::coro::task<int> compact();
    int grow(uint64_t virtual_size);  // Blocking: run off scheduler workers.
    elio::coro::task<int> seal(std::string user_tag = "");
    uint64_t virtual_size() const noexcept { return virtual_size_.load(); }
    std::vector<bytes::segment_mapping> snapshot() const;

private:
    elio::coro::task<int> compact_locked();
    elio::coro::task<int> flush_locked();
    elio::coro::task<ssize_t> modify(const void* buffer, size_t count,
                                     uint64_t offset, bool zero);
    std::unique_ptr<FilePair> pair_;
    elio::sync::mutex operation_;
    mutable std::mutex index_mutex_;
    std::vector<bytes::segment_mapping> mappings_;
    std::atomic<uint64_t> virtual_size_{0};
    bool sealed_ = false;
    bool checkpointed_ = false;  // Protected by operation_; published after sync.
    std::atomic<uint64_t> data_end_;
    uint64_t index_end_;
    bool failed_ = false;  // Protected with index_mutex_; fail closed after uncertain IO.
};
}  // namespace obd::format::hybrid
