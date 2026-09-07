// LSMT read-only views: a single layer over its data source, and the merged
// multi-layer block view.
//
// Reimplements the read-only side of overlaybd's LSMT
// (src/overlaybd/lsmt/file.cpp + index.cpp): open_file_ro (header/trailer
// verification, index load and validation), open_files_ro (bottom-up layer
// list, reverse so tag 0 = topmost, recursive gap merge), and
// LSMTReadOnlyFile::pread (sector-aligned reads, holes/zeroed segments read
// as zeroes, per-segment dispatch to the owning layer's data source).
#pragma once

#include "common/bytes.hpp"
#include "format/lsmt_format.hpp"
#include "source/blob_source.hpp"

#include <elio/coro/task.hpp>

#include <memory>
#include <string>
#include <vector>

namespace obd::format {

/// One sealed read-only LSMT layer: parsed segment index plus the underlying
/// data source (typically a ZFileSource view of the layer blob).
class LsmtLayer {
public:
    /// Opens a single-layer read-only LSMT over `src` (ownership taken).
    /// Validation mirrors overlaybd do_load_index()/create_memory_index():
    /// header magic + is_header + is_data_file; trailer magic + is_trailer +
    /// is_data_file + is_sealed; trailer fields are authoritative; index
    /// entries with offset == INVALID_OFFSET are dropped; remaining entries
    /// must be strictly ordered (a.end() <= b.offset) and every moffset must
    /// lie within the data region [8, index_offset/512) sectors.
    /// Throws obd::format_error / obd::error on any violation.
    static elio::coro::task<std::unique_ptr<LsmtLayer>> open(
        source::BlobSourcePtr src);

    /// Sorted, disjoint segment index (512B sector units, tag = 0).
    const std::vector<bytes::segment_mapping>& segments() const noexcept {
        return segments_;
    }

    uint64_t virtual_size() const noexcept { return ht_.virtual_size; }
    const lsmt::HeaderTrailer& header() const noexcept { return ht_; }

    /// The source segment data is read from (below this layer's view).
    source::BlobSource& data_source() const noexcept { return *src_; }

    const std::string& layer_label() const noexcept { return label_; }

private:
    LsmtLayer() = default;

    source::BlobSourcePtr src_;
    lsmt::HeaderTrailer ht_;  // trailer-authoritative
    std::vector<bytes::segment_mapping> segments_;
    std::string label_;
};

/// Merged read-only view of a layer stack — the final block-device backing
/// source. Constructed from layers in BOTTOM-UP order (layers[0] =
/// bottom-most), matching overlaybd LSMT::open_files_ro(); internally the
/// stack is reversed so index tag 0 addresses the topmost layer.
class MergedLsmt final : public source::BlobSource {
public:
    /// Merges `layers_bottom_up` (1..255 layers, ownership taken together
    /// with each layer's source chain). Virtual size is the topmost non-zero
    /// layer virtual_size (overlaybd load_merge_index semantics).
    /// Throws obd::format_error on invalid stacks.
    static elio::coro::task<std::unique_ptr<MergedLsmt>> open(
        std::vector<std::unique_ptr<LsmtLayer>> layers_bottom_up);

    /// Sector-aligned positional read: `offset` and `count` must be
    /// multiples of 512 (the LSMT contract; ublk request granularity
    /// guarantees this). Holes and zeroed segments read as zeroes. Clamped
    /// at size(). Returns count (or remaining at EOF), negative -errno on
    /// failure.
    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;

    uint64_t size() const noexcept override { return vsize_; }
    std::string_view label() const noexcept override { return label_; }

    /// Merged index: sorted, disjoint, tag = layer position with 0 =
    /// topmost. Exposed for diagnostics and tests.
    const std::vector<bytes::segment_mapping>& merged_index() const noexcept {
        return index_;
    }

    /// Layers in top-first order (index 0 = topmost).
    const std::vector<std::unique_ptr<LsmtLayer>>& layers() const noexcept {
        return layers_;
    }

    /// Gap-merge of per-layer indexes, exposed for unit tests. `layers[i]`
    /// is the segment list of layer i, TOPMOST FIRST. Appends the merged,
    /// sorted, disjoint segment list to `out` with tag = layer index.
    static void merge_indexes(
        const std::vector<const std::vector<bytes::segment_mapping>*>& layers,
        std::vector<bytes::segment_mapping>& out);

private:
    MergedLsmt() = default;

    static void merge_range(
        const std::vector<const std::vector<bytes::segment_mapping>*>& layers,
        size_t level, uint64_t lo, uint64_t hi,
        std::vector<bytes::segment_mapping>& out);

    std::vector<std::unique_ptr<LsmtLayer>> layers_;  // topmost first
    std::vector<bytes::segment_mapping> index_;
    uint64_t vsize_ = 0;
    std::string label_;
};

}  // namespace obd::format
