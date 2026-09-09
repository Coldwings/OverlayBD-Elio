// Structural warm-up (ADR-0012's cold-start floor): at device bring-up,
// populate a bounded window at the head and the tail of every data lower
// — index regions, tar headers, filesystem metadata neighborhoods — with
// no trace required.
//
// Windows are computed from the layer blob's byte layout alone (head =
// [0, W_head), tail = [size-W_tail, size)) — no format parsing (the
// prioritized-files mode remains rejected, ADR-0013). They are computed
// in the STORED-BLOB byte space of the TarOffsetSource view — the same
// byte space trace replay addresses:
//
//   * the head window covers the payload head: the ZFile header / LSMT
//     header and the first data blocks. For tar-wrapped lowers the tar
//     header itself sits just BELOW the view (base offset 512/1536), in
//     the same first underlying extents — and is already fetched by the
//     TarOffsetSource::open probe during assembly.
//   * the tail window covers the payload tail: the ZFile jump table and
//     trailer and the LSMT index all live at the tail of the payload
//     (zfile_format.hpp layout: ... | index | Trailer (512B); LSMT keeps
//     its index at the file tail), so the VIEW tail — not the underlying
//     blob's tail — is the right window: the bytes past the payload end
//     inside the blob are tar zero-padding, which holds no data. The
//     view's populate() translates the window through the tar base
//     offset, so the LayerStore below warms exactly the extents carrying
//     the indexes.
//
// Warm-up runs AWAITED INLINE during device bring-up, before trace
// replay (the floor first; replay refines it), bounded by the wall-time
// budget below; every populate passes the device's read admission funnel
// as the Prefetch scavenger class (populate's wiring, ADR-0012), so it
// yields to on-demand reads automatically. Extent-granular dedup against
// replay, fill, and the open-time format probes is automatic via the
// LayerStore in-flight map and present flags — an already-warm extent is
// joined or skipped, never re-fetched.
//
// Warm-up is OPPORTUNISTIC: a failing populate (a source error, a
// bypassed/degraded LayerStore — populate is a no-op there, never an
// error) is logged and skipped, never a device bring-up error, and the
// wall-time budget caps the worst-case bring-up delay.
#pragma once

#include "source/blob_source.hpp"

#include <elio/coro/task.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace obd::image {

/// One byte-range window [offset, offset+len) to populate.
struct WarmWindow {
    uint64_t offset = 0;
    uint64_t len = 0;
};

/// Function structural_windows computes the structural warm-up windows
/// for a blob of `blob_size`
/// bytes: head [0, head_bytes) and tail [blob_size-tail_bytes, blob_size),
/// each clamped to the blob (a 0 size disables that side). When the two
/// clamped windows would cover the whole blob together (head+tail >=
/// blob_size — overlap or exact adjacency, i.e. any blob smaller than
/// head+tail), they merge into the single window [0, blob_size) so no
/// byte is populated twice. An empty blob yields no windows.
std::vector<WarmWindow> structural_windows(uint64_t blob_size,
                                           uint64_t head_bytes,
                                           uint64_t tail_bytes) noexcept;

/// Bounds so a stalled or hostile source cannot delay bring-up without
/// limit (rationale: docs/image.md "Structural warm-up").
struct StructuralWarmupOptions {
    /// Head window size in bytes (default 1 MiB; 0 disables).
    uint64_t head_bytes = uint64_t{1} << 20;
    /// Tail window size in bytes (default 1 MiB; 0 disables).
    uint64_t tail_bytes = uint64_t{1} << 20;
    /// Wall-time budget: warm-up is sequentially awaited during device
    /// bring-up, so this caps the worst-case bring-up delay it adds
    /// (same pattern and default as trace replay).
    std::chrono::milliseconds max_wall_time{30000};
};

/// Outcome of one warm-up pass, for logs and diagnostics (OpenedImage).
struct StructuralWarmupStats {
    size_t layers_total = 0;      ///< data lowers warm-up ran for
    size_t layers_warmed = 0;     ///< layers with >= 1 successful window
    size_t windows_populated = 0; ///< populate() calls that succeeded
    size_t windows_failed = 0;    ///< populate() calls that failed/threw
    uint64_t bytes_warmed = 0;    ///< sum of populated window lengths
    bool budget_exhausted = false; ///< stopped early on the wall budget
};

/// Function warmup_structural populates the head/tail windows of every
/// target: warm_targets[i] is the stored-blob-level source of data lower
/// i (the TarOffsetSource view; a nullptr entry is skipped). Per layer
/// the head window is populated before the tail window, each populate()
/// sequentially awaited. Never throws: a failed or throwing populate is
/// logged and counted (windows_failed), and warm-up moves on — the
/// device comes up regardless. Processing stops early when the
/// wall-time budget is spent (budget_exhausted set).
elio::coro::task<StructuralWarmupStats> warmup_structural(
    const std::vector<source::BlobSource*>& warm_targets,
    const StructuralWarmupOptions& opts = {});

}  // namespace obd::image
