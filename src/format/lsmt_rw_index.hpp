// Upstream append-type LSMT writable index decoding; no filesystem operations.
#pragma once

#include "format/lsmt_format.hpp"
#include <map>
#include <span>
#include <vector>

namespace obd::format::lsmt_rw {

inline constexpr uint64_t kMaxLogRecords = 4 * lsmt::kMaxRoIndexSize;

// Validate both artifacts before replay. The index consists of its 4096-byte
// header followed by raw 16-byte SegmentMapping records, as in upstream LSMT.
uint64_t validate_pair(const lsmt::HeaderTrailer& data,
                       const lsmt::HeaderTrailer& index,
                       uint64_t data_bytes, uint64_t index_bytes);

class Replay {
public:
    Replay(uint64_t virtual_size, uint64_t data_bytes);
    void append(std::span<const uint8_t> records);
    // In-place writers require disjoint physical DATA ownership. ZERO records
    // have no backing and are normalized to the ordinary LSMT placeholder.
    std::vector<bytes::segment_mapping> finish() const;

private:
    uint64_t sectors_;
    uint64_t data_sectors_;
    uint64_t records_ = 0;
    std::map<uint64_t, bytes::segment_mapping> live_;
};

}  // namespace obd::format::lsmt_rw
