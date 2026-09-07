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
//     → ChunkCache (read path)
//     [→ SwitchSource with background Downloader when download.enable]
//
//   → TarOffsetSource (auto-detected tar wrapper)
//   → ZFileSource when is_zfile(), else the raw view
//   → LsmtLayer
//
// All layers are finally merged by MergedLsmt (topmost = last lower).
#pragma once

#include "image/config.hpp"
#include "source/blob_source.hpp"

#include <elio/coro/task.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace obd::image {

struct OpenedImage {
    source::BlobSourcePtr root;  // MergedLsmt: the block-device backing
    uint64_t virtual_size = 0;
    size_t layer_count = 0;
};

/// Assembles the merged read-only view for an image. Throws obd::error /
/// obd::format_error on any failure (a device that cannot assemble must not
/// come up half-broken).
elio::coro::task<OpenedImage> open_image(const ImageConfig& cfg,
                                         const GlobalConfig& global);

}  // namespace obd::image
