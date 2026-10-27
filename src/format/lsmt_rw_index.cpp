#include "format/lsmt_rw_index.hpp"
#include "common/errors.hpp"

#include <algorithm>
#include <cctype>

namespace obd::format::lsmt_rw {
namespace {
using Mapping = bytes::segment_mapping;

bool valid_uuid(const std::string& uuid) {
    if (uuid.size() != 36) return false;
    for (size_t i = 0; i < uuid.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (uuid[i] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(uuid[i]))) {
            return false;
        }
    }
    return true;
}
}

uint64_t validate_pair(const lsmt::HeaderTrailer& data,
                       const lsmt::HeaderTrailer& index,
                       uint64_t data_bytes, uint64_t index_bytes) {
    if (data.flags != 3 || index.flags != 1 ||
        data.version != 1 || index.version != 1 ||
        data.sub_version != 1 || index.sub_version != 1 ||
        data.reserved != 0 || index.reserved != 0)
        throw format_error("lsmt rw: unsupported writable pair headers");
    if (!valid_uuid(data.uuid) || data.uuid != index.uuid ||
        data.parent_uuid != index.parent_uuid ||
        (!data.parent_uuid.empty() && !valid_uuid(data.parent_uuid)) ||
        data.virtual_size != index.virtual_size)
        throw format_error("lsmt rw: mismatched pair identity or capacity");
    if (data.virtual_size == 0 || data.virtual_size % lsmt::kAlignment != 0 ||
        data.virtual_size / lsmt::kAlignment > Mapping::kMaxOffset ||
        data_bytes < lsmt::kSpace || data_bytes % lsmt::kAlignment != 0 ||
        data_bytes / lsmt::kAlignment >= Mapping::kMaxMoffset)
        throw format_error("lsmt rw: invalid data bounds");
    if (data.index_offset != 0 || data.index_size != 0 ||
        index.index_offset != lsmt::kSpace || index.index_size != 0 ||
        index_bytes < lsmt::kSpace ||
        (index_bytes - lsmt::kSpace) % Mapping::kEncodedSize != 0)
        throw format_error("lsmt rw: invalid writable index bounds");
    const auto count = (index_bytes - lsmt::kSpace) / Mapping::kEncodedSize;
    if (count > kMaxLogRecords)
        throw format_error("lsmt rw: index exceeds replay limit");
    return count;
}

Replay::Replay(uint64_t virtual_size, uint64_t data_bytes)
    : sectors_(virtual_size / lsmt::kAlignment),
      data_sectors_(data_bytes / lsmt::kAlignment) {
    if (virtual_size == 0 || virtual_size % lsmt::kAlignment != 0 ||
        sectors_ > Mapping::kMaxOffset || data_bytes < lsmt::kSpace ||
        data_bytes % lsmt::kAlignment != 0 || data_sectors_ >= Mapping::kMaxMoffset)
        throw format_error("lsmt rw: invalid replay bounds");
}

void Replay::append(std::span<const uint8_t> records) {
    if (records.size() % Mapping::kEncodedSize != 0 ||
        records.size() / Mapping::kEncodedSize > kMaxLogRecords - records_)
        throw format_error("lsmt rw: invalid replay chunk");
    records_ += records.size() / Mapping::kEncodedSize;
    for (size_t pos = 0; pos < records.size(); pos += Mapping::kEncodedSize) {
        auto m = bytes::load_segment_le(records.data() + pos);
        // Upstream uses INVALID_OFFSET as padding irrespective of other bits.
        if (m.offset == Mapping::kInvalidOffset) continue;
        if (m.length == 0 || m.offset >= sectors_ || m.length > sectors_ - m.offset)
            throw format_error("lsmt rw: invalid logical mapping");
        if (!m.zeroed && (m.moffset < lsmt::kDataStartSector ||
            m.moffset >= data_sectors_ || m.length > data_sectors_ - m.moffset))
            throw format_error("lsmt rw: DATA mapping outside payload");
        if (m.zeroed) m.moffset = lsmt::kDataStartSector;
        m.tag = 0;
        auto it = live_.lower_bound(m.offset);
        if (it != live_.begin()) {
            auto previous = std::prev(it);
            if (previous->second.end() > m.offset) it = previous;
        }
        std::vector<Mapping> retained;
        while (it != live_.end() && it->first < m.end()) {
            const auto old = it->second;
            if (old.offset < m.offset) {
                auto left = old;
                left.length = static_cast<uint32_t>(m.offset - old.offset);
                retained.push_back(left);
            }
            if (old.end() > m.end()) {
                auto right = old;
                right.offset = m.end();
                right.length = static_cast<uint32_t>(old.end() - m.end());
                if (!right.zeroed) right.moffset += m.end() - old.offset;
                retained.push_back(right);
            }
            it = live_.erase(it);
        }
        for (const auto& old : retained) live_.emplace(old.offset, old);
        live_.emplace(m.offset, m);
        if (live_.size() > lsmt::kMaxRoIndexSize)
            throw format_error("lsmt rw: live index exceeds limit");
    }
}

std::vector<Mapping> Replay::finish() const {
    std::vector<Mapping> result;
    std::vector<std::pair<uint64_t, uint64_t>> physical;
    result.reserve(live_.size());
    for (const auto& [offset, m] : live_) {
        (void)offset;
        result.push_back(m);
        if (!m.zeroed) physical.emplace_back(m.moffset, m.mend());
    }
    std::sort(physical.begin(), physical.end());
    for (size_t i = 1; i < physical.size(); ++i)
        if (physical[i].first < physical[i - 1].second)
            throw format_error("lsmt rw: live DATA extents alias");
    return result;
}
}  // namespace obd::format::lsmt_rw
