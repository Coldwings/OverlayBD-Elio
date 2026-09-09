// Structural warm-up. See structural_warmup.hpp for the contract.
#include "image/structural_warmup.hpp"

#include <elio/log/macros.hpp>

#include <algorithm>

namespace obd::image {

namespace {

/// Populate slice size: 64 KiB — the LayerStore extent size (ADR-0011),
/// chosen as the path's native per-extent suspension granularity, not an
/// alignment guarantee. The LayerStore populate path already suspends per
/// extent between remote fetches, so slicing at this granularity adds no
/// remote traffic (already-fetched extents are skipped via the present
/// flags). Slices are computed in the TarOffsetSource VIEW byte space: at
/// an unaligned tar base one
/// view-space slice can span TWO underlying extents (two fetches and
/// funnel acquires), so the wall-budget check below bounds how far past
/// the budget one slow source can carry warm-up to at most two extent
/// fetches — and lets a window that cannot finish in time be abandoned
/// mid-window instead of awaited to its end.
constexpr uint64_t kPopulateSliceBytes = 64 * 1024;

}  // namespace

std::vector<WarmWindow> structural_windows(uint64_t blob_size,
                                           uint64_t head_bytes,
                                           uint64_t tail_bytes) noexcept {
    std::vector<WarmWindow> out;
    if (blob_size == 0) return out;
    const uint64_t head = std::min(head_bytes, blob_size);
    const uint64_t tail = std::min(tail_bytes, blob_size);
    if (head > 0 && tail > 0 && head + tail >= blob_size) {
        // The clamped windows cover the whole blob (a blob smaller than
        // head+tail): one merged window, so nothing is populated twice.
        out.push_back({0, blob_size});
        return out;
    }
    if (head > 0) out.push_back({0, head});
    if (tail > 0) out.push_back({blob_size - tail, tail});
    return out;
}

elio::coro::task<StructuralWarmupStats> warmup_structural(
    const std::vector<source::BlobSource*>& warm_targets,
    const StructuralWarmupOptions& opts) {
    StructuralWarmupStats stats;
    const auto deadline =
        std::chrono::steady_clock::now() + opts.max_wall_time;

    for (auto* target : warm_targets) {
        if (std::chrono::steady_clock::now() >= deadline) {
            stats.budget_exhausted = true;
            break;
        }
        if (target == nullptr) continue;
        ++stats.layers_total;
        bool warmed = false;
        bool stop = false;
        for (const auto& win :
             structural_windows(target->size(), opts.head_bytes,
                                opts.tail_bytes)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                stats.budget_exhausted = true;
                stop = true;
                break;
            }
            // Populate the window in extent-sized slices, re-checking the
            // wall budget between slices: a window whose source is slow
            // (a congested registry, a funnel gate closed by contention)
            // is abandoned mid-window once the budget is spent — skipped,
            // never awaited to its end; its remaining extents are served
            // on demand. A slice that fails or reports EAGAIN (the funnel
            // gate stayed closed past the admit timeout, issue #35) skips
            // the whole window; a throwing slice fails it (the previous
            // all-at-once semantics) — warm-up moves on either way.
            bool window_failed = false;
            bool window_skipped = false;
            uint64_t off = win.offset;
            const uint64_t end = win.offset + win.len;
            while (off < end) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    stats.budget_exhausted = true;
                    window_skipped = true;
                    stop = true;
                    break;
                }
                const size_t slice = static_cast<size_t>(
                    std::min(kPopulateSliceBytes, end - off));
                ssize_t r = 0;
                try {
                    r = co_await target->populate(off, slice);
                } catch (const std::exception& e) {
                    // Defensive: populate is a hot-path -errno interface
                    // and must not throw, but warm-up is opportunistic —
                    // a throw degrades to a skipped window, never a
                    // bring-up error.
                    ELIO_LOG_WARNING("structural warm-up populate threw "
                                     "({}) on {}; window skipped",
                                     e.what(), target->label());
                    window_failed = true;
                    break;
                }
                if (r == -EAGAIN) {
                    // The funnel gate stayed closed past the admit
                    // timeout: skipped, never awaited (issue #35).
                    ELIO_LOG_DEBUG("structural warm-up populate not "
                                   "admitted promptly on {}; window "
                                   "skipped",
                                   target->label());
                    window_skipped = true;
                    break;
                }
                if (r < 0) {
                    // Opportunistic: a failed warm-up window is logged
                    // and skipped; the device serves those extents on
                    // demand.
                    ELIO_LOG_DEBUG("structural warm-up populate failed "
                                   "({}) on {}; window skipped",
                                   static_cast<int>(-r), target->label());
                    window_failed = true;
                    break;
                }
                stats.bytes_warmed += slice;
                warmed = true;
                off += slice;
            }
            if (window_skipped) {
                ++stats.windows_skipped;
                if (stop) break;
                continue;
            }
            if (window_failed) {
                ++stats.windows_failed;
                continue;
            }
            ++stats.windows_populated;
        }
        if (warmed) ++stats.layers_warmed;
        if (stats.budget_exhausted) break;
    }

    ELIO_LOG_INFO("structural warm-up: {}/{} layers warmed ({} windows, {} "
                  "bytes, {} failed, {} skipped{})",
                  stats.layers_warmed, stats.layers_total,
                  stats.windows_populated, stats.bytes_warmed,
                  stats.windows_failed, stats.windows_skipped,
                  stats.budget_exhausted ? ", budget exhausted" : "");
    co_return stats;
}

}  // namespace obd::image
