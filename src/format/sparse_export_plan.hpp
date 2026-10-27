// Internal deterministic layout plan for an exact sparse upper delta.
#pragma once
#include "common/bytes.hpp"
#include "source/blob_source.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace obd::format::sparse_export_detail {
struct Plan {
    uint64_t virtual_size = 0;
    uint64_t payload_bytes = 0;
    uint64_t index_offset = 0;
    uint64_t trailer_offset = 0;
    uint64_t file_bytes = 0;
    // Sorted logical mappings with packed DATA offsets and explicit ZERO.
    // DATA input bytes come from offset*512 in the stable sparse source.
    std::vector<bytes::segment_mapping> mappings;
};

// Validate identity DATA and disjoint logical ownership before any output IO.
// Adjacent ranges of the same state are combined before canonical wire-sized
// splitting, making the result independent of the input's fragment history.
Plan make_plan(uint64_t virtual_size,
               const std::vector<bytes::segment_mapping>& ownership);

struct EncodedDelta {
    std::string uuid;
    uint64_t file_bytes = 0;
};
// The caller owns a drained/checkpointed source and a fresh, empty staging fd.
// This internal writer does not publish a pathname or alter source ownership.
// Metadata setup/cleanup and source/destination alias admission belong to the
// export transaction. Native asynchronous IO writes and syncs the artifact.
elio::coro::task<EncodedDelta> write_delta(
    source::BlobSource& source, int staging_fd, uint64_t virtual_size,
    const std::vector<bytes::segment_mapping>& ownership,
    std::string parent_uuid, std::string user_tag);
}  // namespace obd::format::sparse_export_detail
