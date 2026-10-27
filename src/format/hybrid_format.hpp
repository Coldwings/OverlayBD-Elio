// Bounded replay of upstream Hybrid mapping logs. No filesystem operations.
#pragma once

#include "format/lsmt_format.hpp"

#include <map>
#include <span>
#include <vector>

namespace obd::format::hybrid {

constexpr uint32_t kHybridFlag = 1u << 5;
constexpr uint64_t kNoPhysicalOffset = bytes::segment_mapping::kMaxMoffset;
constexpr uint64_t kMaxLogRecords = 4 * lsmt::kMaxRoIndexSize;

/// Validate before allocating a buffer from the on-disk index length. Returns
/// the record count; partial records and excessive replay work fail closed.
uint64_t validate_pair(const lsmt::HeaderTrailer& data,
                       const lsmt::HeaderTrailer& index,
                       uint64_t data_bytes, uint64_t index_bytes);

/// Consume bounded chunks after validate_pair(). Records are applied in log
/// order, with later ranges replacing earlier ones. finish() verifies physical
/// ownership and normalizes foreign ZERO markers that are not exclusive backing.
class Replay {
public:
    Replay(uint64_t virtual_size, uint64_t data_bytes);
    void append(std::span<const uint8_t> records);
    std::vector<bytes::segment_mapping> finish() const;

private:
    uint64_t sectors_;
    uint64_t data_sectors_;
    uint64_t records_ = 0;
    std::map<uint64_t, bytes::segment_mapping> live_;
};

}  // namespace obd::format::hybrid
