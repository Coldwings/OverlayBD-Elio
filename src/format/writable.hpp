// Writable layer interface (ADR-0008). A writable layer is the topmost
// layer of an image stack; two implementations exist:
//   * SparseRwLayer (sparse_rw.hpp) — a sparse file with fiemap extent
//     tracking, identity segment mapping.
//   * LsmtRwLayer   (lsmt_rw.hpp)   — an unsealed LSMT file with in-place
//     edit: subranges already covered by the layer overwrite their data
//     blocks in place; only previously-uncovered subranges are appended.
//     seal() compacts into a standard sealed LSMT RO file.
#pragma once

#include "common/bytes.hpp"
#include "source/blob_source.hpp"

#include <elio/coro/task.hpp>

#include <cstdint>
#include <vector>

namespace obd::format {

/// Sector-aligned writable layer. The 512B alignment contract is identical
/// to the read side (ublk request granularity guarantees it).
class WritableLayer {
public:
    virtual ~WritableLayer() = default;

    /// Writes count bytes at offset; both must be 512B multiples.
    /// Returns count, or negative -errno.
    virtual elio::coro::task<ssize_t> pwrite(const void* buf, size_t count,
                                             uint64_t offset) = 0;

    /// Reads through this layer alone; holes read as zeroes (fall-through
    /// to lower layers is the merger's job).
    virtual elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                            uint64_t offset) = 0;

    /// Durability point (ublk FLUSH).
    virtual elio::coro::task<int> flush() = 0;

    /// Discards [offset, offset+len) (both 512B multiples): the range
    /// reads back as zeroes from this layer onwards (ADR-0009). Returns 0
    /// or a negative -errno.
    virtual elio::coro::task<int> discard(uint64_t offset,
                                          uint64_t len) = 0;

    virtual uint64_t virtual_size() const = 0;

    /// Current segment index: sorted, disjoint, 512B sector units, tag 0.
    virtual const std::vector<bytes::segment_mapping>& segments() const = 0;

    /// The file view segment data is read from (identity for sparse).
    virtual source::BlobSource& data_source() = 0;
};

}  // namespace obd::format
