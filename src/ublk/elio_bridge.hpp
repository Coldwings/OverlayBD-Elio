// Elio bridge: the coroutine side of one ublk queue (ADR-0006).
//
// The bridge coroutine waits on the queue's elio_efd (registered with the
// Elio IO backend), pops pending requests, and fans each one out as its own
// coroutine so per-tag IO overlaps. Read handlers fill the tag's kernel-
// shared IO buffer from the merged block source and hand the result back to
// the queue thread via push_completion().
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
/// Intended to be spawned with elio::go() from the device process.
elio::coro::task<void> run_bridge(Queue* q, source::BlobSource* src,
                                  std::atomic<bool>* stop);

}  // namespace obd::ublk
