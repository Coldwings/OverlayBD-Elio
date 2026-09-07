// Merged writable view (ADR-0008): RO lowers plus a writable top layer as
// one block source. Reads walk the merged index (top wins, holes zero);
// writes always land in the writable top layer (copy-on-write — lower
// layers are never modified). The merged index is recomputed after every
// write; write-heavy workloads should batch (documented in docs/format.md).
#pragma once

#include "format/lsmt.hpp"
#include "format/writable.hpp"
#include "source/blob_source.hpp"

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

    uint64_t size() const noexcept override { return vsize_; }
    std::string_view label() const noexcept override { return label_; }

    WritableLayer& writable_top() const noexcept { return *top_; }

    const std::vector<bytes::segment_mapping>& merged_index() const noexcept {
        return index_;
    }

private:
    MergedWritable() = default;
    void rebuild_index();

    std::vector<std::unique_ptr<LsmtLayer>> layers_;  // topmost first (RO)
    std::unique_ptr<WritableLayer> top_;
    std::vector<bytes::segment_mapping> index_;       // tag 0 = writable top
    uint64_t vsize_ = 0;
    std::string label_;
};

}  // namespace obd::format
