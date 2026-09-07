// Block codec implementations. LZ4 is always built; ZSTD is optional
// (OBD_ENABLE_ZSTD, on by default).
#include "format/block_codec.hpp"

#include "format/zfile_format.hpp"

#include <cerrno>
#include <cstring>

#include <lz4.h>
#if OBD_HAVE_ZSTD
#include <zstd.h>
#endif

namespace obd::format {
namespace {

class Lz4Codec final : public BlockCodec {
public:
    int decompress(const void* src, size_t src_len, void* dst,
                   size_t dst_capacity, size_t expected) override {
        if (dst_capacity < expected) return -EOVERFLOW;
        const int ret = LZ4_decompress_safe(static_cast<const char*>(src),
                                            static_cast<char*>(dst),
                                            static_cast<int>(src_len),
                                            static_cast<int>(dst_capacity));
        if (ret < 0) return -EIO;
        if (static_cast<size_t>(ret) != expected) return -EINVAL;
        return 0;
    }

    int compress(const void* src, size_t src_len, void* dst,
                 size_t dst_capacity) override {
        if (src_len > static_cast<size_t>(LZ4_MAX_INPUT_SIZE)) return -E2BIG;
        if (dst_capacity < compress_bound(src_len)) return -EOVERFLOW;
        const int ret = LZ4_compress_default(static_cast<const char*>(src),
                                             static_cast<char*>(dst),
                                             static_cast<int>(src_len),
                                             static_cast<int>(dst_capacity));
        if (ret <= 0) return -EIO;
        return ret;
    }

    size_t compress_bound(size_t src_len) const override {
        return static_cast<size_t>(LZ4_compressBound(static_cast<int>(src_len)));
    }

    const char* name() const override { return "lz4"; }
};

#if OBD_HAVE_ZSTD
class ZstdCodec final : public BlockCodec {
public:
    explicit ZstdCodec(uint8_t level)
        : level_(level ? level : 3) {}  // overlaybd zfile uses zstd level 3

    int decompress(const void* src, size_t src_len, void* dst,
                   size_t dst_capacity, size_t expected) override {
        if (dst_capacity < expected) return -EOVERFLOW;
        const size_t ret = ZSTD_decompress(dst, expected, src, src_len);
        if (ZSTD_isError(ret)) return -EIO;
        if (ret != expected) return -EINVAL;
        return 0;
    }

    int compress(const void* src, size_t src_len, void* dst,
                 size_t dst_capacity) override {
        if (dst_capacity < compress_bound(src_len)) return -EOVERFLOW;
        const size_t ret =
            ZSTD_compress(dst, dst_capacity, src, src_len, level_);
        if (ZSTD_isError(ret)) return -EIO;
        return static_cast<int>(ret);
    }

    size_t compress_bound(size_t src_len) const override {
        return ZSTD_compressBound(src_len);
    }

    const char* name() const override { return "zstd"; }

private:
    int level_;
};
#endif

}  // namespace

std::unique_ptr<BlockCodec> create_block_codec(uint8_t algo, uint8_t level) {
    switch (algo) {
        case zfile::kAlgoLz4:
            return std::make_unique<Lz4Codec>();
#if OBD_HAVE_ZSTD
        case zfile::kAlgoZstd:
            return std::make_unique<ZstdCodec>(level);
#endif
        default:
            return nullptr;  // MINI_LZO legacy and unknown algos
    }
}

bool block_codec_available(uint8_t algo) {
    switch (algo) {
        case zfile::kAlgoLz4:
            return true;
#if OBD_HAVE_ZSTD
        case zfile::kAlgoZstd:
            return true;
#endif
        default:
            return false;
    }
}

}  // namespace obd::format
