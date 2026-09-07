// LSMT (overlaybd layer) on-disk format: header/trailer structures.
//
// Byte-exact reimplementation of the OverlayBD LSMT read-only format
// (containerd/overlaybd src/overlaybd/lsmt/file.cpp HeaderTrailer and
// docs/specs/lsmt_format_spec.md). Layout, little-endian throughout:
//
//   | Header (4096B) | data | index (SegmentMapping[16B] array) | Trailer
//     (4096B) |
//
// HeaderTrailer occupies the first 390 bytes of each 4096B region; the rest
// is zero padding. Field offsets:
//
//    0  u64      magic0 = bytes 4C 53 4D 54 00 01 02 00 ("LSMT\0\1\2\0")
//    8  16B      magic1 = bytes 65 7E 63 D2 94 44 08 4C A2 D2 C8 EC 4F CF
//                 AE 8A
//   24  u32      size = 390
//   28  u32      flags
//   32  u64      index_offset   byte offset of the index region
//   40  u64      index_size     SegmentMapping entry count (bytes = *16)
//   48  u64      virtual_size   virtual device size in bytes
//   56  char[37] uuid           layer UUID, NUL-terminated string
//   93  char[37] parent_uuid
//  130  u16      reserved
//  132  u8       version = 1
//  133  u8       sub_version = 1
//  134  char[256] user_tag      commit message, NUL-padded
//
// Flags: bit0 is_header (1 header / 0 trailer), bit1 data_file (1) /
// index_file (0), bit2 sealed, bit3 gc_layer, bit4 sparse_rw, bit5
// info_valid.
//
// All index offsets/lengths are in 512-byte sectors. The data region starts
// at sector 8 (byte 4096) and a valid moffset lies in
// [8, index_offset/512).
#pragma once

#include "common/bytes.hpp"

#include <cstdint>
#include <string>

namespace obd::format::lsmt {

constexpr uint32_t kSpace = 4096;      // header/trailer region size
constexpr uint32_t kHeaderSize = 390;  // HeaderTrailer::size field value
constexpr uint32_t kAlignment = 512;   // sector size
constexpr uint64_t kMaxRoIndexSize = 1000000;   // MAX_LSMT_RO_INDEX_SIZE
constexpr uint64_t kMaxStackLayers = 255;       // MAX_STACK_LAYERS
/// First sector of the data region (byte 4096 / 512).
constexpr uint64_t kDataStartSector = kSpace / kAlignment;

/// magic0: "LSMT\0\1\2" plus the string literal's terminating NUL, as 8 raw
/// bytes (overlaybd lsmt/file.cpp MAGIC0()).
constexpr uint8_t kMagic0[8] = {0x4C, 0x53, 0x4D, 0x54, 0x00, 0x01, 0x02, 0x00};

/// magic1: UUID {0xd2637e65,0x4494,0x4c08,0xd2a2,{0xc8,0xec,0x4f,0xcf,0xae,0x8a}}
/// serialized by GCC as 16 little-endian bytes.
constexpr uint8_t kMagic1[16] = {0x65, 0x7E, 0x63, 0xD2, 0x94, 0x44, 0x08, 0x4C,
                                 0xA2, 0xD2, 0xC8, 0xEC, 0x4F, 0xCF, 0xAE, 0x8A};

// Flag bit positions (overlaybd lsmt/file.cpp FLAG_SHIFT_*).
constexpr int kFlagShiftHeader = 0;
constexpr int kFlagShiftType = 1;  // 1: data file, 0: index file
constexpr int kFlagShiftSealed = 2;
constexpr int kFlagShiftGcLayer = 3;
constexpr int kFlagShiftSparseRw = 4;
constexpr int kFlagShiftInfoValid = 5;

/// Parsed LSMT HeaderTrailer (the 390 meaningful bytes of a 4096B region).
struct HeaderTrailer {
    uint32_t size = kHeaderSize;
    uint32_t flags = 0;
    uint64_t index_offset = 0;  // bytes
    uint64_t index_size = 0;    // entry count
    uint64_t virtual_size = 0;  // bytes
    std::string uuid;           // without NUL
    std::string parent_uuid;    // without NUL
    uint16_t reserved = 0;
    uint8_t version = 1;
    uint8_t sub_version = 1;
    std::string user_tag;  // without NUL padding

    bool get_flag_bit(int shift) const { return (flags >> shift) & 1u; }
    void set_flag_bit(int shift) { flags |= 1u << shift; }
    void clr_flag_bit(int shift) { flags &= ~(1u << shift); }

    bool is_header() const { return get_flag_bit(kFlagShiftHeader); }
    bool is_trailer() const { return !is_header(); }
    bool is_data_file() const { return get_flag_bit(kFlagShiftType); }
    bool is_sealed() const { return get_flag_bit(kFlagShiftSealed); }

    /// Parses a 4096-byte region. Throws obd::format_error on bad magic or
    /// size field. Strings are read as NUL-terminated within their fixed
    /// 37/256-byte fields.
    static HeaderTrailer parse(const void* region);

    /// Serializes into a 4096-byte region (390 payload bytes plus zero
    /// padding). Strings longer than the on-disk fields throw format_error.
    void serialize(void* region) const;
};

}  // namespace obd::format::lsmt
