// ZFile format structures. See zfile_format.hpp for the on-disk layout.
#include "format/zfile_format.hpp"

#include "common/crc32c.hpp"
#include "common/errors.hpp"

#include <cstring>

namespace obd::format::zfile {

void CompressOptions::parse(const void* p) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    block_size = bytes::load_u32_le(b);
    algo = b[4];
    level = b[5];
    use_dict = b[6];
    // b[7] padding
    reserved = bytes::load_u32_le(b + 8);
    dict_size = bytes::load_u32_le(b + 12);
    verify = b[16];
    // b[17..23] padding
}

void CompressOptions::serialize(void* p) const {
    uint8_t* b = static_cast<uint8_t*>(p);
    bytes::store_u32_le(b, block_size);
    b[4] = algo;
    b[5] = level;
    b[6] = use_dict;
    b[7] = 0;
    bytes::store_u32_le(b + 8, reserved);
    bytes::store_u32_le(b + 12, dict_size);
    b[16] = verify;
    std::memset(b + 17, 0, 7);
}

HeaderTrailer HeaderTrailer::parse(const void* region) {
    const uint8_t* b = static_cast<const uint8_t*>(region);
    if (std::memcmp(b, kMagic0, sizeof(kMagic0)) != 0 ||
        std::memcmp(b + 8, kMagic1, sizeof(kMagic1)) != 0) {
        throw format_error("zfile magic mismatch");
    }
    HeaderTrailer ht;
    ht.size = bytes::load_u32_le(b + 24);
    if (ht.size != kHeaderSize) {
        throw format_error("zfile HeaderTrailer size field is " +
                           std::to_string(ht.size) + ", expected 96");
    }
    ht.digest = bytes::load_u32_le(b + 28);
    ht.flags = bytes::load_u64_le(b + 32);
    ht.index_offset = bytes::load_u64_le(b + 40);
    ht.index_size = bytes::load_u64_le(b + 48);
    ht.original_file_size = bytes::load_u64_le(b + 56);
    ht.index_crc = bytes::load_u32_le(b + 64);
    ht.reserved_0 = bytes::load_u32_le(b + 68);
    ht.opt.parse(b + 72);
    return ht;
}

void HeaderTrailer::serialize(void* region) const {
    uint8_t* b = static_cast<uint8_t*>(region);
    std::memset(region, 0, kSpace);
    std::memcpy(b, kMagic0, sizeof(kMagic0));
    std::memcpy(b + 8, kMagic1, sizeof(kMagic1));
    bytes::store_u32_le(b + 24, kHeaderSize);
    bytes::store_u32_le(b + 28, digest);
    bytes::store_u64_le(b + 32, flags);
    bytes::store_u64_le(b + 40, index_offset);
    bytes::store_u64_le(b + 48, index_size);
    bytes::store_u64_le(b + 56, original_file_size);
    bytes::store_u32_le(b + 64, index_crc);
    bytes::store_u32_le(b + 68, reserved_0);
    opt.serialize(b + 72);
}

bool HeaderTrailer::region_digest_ok(const void* region) const {
    if (!is_digest_enabled()) return true;  // overlaybd is_valid(): warn + pass
    uint8_t copy[kSpace];
    std::memcpy(copy, region, kSpace);
    bytes::store_u32_le(copy + 28, 0);
    return crc32::crc32c(copy, kSpace) == digest;
}

void HeaderTrailer::set_region_digest(void* region) {
    uint8_t* b = static_cast<uint8_t*>(region);
    bytes::store_u32_le(b + 28, 0);
    digest = crc32::crc32c(b, kSpace);
    bytes::store_u32_le(b + 28, digest);
}

void JumpTable::build(const uint32_t* ibuf, size_t n, uint64_t offset_begin,
                      uint32_t block_size, bool enable_crc) {
    // group_size must be a power of two: operator[] uses (group_size - 1) as
    // an index mask, so block_size must be a power of two <= 65536.
    if (block_size == 0 || block_size > 65536 ||
        (block_size & (block_size - 1)) != 0) {
        throw format_error("zfile block_size " + std::to_string(block_size) +
                           " is not a power of two <= 65536");
    }
    partial_offset_.clear();
    deltas_.clear();
    group_size_ = static_cast<int>((kUinttypeMax + 1) / block_size);
    partial_offset_.reserve(n / group_size_ + 1);
    deltas_.reserve(n + 1);
    uint64_t raw_offset = offset_begin;
    partial_offset_.push_back(raw_offset);
    deltas_.push_back(0);
    const uint32_t min_blksize = enable_crc ? sizeof(uint32_t) : 0;
    for (size_t i = 1; i < n + 1; i++) {
        if (ibuf[i - 1] <= min_blksize) {
            throw format_error("zfile index entry " + std::to_string(i - 1) +
                               " has invalid compressed size " +
                               std::to_string(ibuf[i - 1]), EIO);
        }
        raw_offset += ibuf[i - 1];
        if ((i % static_cast<size_t>(group_size_)) == 0) {
            partial_offset_.push_back(raw_offset);
            deltas_.push_back(0);
            continue;
        }
        if (static_cast<uint64_t>(deltas_[i - 1]) + ibuf[i - 1] >=
            static_cast<uint64_t>(kUinttypeMax)) {
            throw format_error("zfile jump table delta overflow at block " +
                               std::to_string(i - 1), ERANGE);
        }
        deltas_.push_back(static_cast<uinttype>(deltas_[i - 1] + ibuf[i - 1]));
    }
}

}  // namespace obd::format::zfile
