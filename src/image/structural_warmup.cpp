// Structural warm-up. See structural_warmup.hpp for the contract.
#include "image/structural_warmup.hpp"

#include <elio/log/macros.hpp>

#include <algorithm>

namespace obd::image {

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
        for (const auto& win :
             structural_windows(target->size(), opts.head_bytes,
                                opts.tail_bytes)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                stats.budget_exhausted = true;
                break;
            }
            ssize_t r = 0;
            try {
                r = co_await target->populate(
                    win.offset, static_cast<size_t>(win.len));
            } catch (const std::exception& e) {
                // Defensive: populate is a hot-path -errno interface and
                // must not throw, but warm-up is opportunistic — a throw
                // degrades to a skipped window, never a bring-up error.
                ELIO_LOG_WARNING("structural warm-up populate threw ({}) on "
                                 "{}; window skipped",
                                 e.what(), target->label());
                ++stats.windows_failed;
                continue;
            }
            if (r < 0) {
                // Opportunistic: a failed warm-up window is logged and
                // skipped; the device serves those extents on demand.
                ELIO_LOG_DEBUG("structural warm-up populate failed ({}) on "
                               "{}; window skipped",
                               static_cast<int>(-r), target->label());
                ++stats.windows_failed;
                continue;
            }
            ++stats.windows_populated;
            stats.bytes_warmed += win.len;
            warmed = true;
        }
        if (warmed) ++stats.layers_warmed;
        if (stats.budget_exhausted) break;
    }

    ELIO_LOG_INFO("structural warm-up: {}/{} layers warmed ({} windows, {} "
                  "bytes, {} failed{})",
                  stats.layers_warmed, stats.layers_total,
                  stats.windows_populated, stats.bytes_warmed,
                  stats.windows_failed,
                  stats.budget_exhausted ? ", budget exhausted" : "");
    co_return stats;
}

}  // namespace obd::image
