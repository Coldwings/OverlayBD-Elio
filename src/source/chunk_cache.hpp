// ChunkCache — in-memory LRU chunk cache in front of a slow BlobSource
// (registry or DART-backed), sized ~64KB chunks / ~256MB total by default.
//
// This stands in for overlaybd's file-based cache (fiemap-tracked local
// cache file): the per-device process model makes a private in-memory cache
// simpler and safer than shared on-disk cache files; when DART is in the
// path, DART itself provides the shared on-node disk cache
// (design-assumptions.md §S-4).
#pragma once

#include "source/blob_source.hpp"

#include <elio/coro/task.hpp>
#include <elio/sync/mutex.hpp>

#include <atomic>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

namespace obd::source {

class ChunkCache final : public BlobSource {
public:
    struct Config {
        size_t chunk_size = 64 * 1024;
        size_t max_bytes = 256ULL * 1024 * 1024;
    };

    /// Wraps `inner` (ownership taken). `inner` must report a stable size.
    static elio::coro::task<std::unique_ptr<ChunkCache>> open(
        BlobSourcePtr inner);
    static elio::coro::task<std::unique_ptr<ChunkCache>> open(
        BlobSourcePtr inner, Config cfg);

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;

    uint64_t size() const noexcept override { return size_; }
    std::string_view label() const noexcept override { return label_; }

    uint64_t hits() const noexcept { return hits_.load(); }
    uint64_t misses() const noexcept { return misses_.load(); }

private:
    ChunkCache() = default;

    BlobSourcePtr inner_;
    Config cfg_;
    uint64_t size_ = 0;
    std::string label_;

    struct CachedChunk {
        std::shared_ptr<const std::vector<uint8_t>> data;
        std::list<uint64_t>::iterator lru_it;
    };
    elio::sync::mutex mu_;
    std::list<uint64_t> lru_;  // chunk ids, front = most recently used
    std::unordered_map<uint64_t, CachedChunk> chunks_;
    size_t bytes_ = 0;

    std::atomic<uint64_t> hits_{0};
    std::atomic<uint64_t> misses_{0};
};

}  // namespace obd::source
