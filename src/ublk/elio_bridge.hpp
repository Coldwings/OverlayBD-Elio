// Fixed-worker ublk owner and legacy test bridge (ADR-0023).
//
// The production owner creates and drives the raw ring on a fixed Elio
// worker. Its registered eventfd is awaited through Elio native I/O, and
// reusable per-tag handlers restore that worker before directly submitting
// COMMIT_AND_FETCH. The legacy bridge below remains for deterministic unit
// fixtures whose synchronous gates require a native thread.
//
// Op policy (read-only device, ADR-0007):
//   READ    → served from the block source; short reads zero-filled
//   FLUSH   → immediate success (nothing to persist)
//   others  → -EOPNOTSUPP (writes included: -EROFS for write ops)
#pragma once

#include "source/blob_source.hpp"
#include "ublk/queue.hpp"

#include <elio/coro/task.hpp>

#include <memory>

namespace obd::ublk {

/// Runs the bridge loop for `q` against block source `src` until `stop`
/// becomes true (and the efd watch errors out on device teardown).
/// Intended to be launched independently with a native join handle. The
/// bridge owns every tag handler in a TaskGroup and drains it before returning;
/// Device stop then observes the bridge's final frame/capture destruction.
elio::coro::task<void> run_bridge(Queue* q, source::BlobSource* src,
                                  std::atomic<bool>* stop);

/// ADR-0023 owner: creates and exclusively drives the raw ublk ring on one
/// fixed Elio worker, and owns that queue's reusable tag handlers until their
/// source-buffer loans have retired. It remains alive after source drain to
/// consume STOP CQEs and prove raw-command retirement.
elio::coro::task<void> run_owner_pump(Queue* q, source::BlobSource* src,
                                      std::atomic<bool>* stop);

}  // namespace obd::ublk
