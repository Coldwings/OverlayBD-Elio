// Trace replay (ADR-0013): translate a parsed prefetch trace
// blob into populate() warm-up calls on the per-lower source chains.
//
// Wire authority: docs/trace-format.md. Each READ record
// {layer_index, offset, count} becomes populate(offset, count) on the
// stored-blob-level source of the corresponding lower (the
// TarOffsetSource view — the same byte space upstream's PrefetchFile
// wraps, i.e. the layer blob file below decompression), executed in
// recorded order. Replay is awaited INLINE during device bring-up,
// bounded by the wall-time budget below; every populate it issues
// passes the device's read admission funnel (ADR-0012) as the Prefetch
// scavenger class. Detaching replay off the bring-up path — now safe,
// since the funnel yields to on-demand reads — is a documented
// follow-up, not part of the funnel's landing.
//
// Replay is OPPORTUNISTIC: a missing, malformed, or stale trace and any
// individual populate failure are logged and skipped — never a device
// bring-up error (upstream degrades identically, trace-format.md §7).
#pragma once

#include "source/blob_source.hpp"

#include <elio/coro/task.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace obd::image {

/// Bounds so a hostile or stale trace cannot amplify registry traffic or
/// delay bring-up without limit (rationale: docs/image.md "Trace layer").
struct TraceReplayOptions {
    /// Records processed from the trace before replay stops early.
    /// Real recorded traces carry thousands of records (one per pread
    /// during container start); 64k is far above legitimate sizes yet
    /// bounds the loop.
    size_t max_records = 65536;

    /// Total bytes handed to populate() before replay stops early.
    /// Warm-up beyond ~1 GiB delays the cold start more than it saves;
    /// with the per-record 1 MiB cap this also bounds traffic
    /// amplification from extent rounding.
    uint64_t max_bytes = uint64_t{1} << 30;  // 1 GiB

    /// Wall-time budget: replay is sequentially awaited during device
    /// bring-up, so this caps the worst-case bring-up delay.
    std::chrono::milliseconds max_wall_time{30000};

    /// Per-record count cap (trace-format.md §8/§10: the upstream replay
    /// buffer is 1 MiB; conforming writers split larger ranges). Records
    /// above the cap are skipped, not clamped: they can only come from a
    /// non-conforming writer.
    uint64_t max_record_count = 1048576;  // 1 MiB
};

/// Outcome of one replay pass, for logs and diagnostics (OpenedImage).
struct TraceReplayStats {
    bool trace_present = false;  ///< blob parsed as a valid trace
    size_t records_total = 0;    ///< records in the parsed trace
    size_t records_replayed = 0; ///< populate() calls issued
    size_t records_skipped = 0;  ///< non-READ / unknown layer / bad record
    uint64_t bytes_warmed = 0;   ///< sum of replayed record counts
    bool budget_exhausted = false; ///< stopped early on a replay bound
};

/// Function replay_trace parses `blob` (the trace codec,
/// src/format/trace.hpp) and replays it against `warm_targets`:
/// warm_targets[i] is the stored-blob-level source of data lower i
/// (layer_index addresses it directly; a nullptr entry counts as unknown
/// layer). Records are processed in recorded order, each populate()
/// sequentially awaited. Never throws: parse failure yields
/// {trace_present=false}; per-record skips follow upstream replay parity
/// (non-READ ops and unknown layer indexes are silent no-ops,
/// trace-format.md §5/§8); a failed populate is logged and skipped.
/// Processing stops early when a TraceReplayOptions bound is hit
/// (budget_exhausted set).
elio::coro::task<TraceReplayStats> replay_trace(
    std::span<const uint8_t> blob,
    const std::vector<source::BlobSource*>& warm_targets,
    const TraceReplayOptions& opts = {});

}  // namespace obd::image
