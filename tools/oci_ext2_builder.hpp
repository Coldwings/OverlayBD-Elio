#pragma once
#include "oci_layer_plan.hpp"
#include "common/bytes.hpp"
#include <memory>
#include <string>
#include <vector>

namespace obd::convert {
/// Cumulative, device-free filesystem used to emit differential warp layers.
/// Capacity is fixed up front; budget metadata, directory growth, xattrs,
/// payload and indirect blocks across the input plans. Final-link deletion
/// frees inode data and xattrs using libe2fs lifecycle APIs.
class OciExt2Builder {
public:
    OciExt2Builder(const std::string& raw_path, uint64_t virtual_size,
                   uint32_t inode_capacity);
    ~OciExt2Builder();
    OciExt2Builder(const OciExt2Builder&) = delete;
    OciExt2Builder& operator=(const OciExt2Builder&) = delete;
    /// Applies whiteouts to the lower namespace before additions. Returns
    /// only current-layer, still-live full-sector payload mappings (tag 1).
    /// All filesystem writes have been flushed before returning.
    std::vector<bytes::segment_mapping> apply(const LayerPlan& plan,
                                             const std::string& uncompressed_tar_path);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace obd::convert
