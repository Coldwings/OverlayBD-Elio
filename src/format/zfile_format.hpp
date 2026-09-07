// ZFile on-disk format: header/trailer structures, flags, and the in-memory
// jump table.
//
// Byte-exact reimplementation of the OverlayBD ZFile format
// (containerd/overlaybd src/overlaybd/zfile/zfile.cpp and
// docs/specs/zfile_format_spec.md). Layout, little-endian throughout:
//
//   | Header (512B) | dict (optional) | block0 [+crc 4B] | ... | blockN
//     [+crc] | index (u32 array) | Trailer (512B) |
//
// HeaderTrailer occupies the first 96 bytes of each 512B region; the rest is
// zero padding. Field offsets:
//
//    0  u64   magic0 = bytes 5A 46 69 6C 65 00 01 00 ("ZFile\0\1\0")
//    8  16B   magic1 = bytes 74 75 6A 69 2E 79 79 66 40 41 6C 69 62 61 62 61
//   24  u32   size = 96
//   28  u32   digest   crc32c over the whole 512B region with digest=0
//   32  u64   flags
//   40  u64   index_offset        byte offset of the index region
//   48  u64   index_size          ENTRY COUNT (on-disk bytes = index_size*4)
//   56  u64   original_file_size  uncompressed size
//   64  u32   index_crc           crc32c over the index region bytes
//   68  u32   reserved_0
//   72  CompressOptions (24B):
//       72 u32 block_size   (default 4096)
//       76 u8  algo         0=MINI_LZO(legacy) 1=LZ4 2=ZSTD
//       77 u8  level
//       78 u8  use_dict
//       79 u8  padding
//       80 u32 reserved
//       84 u32 dict_size
//       88 u8  verify       each block carries a trailing 4B crc32c_salt
//       89 u8[7] padding
//
// Flags: bit0 is_header (1 header / 0 trailer), bit1 data_file (1) /
// index_file (0), bit2 sealed, bit3 header_overwrite (trailer info copied
// back into the header), bit4 calc_digest, bit5 index compressed.
#pragma once

#include "common/bytes.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace obd::format::zfile {

constexpr uint32_t kSpace = 512;         // header/trailer region size
constexpr uint32_t kHeaderSize = 96;     // HeaderTrailer::size field value
constexpr uint64_t kMaxIndexSize = 1000000000ULL;  // MAX_ZFILE_INDEX_SIZE
constexpr uint32_t kNoiWellKnownPrime = 100007;    // crc32c_salt seed

/// magic0: "ZFile\0\1" plus the string literal's terminating NUL, read as 8
/// raw bytes (overlaybd zfile.cpp MAGIC0()).
constexpr uint8_t kMagic0[8] = {0x5A, 0x46, 0x69, 0x6C, 0x65, 0x00, 0x01, 0x00};

/// magic1: UUID {0x696a7574,0x792e,0x6679,0x4140,{0x6c,0x69,0x62,0x61,0x62,0x61}}
/// serialized by GCC as 16 little-endian bytes ("tuji.yyf@Alibaba").
constexpr uint8_t kMagic1[16] = {0x74, 0x75, 0x6A, 0x69, 0x2E, 0x79, 0x79, 0x66,
                                 0x40, 0x41, 0x6C, 0x69, 0x62, 0x61, 0x62, 0x61};

// Flag bit positions (overlaybd zfile.cpp FLAG_SHIFT_*).
constexpr int kFlagShiftHeader = 0;
constexpr int kFlagShiftType = 1;  // 1: data file, 0: index file
constexpr int kFlagShiftSealed = 2;
constexpr int kFlagShiftHeaderOverwrite = 3;
constexpr int kFlagShiftCalcDigest = 4;
constexpr int kFlagShiftIdxComp = 5;

enum Algo : uint8_t {
    kAlgoMiniLzo = 0,  // legacy, unsupported by this implementation
    kAlgoLz4 = 1,
    kAlgoZstd = 2,
};

/// CompressOptions, 24 bytes on disk at HeaderTrailer offset 72.
struct CompressOptions {
    uint32_t block_size = 4096;
    uint8_t algo = kAlgoLz4;
    uint8_t level = 0;      // zstd level; lz4 ignores
    uint8_t use_dict = 0;   // dictionary support: parsed, rejected at open
    uint32_t reserved = 0;
    uint32_t dict_size = 0;
    uint8_t verify = 0;     // per-block trailing crc32c_salt

    void parse(const void* p);
    void serialize(void* p) const;
};

/// Parsed HeaderTrailer (the 96 meaningful bytes of a 512B region).
struct HeaderTrailer {
    uint32_t size = kHeaderSize;
    uint32_t digest = 0;
    uint64_t flags = 0;
    uint64_t index_offset = 0;
    uint64_t index_size = 0;  // entry count
    uint64_t original_file_size = 0;
    uint32_t index_crc = 0;
    uint32_t reserved_0 = 0;
    CompressOptions opt;

    bool get_flag_bit(int shift) const { return (flags >> shift) & 1ULL; }
    void set_flag_bit(int shift) { flags |= 1ULL << shift; }
    void clr_flag_bit(int shift) { flags &= ~(1ULL << shift); }

    bool is_header() const { return get_flag_bit(kFlagShiftHeader); }
    bool is_trailer() const { return !is_header(); }
    bool is_data_file() const { return get_flag_bit(kFlagShiftType); }
    bool is_sealed() const { return get_flag_bit(kFlagShiftSealed); }
    bool is_header_overwrite() const { return get_flag_bit(kFlagShiftHeaderOverwrite); }
    bool is_digest_enabled() const { return get_flag_bit(kFlagShiftCalcDigest); }

    /// Parses a 512-byte region. Throws obd::format_error on bad magic or
    /// bad size field. Does NOT verify the digest (see region_digest_ok).
    static HeaderTrailer parse(const void* region);

    /// Serializes into a 512-byte region: 96 payload bytes plus zero
    /// padding. The digest field is written as-is (use set_region_digest
    /// afterwards when calc_digest is enabled).
    void serialize(void* region) const;

    /// True when the region's digest field equals crc32c over the whole
    /// 512B region computed with the digest field zeroed. Regions without
    /// the calc_digest flag trivially pass (overlaybd is_valid() warns and
    /// returns true in that case).
    bool region_digest_ok(const void* region) const;

    /// Computes the digest over `region` (digest field zeroed during the
    /// computation) and stores it both into the region and into this
    /// struct's digest field.
    void set_region_digest(void* region);
};

/// In-memory block offset index, byte-compatible with overlaybd's
/// CompressionFile::JumpTable: partial group base offsets plus u16 prefix
/// sums inside each group. group_size = 65536 / block_size (must be a power
/// of two, so block_size must be a power of two <= 65536).
class JumpTable {
public:
    using uinttype = uint16_t;
    static constexpr uinttype kUinttypeMax = UINT16_MAX;

    /// Builds from the on-disk u32 array of per-block compressed sizes
    /// (including the 4B crc when enable_crc). `offset_begin` is the byte
    /// offset of block 0 (512 + dict_size). Throws obd::format_error on
    /// invalid block sizes or u16 delta overflow (overlaybd build()
    /// semantics: EIO / ERANGE).
    void build(const uint32_t* ibuf, size_t n, uint64_t offset_begin,
               uint32_t block_size, bool enable_crc);

    /// Byte offset of block `idx` (idx in [0, n]; idx == n yields the end
    /// of the last block, i.e. the index region offset).
    uint64_t operator[](size_t idx) const {
        const size_t part_idx = idx / group_size_;
        const size_t inner_idx = idx & (group_size_ - 1);
        const uint64_t part = partial_offset_[part_idx];
        return inner_idx ? part + deltas_[idx] : part;
    }

    size_t size() const { return deltas_.size(); }
    int group_size() const { return group_size_; }

private:
    int group_size_ = 0;
    std::vector<uint64_t> partial_offset_;
    std::vector<uinttype> deltas_;
};

}  // namespace obd::format::zfile
