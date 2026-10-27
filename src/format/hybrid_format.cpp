#include "format/hybrid_format.hpp"

#include "common/errors.hpp"

#include <algorithm>
#include <cctype>

namespace obd::format::hybrid {
namespace {
using Mapping = bytes::segment_mapping;

bool uuid_valid(const std::string& uuid) {
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

Mapping suffix(Mapping m, uint64_t start) {
    const auto delta = start - m.offset;
    m.offset = start;
    m.length -= static_cast<uint32_t>(delta);
    if (m.moffset != kNoPhysicalOffset) m.moffset += delta;
    return m;
}
}

uint64_t validate_pair(const lsmt::HeaderTrailer& data,
                       const lsmt::HeaderTrailer& index,
                       uint64_t data_bytes, uint64_t index_bytes) {
    if (data.flags != (kHybridFlag | 3u) || index.flags != (kHybridFlag | 1u))
        throw format_error("hybrid: unsupported writable header flags");
    if (data.version != 1 || index.version != 1 ||
        data.sub_version != 1 || index.sub_version != 1 ||
        data.reserved != 0 || index.reserved != 0)
        throw format_error("hybrid: unsupported writable header version");
    if (!uuid_valid(data.uuid) || data.uuid != index.uuid ||
        data.parent_uuid != index.parent_uuid ||
        (!data.parent_uuid.empty() && !uuid_valid(data.parent_uuid)) ||
        data.virtual_size != index.virtual_size)
        throw format_error("hybrid: mismatched or invalid artifact identity");
    if (data.virtual_size == 0 || data.virtual_size % lsmt::kAlignment != 0 ||
        data.virtual_size / lsmt::kAlignment > Mapping::kMaxOffset)
        throw format_error("hybrid: invalid virtual size");
    if (data.index_offset != 0 || data.index_size != 0 ||
        index.index_offset != lsmt::kSpace || index.index_size != 0)
        throw format_error("hybrid: invalid writable index header");
    if (data_bytes < lsmt::kSpace || data_bytes % lsmt::kAlignment != 0 ||
        data_bytes / lsmt::kAlignment >= kNoPhysicalOffset ||
        index_bytes < lsmt::kSpace ||
        (index_bytes - lsmt::kSpace) % Mapping::kEncodedSize != 0)
        throw format_error("hybrid: truncated or unaligned artifact");
    const auto count = (index_bytes - lsmt::kSpace) / Mapping::kEncodedSize;
    if (count > kMaxLogRecords)
        throw format_error("hybrid: mapping log exceeds replay limit");
    return count;
}

Replay::Replay(uint64_t virtual_size, uint64_t data_bytes)
    : sectors_(virtual_size / lsmt::kAlignment),
      data_sectors_(data_bytes / lsmt::kAlignment) {
    if (virtual_size == 0 || virtual_size % lsmt::kAlignment != 0 ||
        sectors_ > Mapping::kMaxOffset || data_bytes < lsmt::kSpace ||
        data_bytes % lsmt::kAlignment != 0 || data_sectors_ >= kNoPhysicalOffset)
        throw format_error("hybrid: invalid replay bounds");
}

void Replay::append(std::span<const uint8_t> records) {
    if (records.size() % Mapping::kEncodedSize != 0 ||
        records.size() / Mapping::kEncodedSize > kMaxLogRecords - records_)
        throw format_error("hybrid: invalid replay chunk or record limit");
    records_ += records.size() / Mapping::kEncodedSize;
    for (size_t pos = 0; pos < records.size(); pos += Mapping::kEncodedSize) {
        auto m = bytes::load_segment_le(records.data() + pos);
        if (m.offset == Mapping::kInvalidOffset && m.length == 0) {
            if (m.moffset != 0 || m.zeroed || m.tag != 0)
                throw format_error("hybrid: invalid index padding");
            continue;
        }
        if (m.length == 0 || m.offset >= sectors_ || m.length > sectors_ - m.offset)
            throw format_error("hybrid: invalid logical mapping");
        if (m.zeroed) {
            if (m.moffset != kNoPhysicalOffset &&
                (m.moffset < lsmt::kDataStartSector || m.moffset > data_sectors_))
                throw format_error("hybrid: invalid ZERO marker");
        } else if (m.moffset < lsmt::kDataStartSector ||
                   m.moffset >= data_sectors_ || m.length > data_sectors_ - m.moffset) {
            throw format_error("hybrid: data extent exceeds artifact");
        }
        m.tag = 0;  // Runtime layer tags are not persistent ownership.
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
            if (old.end() > m.end()) retained.push_back(suffix(old, m.end()));
            it = live_.erase(it);
        }
        for (const auto& old : retained) live_.emplace(old.offset, old);
        live_.emplace(m.offset, m);
        if (live_.size() > lsmt::kMaxRoIndexSize)
            throw format_error("hybrid: live index exceeds supported limit");
    }
}

std::vector<Mapping> Replay::finish() const {
    std::vector<Mapping> result;
    result.reserve(live_.size());
    for (const auto& [offset, mapping] : live_) {
        (void)offset;
        result.push_back(mapping);
    }
    struct Extent { uint64_t start, end; size_t index; };
    std::vector<Extent> physical;
    physical.reserve(result.size());
    for (size_t i = 0; i < result.size(); ++i) {
        auto& m = result[i];
        if (m.zeroed && (m.moffset == kNoPhysicalOffset ||
            m.moffset > data_sectors_ || m.length > data_sectors_ - m.moffset)) {
            m.moffset = kNoPhysicalOffset;
            continue;
        }
        physical.push_back({m.moffset, m.moffset + m.length, i});
    }
    std::sort(physical.begin(), physical.end(), [](const Extent& a, const Extent& b) {
        return a.start < b.start;
    });
    uint64_t prior_end = 0, prior_data_end = 0;
    for (size_t i = 0; i < physical.size(); ++i) {
        const auto& extent = physical[i];
        auto& m = result[extent.index];
        if (!m.zeroed) {
            if (extent.start < prior_data_end)
                throw format_error("hybrid: live DATA extents alias");
            prior_data_end = extent.end;
        } else if (extent.start < prior_end ||
                   (i + 1 < physical.size() && physical[i + 1].start < extent.end)) {
            m.moffset = kNoPhysicalOffset;
        }
        prior_end = std::max(prior_end, extent.end);
    }
    return result;
}

}  // namespace obd::format::hybrid
