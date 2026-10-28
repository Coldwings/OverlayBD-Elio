// ZFile decompression view over a BlobSource.
//
// Reimplements the read-only side of overlaybd's CompressionFile
// (src/overlaybd/zfile/zfile.cpp): trailer-authoritative open, jump table,
// batched block reads, optional per-block crc32c_salt verification, LZ4/ZSTD
// raw-block decompression.
#pragma once

#include "format/block_codec.hpp"
#include "format/zfile_format.hpp"
#include "source/blob_source.hpp"

#include <elio/coro/task.hpp>

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace obd::format {

/// Probes whether `src` is a ZFile: reads the first 512 bytes and checks
/// magic plus (when the calc_digest flag is set) the region digest, matching
/// overlaybd's is_zfile(). Never throws; returns false on read errors or
/// short files.
elio::coro::task<bool> is_zfile(source::BlobSource& src);

/// Read-only decompression view. pread offsets/sizes are in the
/// UNCOMPRESSED address space [0, original_file_size).
class ZFileSource final : public source::BlobSource {
public:
    /// Opens a ZFile over `src` (ownership is taken). `verify` enables
    /// per-block crc32c_salt checks when the file itself carries verify
    /// checksums (overlaybd: verify = !is_local, i.e. remote data).
    /// Throws obd::format_error / obd::error on any validation failure.
    static elio::coro::task<std::unique_ptr<ZFileSource>> open(
        source::BlobSourcePtr src, bool verify);

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;

    uint64_t size() const noexcept override { return ht_.original_file_size; }
    std::string_view label() const noexcept override { return label_; }

    const zfile::HeaderTrailer& header() const noexcept { return ht_; }
    const zfile::JumpTable& jump_table() const noexcept { return jump_; }

    /// Maximum compressed bytes fetched from the underlying source per batch
    /// (overlaybd uses a 64 KiB read window; a larger window amortizes
    /// remote round-trips for ublk-sized requests).
    static constexpr size_t kReadWindow = 512 * 1024;

private:
    ZFileSource() = default;

    struct DecodedBlock {
        size_t index = 0;
        std::shared_ptr<const uint8_t[]> bytes;
    };
    // Eight retained blocks (at most 512 KiB). Active readers may outlive
    // eviction. Admission needs a repeat within the last sixteen miss keys.
    std::mutex decoded_mutex_;
    std::array<DecodedBlock, 8> decoded_{};
    size_t replacement_ = 0;
    std::array<size_t, 16> seen_{};
    size_t seen_count_ = 0;
    size_t seen_next_ = 0;

    std::shared_ptr<const uint8_t[]> decoded_get(size_t index,
                                                bool& admit) noexcept;
    void decoded_put(size_t index,
                     std::shared_ptr<const uint8_t[]> data) noexcept;

    source::BlobSourcePtr src_;
    zfile::HeaderTrailer ht_;     // effective (trailer unless overwritten)
    zfile::JumpTable jump_;
    std::unique_ptr<BlockCodec> codec_;
    bool verify_ = false;
    uint32_t block_size_ = 4096;
    std::string label_;
};

}  // namespace obd::format
