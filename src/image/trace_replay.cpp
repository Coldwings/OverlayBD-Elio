// Trace replay. See trace_replay.hpp for the contract.
#include "image/trace_replay.hpp"

#include "format/trace.hpp"

#include <elio/log/macros.hpp>

namespace obd::image {

elio::coro::task<TraceReplayStats> replay_trace(
    std::span<const uint8_t> blob,
    const std::vector<source::BlobSource*>& warm_targets,
    const TraceReplayOptions& opts) {
    TraceReplayStats stats;

    const auto parsed = format::trace::parse(blob);
    if (!parsed.has_value()) {
        // Opportunistic: a malformed trace degrades to "no prefetch"
        // (trace-format.md §7 — upstream's reload failure only disables
        // prefetch). The dynamic file-list fallback is not implemented
        // (ADR-0013).
        ELIO_LOG_WARNING("trace blob rejected ({}); continuing without "
                         "prefetch",
                         parsed.error().message);
        co_return stats;
    }
    stats.trace_present = true;
    stats.records_total = parsed->size();

    const auto deadline = std::chrono::steady_clock::now() + opts.max_wall_time;
    size_t processed = 0;
    for (const auto& rec : *parsed) {
        if (processed >= opts.max_records ||
            stats.bytes_requested >= opts.max_bytes ||
            std::chrono::steady_clock::now() >= deadline) {
            stats.budget_exhausted = true;
            break;
        }
        ++processed;

        // Upstream replay parity (trace-format.md §5, §8): non-READ ops
        // and unknown layer indexes are silently skipped.
        if (rec.op != format::trace::kOpRead) {
            ++stats.records_skipped;
            continue;
        }
        if (rec.layer_index >= warm_targets.size() ||
            warm_targets[rec.layer_index] == nullptr) {
            ++stats.records_skipped;
            continue;
        }
        // The parser performs no field validation (§7): replay must.
        // Zero-length records warm nothing; counts above the 1 MiB
        // conforming-writer cap are skipped (not clamped); negative
        // offsets are invalid.
        if (rec.count == 0 || rec.count > opts.max_record_count ||
            rec.offset < 0) {
            ++stats.records_skipped;
            continue;
        }

        const uint64_t remaining = opts.max_bytes - stats.bytes_requested;
        if (rec.count > remaining) {
            stats.budget_exhausted = true;
            break;
        }
        stats.bytes_requested += rec.count;

        const ssize_t r = co_await warm_targets[rec.layer_index]->populate(
            static_cast<uint64_t>(rec.offset),
            static_cast<size_t>(rec.count));
        if (r < 0) {
            // Opportunistic: a failed warm-up read is logged and skipped,
            // exactly like upstream's short/failed replay pread (§5).
            ELIO_LOG_DEBUG("trace replay populate failed ({}), record "
                           "skipped",
                           static_cast<int>(-r));
            ++stats.records_skipped;
            continue;
        }
        ++stats.records_replayed;
        stats.bytes_warmed += rec.count;
    }

    ELIO_LOG_INFO("trace replay: {}/{} records replayed ({} bytes requested, "
                  "{} bytes warmed, {} skipped{})",
                  stats.records_replayed, stats.records_total,
                  stats.bytes_requested, stats.bytes_warmed,
                  stats.records_skipped,
                  stats.budget_exhausted ? ", budget exhausted" : "");
    co_return stats;
}

}  // namespace obd::image
