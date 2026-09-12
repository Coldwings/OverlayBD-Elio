// Writers for the OverlayBD read-only formats, used by obd-mkimage and the
// test fixtures. These are synchronous cold-path utilities (plain POSIX IO);
// the data plane never writes.
//
// Produced files are byte-compatible with upstream overlaybd readers:
//   * write_lsmt_single_layer: a sealed single-layer LSMT covering the whole
//     input (one segment per <= 16383-sector chunk).
//   * write_zfile: a sealed ZFile with LZ4 or ZSTD raw-block compression,
//     optional per-block crc32c_salt and header/index digests.
#pragma once

#include "format/zfile_format.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace obd::format {

/// Generates a random lowercase 8-4-4-4-12 UUID string (36 chars).
std::string generate_uuid();

struct LsmtWriteOptions {
    std::string uuid;         // generated when empty
    std::string parent_uuid;  // may be empty (bottom layer)
    std::string user_tag;     // commit message, may be empty
};

/// Builds a sealed single-layer LSMT file at `out_path` from the raw image
/// accessible via `in_fd` (read from offset 0; `in_size` must be a non-zero
/// multiple of 512). Throws obd::error / obd::format_error on failure.
void write_lsmt_single_layer(int in_fd, uint64_t in_size,
                             const std::string& out_path,
                             const LsmtWriteOptions& opts = {});

/// Writes native warp metadata. Input tag 0 addresses the raw filesystem
/// fd; tag 1 addresses sectors in the original (uncompressed) target tar.
/// Only nonzero tag-0 extents are copied, compactly, into the output.
/// Mappings must be sorted, disjoint, nonempty, and wire-representable.
/// A tag-0 mapping is required when tag 1 is present (upstream mode 3).
/// All structural validation precedes output mutation; target size is
/// checked later by open_warp, since the target is not supplied here.
void write_lsmt_warp_layer(int metadata_fd, uint64_t virtual_size,
                          const std::vector<bytes::segment_mapping>& mappings,
                          const std::string& out_path,
                          const LsmtWriteOptions& opts = {});

struct ZFileWriteOptions {
    uint32_t block_size = 4096;       // power of two, <= 65536
    uint8_t algo = zfile::kAlgoLz4;   // kAlgoLz4 or kAlgoZstd
    uint8_t level = 3;                // zstd level; ignored for lz4
    bool verify = true;               // append crc32c_salt per block
    bool calc_digest = true;          // header/trailer digest + index crc
};

/// Compresses the file accessible via `in_fd` (`in_size` bytes, read from
/// offset 0) into a sealed ZFile at `out_path`. Throws obd::error /
/// obd::format_error on failure.
void write_zfile(int in_fd, uint64_t in_size, const std::string& out_path,
                 const ZFileWriteOptions& opts = {});

}  // namespace obd::format
