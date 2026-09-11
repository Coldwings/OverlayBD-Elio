// Merged writable view (ADR-0008): RO lowers plus a writable top layer as
// one block source. Reads walk the merged index (top wins, holes zero);
// writes always land in the writable top layer (copy-on-write — lower
// layers are never modified). The merged index is recomputed after every
// write; write-heavy workloads should batch (documented in docs/format.md).
#pragma once

#include "format/lsmt.hpp"
#include "format/writable.hpp"
#include "source/blob_source.hpp"

#include <elio/sync/mutex.hpp>

#include <atomic>
#include <mutex>

namespace obd::format {

/// Writable block source: BlobSource + sector-aligned pwrite + flush.
/// The ublk bridge dispatches WRITE requests through this interface; a
/// read-only image simply does not implement it (writes get -EROFS).
class MergedWritable final : public source::WritableBlobSource {
public:
    /// `layers_bottom_up` — sealed RO lowers (may be empty);
    /// `top` — the writable layer (ownership taken). The device virtual
    /// size is max(top vsize, lowers vsize).
    static elio::coro::task<std::unique_ptr<MergedWritable>> open(
        std::vector<std::unique_ptr<LsmtLayer>> layers_bottom_up,
        std::unique_ptr<WritableLayer> top);

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;
    elio::coro::task<ssize_t> pwrite(const void* buf, size_t count,
                                     uint64_t offset) override;
    elio::coro::task<int> flush() override;
    elio::coro::task<int> discard(uint64_t offset, uint64_t len) override;

    uint64_t size() const noexcept override {
        return vsize_.load(std::memory_order_acquire);
    }
    std::string_view label() const noexcept override { return label_; }

    /// D3 grow-only vsize extension: grows the writable top first (so its
    /// pwrite/discard accept the new range) and then widens the merged
    /// view to `vsize` — the headroom region reads as zeroes until
    /// written, and new writes land in the writable top. Equal is an
    /// idempotent no-op; smaller is a shrink and returns -EINVAL.
    /// BLOCKING (the top grow does blocking header IO): run off an Elio
    /// worker via elio::spawn_blocking — the device resize executor does.
    /// Returns 0 or a negative -errno.
    int grow(uint64_t vsize);

    WritableLayer& writable_top() const noexcept { return *top_; }

    std::vector<bytes::segment_mapping> merged_index() const {
        std::lock_guard lock(index_mu_);
        return index_;
    }

private:
    MergedWritable() = default;
    void rebuild_index();

    std::vector<std::unique_ptr<LsmtLayer>> layers_;  // topmost first (RO)
    std::unique_ptr<WritableLayer> top_;
    /// Serializes top-layer mutations and their merged-index rebuilds so an
    /// older rebuild cannot publish after a newer write/discard.
    elio::sync::mutex op_mu_;
    std::vector<bytes::segment_mapping> index_;       // tag 0 = writable top
    mutable std::mutex index_mu_;
    /// Device virtual size in bytes. Atomic: the device resize executor
    /// (a spawn_blocking pool thread) grows the merged view while bridge
    /// coroutines on Elio workers read/write through it.
    std::atomic<uint64_t> vsize_{0};
    std::string label_;
};

}  // namespace obd::format
