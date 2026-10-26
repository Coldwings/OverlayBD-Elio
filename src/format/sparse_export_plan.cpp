#include "format/sparse_export_plan.hpp"
#include "format/sparse_export_fault.hpp"
#include "format/lsmt_format.hpp"
#include "common/errors.hpp"
#include "common/sha256.hpp"
#include <elio/io/file_transfer.hpp>
#include <elio/io/file_operations.hpp>
#include <algorithm>
#include <array>
#include <limits>

namespace obd::format::sparse_export_detail {
Plan make_plan(uint64_t virtual_size,
               const std::vector<bytes::segment_mapping>& ownership) {
    using Mapping = bytes::segment_mapping;
    if (virtual_size == 0 || virtual_size % 512 != 0 ||
        virtual_size / 512 > Mapping::kMaxOffset)
        throw error(EINVAL, "sparse export: invalid virtual size");
    if (ownership.size() > lsmt::kMaxRoIndexSize)
        throw error(EFBIG, "sparse export: too many ownership mappings");
    const auto sectors = virtual_size / 512;
    uint64_t end = 0;
    for (const auto& m : ownership) {
        if (!m.length || m.length > Mapping::kMaxLength || m.offset < end ||
            m.offset >= sectors || m.length > sectors - m.offset || m.tag != 0 ||
            (!m.zeroed && m.moffset != m.offset))
            throw error(EINVAL, "sparse export: invalid exact ownership");
        end = m.end();
    }
    Plan plan;
    plan.virtual_size = virtual_size;
    plan.mappings.reserve(ownership.size());
    uint64_t physical = lsmt::kDataStartSector;
    for (size_t i = 0; i < ownership.size();) {
        const bool zero = ownership[i].zeroed;
        uint64_t first = ownership[i].offset;
        uint64_t last = ownership[i].end();
        ++i;
        while (i < ownership.size() && ownership[i].offset == last &&
               ownership[i].zeroed == zero) {
            last = ownership[i].end();
            ++i;
        }
        while (first < last) {
            if (plan.mappings.size() == lsmt::kMaxRoIndexSize)
                throw error(EFBIG, "sparse export: too many packed mappings");
            const auto count = static_cast<uint32_t>(
                std::min<uint64_t>(last - first, Mapping::kMaxLength));
            plan.mappings.push_back({first, count, physical, zero, 0});
            if (!zero) physical += count;
            first += count;
        }
    }
    // Virtual size is bounded to the 50-bit logical sector space, so these
    // additions cannot overflow uint64_t. Still enforce signed file offsets.
    plan.payload_bytes = (physical - lsmt::kDataStartSector) * 512;
    plan.index_offset = (physical * 512 + 4095) / 4096 * 4096;
    const uint64_t index_bytes = plan.mappings.size() * 16;
    plan.trailer_offset = plan.index_offset + (index_bytes + 4095) / 4096 * 4096;
    plan.file_bytes = plan.trailer_offset + 4096;
    if (plan.file_bytes > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        throw error(EFBIG, "sparse export: output exceeds file offset range");
    return plan;
}

namespace {
elio::coro::task<void> write_exact(int fd, const uint8_t* data, size_t count,
                                uint64_t offset) {
    const auto result = co_await elio::io::pwrite_exactly(fd,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), count), offset);
    if (result.error) throw error(result.error.value(), "sparse export: output write");
    if (result.end != elio::io::transfer_end::complete)
        throw error(EIO, "sparse export: incomplete output write");
}
bool valid_parent(const std::string& uuid) {
    if (uuid.empty()) return true;
    if (uuid.size() != 36) return false;
    for (size_t i = 0; i < uuid.size(); ++i) {
        const auto c = uuid[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}
}

elio::coro::task<EncodedDelta> write_delta(
    source::BlobSource& input, int staging_fd, uint64_t virtual_size,
    const std::vector<bytes::segment_mapping>& ownership,
    std::string parent_uuid, std::string user_tag) {
    const auto plan = make_plan(virtual_size, ownership);
    if (input.size() > virtual_size || staging_fd < 0 || !valid_parent(parent_uuid) ||
        user_tag.size() >= 256 || user_tag.find('\0') != std::string::npos)
        throw error(EINVAL, "sparse export: invalid source or header inputs");
    for (const auto& m : plan.mappings)
        if (!m.zeroed && m.end() * 512 > input.size())
            throw error(EINVAL, "sparse export: DATA extends beyond source");
    // Allocate bounded working buffers before mutating staging. The plan's
    // mapping count is independently bounded; no virtual-capacity bitmap.
    std::vector<uint8_t> buffer(1 << 20);
    std::array<uint8_t, 4096> padding{};
    common::Sha256 content;
    uint8_t size_le[8];
    bytes::store_u64_le(size_le, virtual_size);
    content.update(size_le, sizeof(size_le));
    co_await write_exact(staging_fd, padding.data(), padding.size(), 0);
    for (const auto& mapping : plan.mappings) {
        if (mapping.zeroed) continue;
        const uint64_t bytes = uint64_t{mapping.length} * 512;
        for (uint64_t done = 0; done < bytes;) {
            const auto count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), bytes - done));
            const auto read = co_await input.pread(buffer.data(), count, mapping.offset * 512 + done);
            if (read < 0) throw error(static_cast<int>(-read), "sparse export: source read");
            if (read != static_cast<ssize_t>(count))
                throw error(EIO, "sparse export: incomplete source read");
            content.update(buffer.data(), count);
            const auto write_count = fault_at(Fault::payload) ? count / 2 : count;
            co_await write_exact(staging_fd, buffer.data(), write_count, mapping.moffset * 512 + done);
            check_fault(Fault::payload);
            done += count;
        }
    }
    const auto payload_end = 4096 + plan.payload_bytes;
    if (payload_end < plan.index_offset)
        co_await write_exact(staging_fd, padding.data(), plan.index_offset - payload_end, payload_end);
    constexpr size_t records_per_chunk = 4096;
    for (size_t first = 0; first < plan.mappings.size();) {
        const auto count = std::min(records_per_chunk, plan.mappings.size() - first);
        for (size_t i = 0; i < count; ++i)
            bytes::store_segment_le(buffer.data() + i * 16, plan.mappings[first + i]);
        content.update(buffer.data(), count * 16);
        co_await write_exact(staging_fd, buffer.data(), count * 16, plan.index_offset + first * 16);
        check_fault(Fault::index);
        first += count;
    }
    const auto index_end = plan.index_offset + plan.mappings.size() * 16;
    if (index_end < plan.trailer_offset)
        co_await write_exact(staging_fd, padding.data(), plan.trailer_offset - index_end, index_end);
    // Match the established deterministic-seal digest convention: vsize,
    // packed DATA and encoded index. Parent/tag remain explicit header inputs.
    auto uuid = content.final_hex().substr(0, 32);
    uuid.insert(20, 1, '-'); uuid.insert(16, 1, '-');
    uuid.insert(12, 1, '-'); uuid.insert(8, 1, '-');
    lsmt::HeaderTrailer header;
    header.flags = 7; // header, data file, sealed; no sparse/warp flags
    header.index_offset = plan.index_offset;
    header.index_size = plan.mappings.size();
    header.virtual_size = virtual_size;
    header.uuid = uuid;
    header.parent_uuid = std::move(parent_uuid);
    header.user_tag = std::move(user_tag);
    header.serialize(padding.data());
    co_await write_exact(staging_fd, padding.data(), padding.size(), 0);
    check_fault(Fault::header);
    header.clr_flag_bit(lsmt::kFlagShiftHeader);
    header.serialize(padding.data());
    co_await write_exact(staging_fd, padding.data(), padding.size(), plan.trailer_offset);
    check_fault(Fault::trailer);
    check_fault(Fault::file_sync);
    const auto synced = co_await elio::io::sync_file(staging_fd,
        elio::io::file_sync_mode::data_and_metadata);
    if (!synced) throw error(synced.error_value(), "sparse export: staging sync");
    co_return EncodedDelta{std::move(uuid), plan.file_bytes};
}
}  // namespace obd::format::sparse_export_detail
