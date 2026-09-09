// Image assembly: an overlaybd-compatible config.json becomes one merged
// read-only block source (docs/image.md).
//
// Per lower (bottom-up, as listed in the config):
//
//   local probe (lower.file, then dir/overlaybd.commit, dir/.commit,
//   dir/overlaybd.sealed)
//     → LocalFileSource
//   else remote chain:
//     RegistrySource (shared RegistryClient; DART accelerate prefix when
//     p2pConfig is enabled and reachable)
//     → LayerStore (ADR-0011: read-through persistence into lower.dir,
//       staging pair renamed to overlaybd.commit on completion, optional
//       background fill from the download section; a lower without a dir —
//       or with a dir the store cannot open — is served remote-only,
//       ADR-0016)
//
//   → TarOffsetSource (auto-detected tar wrapper)
//   → ZFileSource when is_zfile(), else the raw view
//   → LsmtLayer
//
// All layers are finally merged by MergedLsmt (topmost = last lower).
// With a non-empty `upper` (ADR-0008) the root instead becomes a
// MergedWritable whose topmost layer is the writable upper — reads fall
// through to the lowers, writes land in the upper.
//
// Trace layer (ADR-0013): when the config carries
// `accelerationLayer: true`, the UPPERMOST lower is the acceleration
// (trace) layer — it is set aside from the merge (not a data layer) and
// its trace blob is replayed as populate() warm-up on the data lowers'
// source chains (trace-format.md §6). Replay is opportunistic: a
// missing/malformed trace never fails assembly.
//
// Structural warm-up (ADR-0012's cold-start floor): before replay, the
// head/tail windows of every data lower's stored-blob view are
// populated (structural_warmup.hpp) — bounded, scavenger-class, and
// opportunistic like replay; `prefetch.enable` gates both.
#pragma once

#include "image/config.hpp"
#include "image/structural_warmup.hpp"
#include "image/trace_record.hpp"
#include "image/trace_replay.hpp"
#include "source/admission.hpp"
#include "source/blob_source.hpp"
#include "source/layer_store.hpp"

#include <elio/coro/task.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace obd::image {

struct OpenedImage {
    source::BlobSourcePtr root;  // MergedLsmt or MergedWritable
    uint64_t virtual_size = 0;
    size_t layer_count = 0;      // data layers (trace layer excluded)
    bool writable = false;       // root is a WritableBlobSource (ADR-0008)
    std::string upper_path;      // the writable layer file, when writable
    TraceReplayStats trace;      // ADR-0013 replay outcome (all zero when
                                 // no acceleration layer was configured)
    StructuralWarmupStats warmup;  // ADR-0012 structural warm-up outcome
                                 // (all zero when prefetch.enable is off)
    /// The layer_stores vector holds non-owning handles to every
    /// LayerStore in the chain (owned by `root`), for lifecycle
    /// operations: background fills must be parked (call park_image_fills)
    /// before the chain is destroyed on any path other than process
    /// exit — see the LayerStore lifetime contract.
    std::vector<source::LayerStore*> layer_stores;
    /// The device's read admission funnel (ADR-0012), shared by every
    /// LayerStore and remote-only source of this image (kept alive by
    /// the chains; this handle is for observability and tests).
    source::AdmissionFunnelPtr funnel;
    /// The trace recorder (ADR-0013, record path): always present, idle
    /// until the supervisor's trace_start arms it. Every remote lower's
    /// registry source is tapped through it (trace_record.hpp). Owned
    /// here so the device process can drive start/stop from the control
    /// channel; outlives the chain (taps hold a shared_ptr too).
    TraceRecorderPtr recorder;
};

/// Assembles the merged read-only view for an image. Throws obd::error /
/// obd::format_error on any failure (a device that cannot assemble must not
/// come up half-broken).
elio::coro::task<OpenedImage> open_image(const ImageConfig& cfg,
                                         const GlobalConfig& global);

/// Parks every background fill in the assembled chain: stop_fill() on
/// each store, then a bounded wait for a terminal fill_status. Call before
/// destroying `opened.root` on any path other than process exit (device
/// shutdown, tests) — destroying a store with a fill in flight is a
/// use-after-free (the fill coroutine touches members on resume).
elio::coro::task<void> park_image_fills(const OpenedImage& opened);

}  // namespace obd::image
