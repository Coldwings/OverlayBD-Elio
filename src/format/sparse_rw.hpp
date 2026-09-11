// Sparse writable layer: a sparse file whose written extents form the
// layer's segment index with identity mapping (moffset == offset).
// Live extents are rebuilt from the kernel fiemap (SEEK_DATA/SEEK_HOLE)
// when an existing file is opened, and discard zero masks are recovered
// from a small sidecar. Note: fiemap is filesystem-block granular, so a
// sub-block discard may recover as a fatter live extent; reads are
// unaffected because punched bytes read back as zeroes.
#pragma once

#include "format/writable.hpp"

#include <elio/sync/mutex.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace obd::format {

class SparseRwLayer final : public WritableLayer {
public:
    ~SparseRwLayer() override;

    /// Opens (creating if needed) `path` as a sparse file of `vsize` bytes.
    /// Existing content is kept; written extents are recovered via fiemap.
    static elio::coro::task<std::unique_ptr<SparseRwLayer>> open(
        const std::string& path, uint64_t vsize);

    elio::coro::task<ssize_t> pwrite(const void* buf, size_t count,
                                     uint64_t offset) override;
    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;
    elio::coro::task<int> flush() override;
    elio::coro::task<int> discard(uint64_t offset, uint64_t len) override;
    /// Persists dirty zero-mask metadata after syncing sparse data. Sparse
    /// uppers never seal (ADR-0014 upstream parity).
    elio::coro::task<int> checkpoint() override;

    /// D3 grow-only vsize extension (see WritableLayer::grow): extends
    /// the sparse file to `vsize` bytes so pwrite/discard accept the new
    /// range. BLOCKING (ftruncate): run off an Elio worker via
    /// elio::spawn_blocking. Returns 0 or a negative -errno.
    int grow(uint64_t vsize) override;

    uint64_t virtual_size() const override {
        return vsize_.load(std::memory_order_acquire);
    }
    const std::vector<bytes::segment_mapping>& segments() const override {
        return segments_;
    }
    std::vector<bytes::segment_mapping> segments_snapshot() const override;
    source::BlobSource& data_source() override;

private:
    class View;

    SparseRwLayer() = default;
    bool has_zero_masks() const;
    bool has_zero_masks_locked() const;
    bool range_intersects_zero_mask_locked(uint64_t lo, uint64_t hi) const;
    std::vector<bytes::segment_mapping> zero_mask_segments() const;
    std::vector<bytes::segment_mapping> zero_mask_segments_locked() const;
    void insert_live_extent(uint64_t off, uint64_t len);
    void insert_zero_extent(uint64_t off, uint64_t len);
    void erase_range(uint64_t lo, uint64_t hi);
    int persist_zero_masks(const std::vector<bytes::segment_mapping>& zeroes,
                           uint64_t vsize) const;
    int persist_current_zero_masks() const;
    void load_zero_masks();

    int fd_ = -1;                    // RW fd (writes + flushes)
    std::unique_ptr<View> ro_;       // RO view for data_source()
    /// Declared size in bytes. Atomic: the device resize executor (a
    /// spawn_blocking pool thread) grows the layer while bridge
    /// coroutines on Elio workers read/write through it.
    std::atomic<uint64_t> vsize_{0}; // bytes
    std::string zero_mask_path_;
    std::vector<bytes::segment_mapping> segments_;
    /// Serializes pwrite/discard/flush durability ordering without
    /// blocking an Elio worker while disk work is offloaded.
    elio::sync::mutex op_mu_;
    mutable std::mutex meta_mu_;
    uint64_t zero_masks_generation_ = 0;
    bool zero_masks_dirty_ = false;
};

}  // namespace obd::format
