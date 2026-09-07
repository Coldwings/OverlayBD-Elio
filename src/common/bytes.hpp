// Little-endian byte accessors for on-disk format parsing.
//
// All OverlayBD on-disk structures are little-endian (ZFile format_spec.md,
// LSMT format_spec.md). These helpers read/write fixed-width integers from
// unaligned raw buffers without type-punning through struct pointers, so
// parsing behaves identically on every supported platform/compiler. On
// little-endian targets (the only ones this project supports today) the
// byte assembly below compiles down to plain unaligned loads/stores.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace obd::bytes {

inline uint16_t load_u16_le(const void* p) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    return static_cast<uint16_t>(b[0]) | static_cast<uint16_t>(b[1]) << 8;
}

inline uint32_t load_u32_le(const void* p) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    return static_cast<uint32_t>(b[0]) | static_cast<uint32_t>(b[1]) << 8 |
           static_cast<uint32_t>(b[2]) << 16 | static_cast<uint32_t>(b[3]) << 24;
}

inline uint64_t load_u64_le(const void* p) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    return static_cast<uint64_t>(b[0]) | static_cast<uint64_t>(b[1]) << 8 |
           static_cast<uint64_t>(b[2]) << 16 | static_cast<uint64_t>(b[3]) << 24 |
           static_cast<uint64_t>(b[4]) << 32 | static_cast<uint64_t>(b[5]) << 40 |
           static_cast<uint64_t>(b[6]) << 48 | static_cast<uint64_t>(b[7]) << 56;
}

inline void store_u16_le(void* p, uint16_t v) {
    uint8_t* b = static_cast<uint8_t*>(p);
    b[0] = static_cast<uint8_t>(v);
    b[1] = static_cast<uint8_t>(v >> 8);
}

inline void store_u32_le(void* p, uint32_t v) {
    uint8_t* b = static_cast<uint8_t*>(p);
    b[0] = static_cast<uint8_t>(v);
    b[1] = static_cast<uint8_t>(v >> 8);
    b[2] = static_cast<uint8_t>(v >> 16);
    b[3] = static_cast<uint8_t>(v >> 24);
}

inline void store_u64_le(void* p, uint64_t v) {
    uint8_t* b = static_cast<uint8_t*>(p);
    b[0] = static_cast<uint8_t>(v);
    b[1] = static_cast<uint8_t>(v >> 8);
    b[2] = static_cast<uint8_t>(v >> 16);
    b[3] = static_cast<uint8_t>(v >> 24);
    b[4] = static_cast<uint8_t>(v >> 32);
    b[5] = static_cast<uint8_t>(v >> 40);
    b[6] = static_cast<uint8_t>(v >> 48);
    b[7] = static_cast<uint8_t>(v >> 56);
}

/// Decodes one on-disk LSMT SegmentMapping (16 bytes, little-endian bit
/// packing identical to overlaybd's packed GCC bitfields on LE targets):
///
///   word0: bits  0..49  offset   (logical offset, 512B sectors)
///          bits 50..63  length   (sectors, max 16383)
///   word1: bits  0..54  moffset  (mapped offset in the layer blob, sectors)
///          bit  55      zeroed   (segment reads as zeroes, no data)
///          bits 56..63  tag      (runtime: layer index after merge)
struct segment_mapping {
    uint64_t offset = 0;    ///< logical offset in 512B sectors (50 bits)
    uint32_t length = 0;    ///< length in sectors (14 bits, max 16383)
    uint64_t moffset = 0;   ///< blob offset in sectors (55 bits)
    bool zeroed = false;    ///< zero-filled segment
    uint8_t tag = 0;        ///< layer index (assigned at merge time)

    static constexpr uint64_t kMaxOffset = (uint64_t(1) << 50) - 1;
    static constexpr uint32_t kMaxLength = (1u << 14) - 1;
    static constexpr uint64_t kMaxMoffset = (uint64_t(1) << 55) - 1;
    static constexpr uint64_t kInvalidOffset = kMaxOffset;
    static constexpr size_t kEncodedSize = 16;

    uint64_t end() const { return offset + length; }
    uint64_t mend() const { return zeroed ? moffset : moffset + length; }
};

inline segment_mapping load_segment_le(const void* p) {
    const uint64_t w0 = load_u64_le(p);
    const uint64_t w1 = load_u64_le(static_cast<const uint8_t*>(p) + 8);
    segment_mapping s;
    s.offset = w0 & segment_mapping::kMaxOffset;
    s.length = static_cast<uint32_t>(w0 >> 50) & segment_mapping::kMaxLength;
    s.moffset = w1 & segment_mapping::kMaxMoffset;
    s.zeroed = (w1 >> 55) & 1;
    s.tag = static_cast<uint8_t>(w1 >> 56);
    return s;
}

inline void store_segment_le(void* p, const segment_mapping& s) {
    const uint64_t w0 = (s.offset & segment_mapping::kMaxOffset) |
                        (uint64_t(s.length & segment_mapping::kMaxLength) << 50);
    const uint64_t w1 = (s.moffset & segment_mapping::kMaxMoffset) |
                        (uint64_t(s.zeroed ? 1 : 0) << 55) | (uint64_t(s.tag) << 56);
    store_u64_le(p, w0);
    store_u64_le(static_cast<uint8_t*>(p) + 8, w1);
}

}  // namespace obd::bytes
