#pragma once

#include "common/bytes.hpp"

#include <cstdint>
#include <vector>

namespace obd::format {

// Validate allocation geometry/policy before enabling extent-owned writes.
// Blocking; requires the same exclusive ownership as the scanner.
void validate_sparse_extent_backing(int fd);

// Reconstruct a replacement-format Sparse upper from its current kernel view.
// HOLE inherits; UNWRITTEN masks the lower with ZERO after FIEMAP_FLAG_SYNC
// drains buffered writes. This is blocking: invoke off an Elio worker. The
// caller exclusively owns the regular file and has drained mutations through
// the entire scan. The first batch requests synchronization; later batches
// enumerate the same quiescent view. Synchronization/query errors propagate.
// Throws obd::error on unsupported/malformed extent responses or syscall error.
std::vector<bytes::segment_mapping> scan_sparse_extents(int fd, uint64_t size);

}  // namespace obd::format
