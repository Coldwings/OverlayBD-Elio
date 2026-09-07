// Block compression codecs used by ZFile (raw-block framing, byte-compatible
// with overlaybd's compressor.cpp: LZ4_compress_default / ZSTD_compress with
// no container framing).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace obd::format {

/// Raw single-block codec. All methods are synchronous CPU work; callers on
/// coroutine hot paths should keep blocks small (ZFile block_size is
/// typically 4 KiB) or offload large batches.
class BlockCodec {
public:
    virtual ~BlockCodec() = default;

    /// Decompresses one raw block. `expected` is the exact uncompressed
    /// size. Returns 0 on success, -EIO on malformed input, -EINVAL on
    /// size mismatch, -EOVERFLOW when dst_capacity is insufficient.
    virtual int decompress(const void* src, size_t src_len, void* dst,
                           size_t dst_capacity, size_t expected) = 0;

    /// Compresses one raw block. Returns the compressed size (> 0), or a
    /// negative -errno on failure. dst_capacity must be at least
    /// compress_bound(src_len).
    virtual int compress(const void* src, size_t src_len, void* dst,
                         size_t dst_capacity) = 0;

    /// Upper bound on compress() output for a given input size.
    virtual size_t compress_bound(size_t src_len) const = 0;

    /// Codec name for logs ("lz4", "zstd").
    virtual const char* name() const = 0;
};

/// Creates a codec for a ZFile algo id (1 = LZ4, 2 = ZSTD). Returns nullptr
/// when the algo is unsupported in this build (MINI_LZO legacy, or ZSTD when
/// compiled without it).
std::unique_ptr<BlockCodec> create_block_codec(uint8_t algo, uint8_t level);

/// True when the build can decode `algo`.
bool block_codec_available(uint8_t algo);

}  // namespace obd::format
