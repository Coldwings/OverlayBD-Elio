// LSMT format structures. See lsmt_format.hpp for the on-disk layout.
#include "format/lsmt_format.hpp"

#include "common/errors.hpp"

#include <cstring>

namespace obd::format::lsmt {

namespace {

/// Reads a fixed-size NUL-terminated string field.
std::string read_cstr(const uint8_t* b, size_t maxlen) {
    size_t len = 0;
    while (len < maxlen && b[len] != 0) len++;
    return std::string(reinterpret_cast<const char*>(b), len);
}

/// Writes a string into a fixed-size NUL-padded field.
void write_cstr(uint8_t* b, size_t maxlen, const std::string& s) {
    if (s.size() >= maxlen) {
        throw format_error("lsmt string field overflow: '" + s + "'");
    }
    std::memset(b, 0, maxlen);
    std::memcpy(b, s.data(), s.size());
}

}  // namespace

HeaderTrailer HeaderTrailer::parse(const void* region) {
    const uint8_t* b = static_cast<const uint8_t*>(region);
    if (std::memcmp(b, kMagic0, sizeof(kMagic0)) != 0 ||
        std::memcmp(b + 8, kMagic1, sizeof(kMagic1)) != 0) {
        throw format_error("lsmt magic mismatch");
    }
    HeaderTrailer ht;
    ht.size = bytes::load_u32_le(b + 24);
    if (ht.size != kHeaderSize) {
        throw format_error("lsmt HeaderTrailer size field is " +
                           std::to_string(ht.size) + ", expected 390");
    }
    ht.flags = bytes::load_u32_le(b + 28);
    ht.index_offset = bytes::load_u64_le(b + 32);
    ht.index_size = bytes::load_u64_le(b + 40);
    ht.virtual_size = bytes::load_u64_le(b + 48);
    ht.uuid = read_cstr(b + 56, 37);
    ht.parent_uuid = read_cstr(b + 93, 37);
    ht.reserved = bytes::load_u16_le(b + 130);
    ht.version = b[132];
    ht.sub_version = b[133];
    ht.user_tag = read_cstr(b + 134, 256);
    return ht;
}

void HeaderTrailer::serialize(void* region) const {
    uint8_t* b = static_cast<uint8_t*>(region);
    std::memset(region, 0, kSpace);
    std::memcpy(b, kMagic0, sizeof(kMagic0));
    std::memcpy(b + 8, kMagic1, sizeof(kMagic1));
    bytes::store_u32_le(b + 24, kHeaderSize);
    bytes::store_u32_le(b + 28, flags);
    bytes::store_u64_le(b + 32, index_offset);
    bytes::store_u64_le(b + 40, index_size);
    bytes::store_u64_le(b + 48, virtual_size);
    write_cstr(b + 56, 37, uuid);
    write_cstr(b + 93, 37, parent_uuid);
    bytes::store_u16_le(b + 130, reserved);
    b[132] = version;
    b[133] = sub_version;
    write_cstr(b + 134, 256, user_tag);
}

}  // namespace obd::format::lsmt
