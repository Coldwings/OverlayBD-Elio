// ZFile read-only decompression view. See zfile.hpp and the format comment
// in zfile_format.hpp. Behavior mirrors overlaybd zfile.cpp:
// load_jump_table() (open) and CompressionFile::pread() (read path).
#include "format/zfile.hpp"

#include "common/crc32c.hpp"
#include "common/errors.hpp"

#include <algorithm>
#include <cstring>

namespace obd::format {

namespace {

constexpr size_t kSpace = zfile::kSpace;

}  // namespace

elio::coro::task<bool> is_zfile(source::BlobSource& src) {
    if (src.size() < kSpace * 2) co_return false;
    uint8_t region[kSpace];
    const ssize_t r = co_await src.pread(region, sizeof(region), 0);
    if (r != static_cast<ssize_t>(sizeof(region))) co_return false;
    if (std::memcmp(region, zfile::kMagic0, sizeof(zfile::kMagic0)) != 0 ||
        std::memcmp(region + 8, zfile::kMagic1, sizeof(zfile::kMagic1)) != 0) {
        co_return false;
    }
    try {
        const auto ht = zfile::HeaderTrailer::parse(region);
        co_return ht.region_digest_ok(region);
    } catch (const std::exception&) {
        co_return false;
    }
}

elio::coro::task<std::unique_ptr<ZFileSource>> ZFileSource::open(
    source::BlobSourcePtr src, bool verify) {
    if (!src) throw error(EINVAL, "zfile open with null source");
    const uint64_t file_size = src->size();
    if (file_size < kSpace * 2) {
        throw format_error("file too small to be a zfile (" +
                           std::to_string(file_size) + " bytes)");
    }

    // --- header (overlaybd load_jump_table: read offset 0, verify magic,
    // is_header, digest) ---
    uint8_t region[kSpace];
    ssize_t r = co_await src->pread(region, kSpace, 0);
    if (r != static_cast<ssize_t>(kSpace)) {
        throw error(EIO, "zfile: short read on header region");
    }
    auto ht = zfile::HeaderTrailer::parse(region);
    if (!ht.is_header()) {
        throw format_error("zfile: first region is not a header");
    }
    if (!ht.region_digest_ok(region)) {
        throw format_error("zfile: header digest verification failed");
    }

    // --- trailer is authoritative unless the header was overwritten ---
    zfile::HeaderTrailer effective = ht;
    uint64_t trailer_offset = 0;
    if (!ht.is_header_overwrite()) {
        if (!ht.is_data_file()) {
            throw format_error("zfile: unrecognized file type");
        }
        trailer_offset = file_size - kSpace;
        uint8_t tregion[kSpace];
        r = co_await src->pread(tregion, kSpace, trailer_offset);
        if (r != static_cast<ssize_t>(kSpace)) {
            throw error(EIO, "zfile: short read on trailer region");
        }
        auto tht = zfile::HeaderTrailer::parse(tregion);
        if (!tht.is_trailer() || !tht.is_data_file() || !tht.is_sealed()) {
            throw format_error(
                "zfile: trailer magic, type or sealedness doesn't match");
        }
        effective = tht;
    }

    if (effective.index_size > zfile::kMaxIndexSize) {
        throw format_error("zfile: index size " +
                           std::to_string(effective.index_size) +
                           " exceeds maximum");
    }
    const uint64_t index_bytes = effective.index_size * sizeof(uint32_t);
    if (!ht.is_header_overwrite() &&
        index_bytes > trailer_offset - effective.index_offset) {
        throw format_error("zfile: invalid index bytes or size");
    }
    if (effective.opt.use_dict != 0 || effective.opt.dict_size != 0) {
        throw format_error("zfile: dictionary compression is not supported");
    }
    if (!block_codec_available(effective.opt.algo)) {
        throw format_error("zfile: unsupported compression algo " +
                           std::to_string(effective.opt.algo));
    }

    // --- index region: u32 LE per-block compressed sizes ---
    std::vector<uint8_t> raw_index(index_bytes);
    if (index_bytes > 0) {
        r = co_await src->pread(raw_index.data(), index_bytes,
                                effective.index_offset);
        if (r != static_cast<ssize_t>(index_bytes)) {
            throw error(EIO, "zfile: short read on index region");
        }
    }
    if (effective.is_digest_enabled()) {
        const uint32_t crc = crc32::crc32c(raw_index.data(), index_bytes);
        if (crc != effective.index_crc) {
            throw format_error("zfile: checksum of jumptable is incorrect");
        }
    }
    std::vector<uint32_t> ibuf(effective.index_size);
    for (uint64_t i = 0; i < effective.index_size; i++) {
        ibuf[i] = bytes::load_u32_le(raw_index.data() + i * 4);
    }

    auto z = std::unique_ptr<ZFileSource>(new ZFileSource());
    z->jump_.build(ibuf.data(), ibuf.size(), kSpace + effective.opt.dict_size,
                   effective.opt.block_size, effective.opt.verify != 0);
    z->src_ = std::move(src);
    z->ht_ = effective;
    // overlaybd zfile_open_ro: ht.opt.verify = ht.opt.verify && verify
    z->verify_ = effective.opt.verify != 0 && verify;
    z->codec_ = create_block_codec(effective.opt.algo, effective.opt.level);
    z->block_size_ = effective.opt.block_size;
    z->label_ = "zfile(" + std::string(z->src_->label()) + ")";
    co_return z;
}

elio::coro::task<ssize_t> ZFileSource::pread(void* buf, size_t count,
                                             uint64_t offset) {
    const uint64_t fsize = ht_.original_file_size;
    if (offset >= fsize) co_return 0;
    if (count > fsize - offset) count = static_cast<size_t>(fsize - offset);
    if (count == 0) co_return 0;

    uint8_t* out = static_cast<uint8_t*>(buf);
    size_t done = 0;
    std::vector<uint8_t> window;              // batched compressed blocks
    std::vector<uint8_t> scratch(block_size_);  // partial-block decompression

    while (done < count) {
        const uint64_t pos = offset + done;
        const size_t begin_idx = static_cast<size_t>(pos / block_size_);
        const size_t last_idx =
            static_cast<size_t>((offset + count - 1) / block_size_);

        // Batch as many consecutive blocks as fit into the read window
        // (overlaybd BlockReader: one underlying pread per batch).
        size_t end_idx = begin_idx + 1;  // exclusive
        while (end_idx <= last_idx &&
               jump_[end_idx] - jump_[begin_idx] <= kReadWindow) {
            end_idx++;
        }
        const size_t comp_total =
            static_cast<size_t>(jump_[end_idx] - jump_[begin_idx]);
        window.resize(comp_total);
        const ssize_t r =
            co_await src_->pread(window.data(), comp_total, jump_[begin_idx]);
        if (r < 0) co_return r;
        if (static_cast<size_t>(r) != comp_total) co_return -EIO;

        for (size_t i = begin_idx; i < end_idx; i++) {
            const size_t off_in_win =
                static_cast<size_t>(jump_[i] - jump_[begin_idx]);
            const size_t blk_len =
                static_cast<size_t>(jump_[i + 1] - jump_[i]);
            size_t comp_len = blk_len;
            if (verify_) {
                comp_len -= sizeof(uint32_t);
                const uint32_t expect = bytes::load_u32_le(
                    window.data() + off_in_win + comp_len);
                const uint32_t got = crc32::crc32c_salt(
                    window.data() + off_in_win, comp_len);
                if (got != expect) {
                    co_return -EIO;  // block checksum mismatch
                }
            }
            const uint64_t blk_start = static_cast<uint64_t>(i) * block_size_;
            const uint64_t blk_uncomp =
                std::min<uint64_t>(block_size_, fsize - blk_start);
            // The subrange of this block the caller still needs.
            const uint64_t abs_from = std::max(pos, blk_start);
            const uint64_t abs_to =
                std::min<uint64_t>(offset + count, blk_start + blk_uncomp);
            const size_t in_start = static_cast<size_t>(abs_from - blk_start);
            const size_t in_len = static_cast<size_t>(abs_to - abs_from);

            const bool direct = (in_start == 0 && in_len == blk_uncomp);
            void* dtarget = direct ? static_cast<void*>(out + done)
                                   : static_cast<void*>(scratch.data());
            const size_t dcapacity = direct ? in_len : block_size_;
            const int rc = codec_->decompress(window.data() + off_in_win,
                                              comp_len, dtarget, dcapacity,
                                              static_cast<size_t>(blk_uncomp));
            if (rc != 0) co_return rc;
            if (!direct) {
                std::memcpy(out + done, scratch.data() + in_start, in_len);
            }
            done += in_len;
        }
    }
    co_return static_cast<ssize_t>(count);
}

}  // namespace obd::format
