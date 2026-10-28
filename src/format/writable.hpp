// Writable layer interface (ADR-0008). A writable layer is the topmost
// layer of an image stack. Implementations:
//   * SparseRwLayer (sparse_rw.hpp) — a sparse data file with exact persisted
//     sector coverage and identity segment mapping.
//   * LsmtRwLayer   (lsmt_rw.hpp)   — an unsealed LSMT file with in-place
//     edit: subranges covered by live segments overwrite their data blocks
//     in place; uncovered or discarded (zeroed) subranges are appended.
//     seal() compacts into a standard sealed LSMT RO file.
//   * HybridRwLayer (hybrid_rw.hpp) — durable mapping log with in-place
//     payload reuse and metadata-only sealing.
#pragma once

#include "common/bytes.hpp"
#include "source/blob_source.hpp"

#include <elio/coro/task.hpp>

#include <cstdint>
#include <optional>
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

    /// Persists whatever on-disk state an offline seal needs (ADR-0014),
    /// without sealing. Called once by the device process on graceful
    /// shutdown, after IO has drained; it is terminal — no pwrite/discard
    /// may follow. Sparse layers use this hook to publish any dirty
    /// discard zero-mask sidecar state. Returns 0 or a negative -errno.
    virtual elio::coro::task<int> checkpoint() = 0;

    virtual uint64_t virtual_size() const = 0;

    /// D3 grow-only vsize extension: extends this layer's write window to
    /// `vsize` bytes so pwrite/discard accept the new range (the merged
    /// view's grow calls this before widening itself). Must be a positive
    /// multiple of 512 and strictly larger than the current size (an
    /// equal request is an idempotent no-op — retried grows after a
    /// partial failure land here); smaller is a shrink and returns
    /// -EINVAL. For LsmtRwLayer this rewrites the on-disk declared-size
    /// header (preserving the uuid), so a graceful-shutdown checkpoint
    /// and the offline seal stay consistent with the grown size. BLOCKING
    /// (header rewrite + fsync for LSMT): callers must run it off an Elio
    /// worker via elio::spawn_blocking — the device resize executor does.
    /// Returns 0 or a negative -errno.
    virtual int grow(uint64_t vsize) = 0;

    /// Current segment index: sorted, disjoint, 512B sector units, tag 0.
    virtual const std::vector<bytes::segment_mapping>& segments() const = 0;

    /// Stable copy of the current segment index for callers that must not
    /// borrow across concurrent writable-layer mutations.
    virtual std::vector<bytes::segment_mapping> segments_snapshot() const {
        return segments();
    }

    /// Monotonic best-effort notification for operations such as flush/grow
    /// that may finish a deferred index transition. Implementations that do
    /// not change their segment view in those operations may keep the default.
    virtual uint64_t segments_generation() const noexcept { return 0; }

    /// Optional exact mapping revision (ADR-0030). Under caller-serialized
    /// mutations, equal available revisions on the same instance prove that
    /// every segment field stayed unchanged between observations. Unknown is
    /// the conservative default; this is distinct from best-effort generation.
    /// Implementations must become unknown before a counter can wrap.
    virtual std::optional<uint64_t> segments_revision() const noexcept {
        return std::nullopt;
    }

    /// The file view segment data is read from (identity for sparse). A
    /// grow-capable writable layer may expose a view whose size() tracks
    /// virtual_size() after successful grow() calls.
    virtual source::BlobSource& data_source() = 0;

    /// In-place ownership transitions may invalidate a physical mapping held
    /// by a concurrent merged reader. Such tops require the merger to serialize
    /// the entire snapshot-plus-payload read with mutations and grow admission.
    /// Readers may overlap each other while excluding those mutations.
    virtual bool serialize_merged_reads() const noexcept { return false; }

    /// After a fully successful pwrite/discard, resolved per-sector DATA/ZERO
    /// ownership and physical offsets outside the requested range are unchanged,
    /// including preparatory/retry work. Segment partitioning may differ.
    /// This is a separate opt-in from exact revisions; the default is unknown.
    /// Callers serialize mutations and retain ordinary full repair on failure.
    virtual bool supports_local_mapping_updates() const noexcept { return false; }
};

}  // namespace obd::format
