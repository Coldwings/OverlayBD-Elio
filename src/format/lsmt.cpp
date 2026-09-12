// LSMT read-only implementation. See lsmt.hpp; behavior mirrors overlaybd
// src/overlaybd/lsmt/{file,index}.cpp as cited per function.
#include "format/lsmt.hpp"

#include "common/errors.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace obd::format {

namespace {

// Keep source selection in the mapped offset space so MergedLsmt can keep
// using every tag exclusively as a layer number. The gap is at most 511B.
class WarpSource final : public source::BlobSource {
public:
    WarpSource(source::BlobSourcePtr metadata, source::BlobSourcePtr target,
               uint64_t base)
        : metadata_(std::move(metadata)), target_(std::move(target)), base_(base) {}
    uint64_t size() const noexcept override { return base_ + target_->size(); }
    std::string_view label() const noexcept override { return "warp-sources"; }
    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                   uint64_t offset) override {
        if (offset >= size()) co_return 0;
        count = static_cast<size_t>(std::min<uint64_t>(count, size() - offset));
        size_t done = 0;
        auto* out = static_cast<uint8_t*>(buf);
        while (done < count) {
            const uint64_t pos = offset + done;
            if (pos >= base_) {
                const auto r = co_await target_->pread(out + done, count - done,
                                                       pos - base_);
                if (r < 0) co_return r;
                if (static_cast<size_t>(r) != count - done) co_return -EIO;
                done = count;
            } else if (pos >= metadata_->size()) {
                const size_t n = static_cast<size_t>(
                    std::min<uint64_t>(count - done, base_ - pos));
                std::memset(out + done, 0, n);
                done += n;
            } else {
                const size_t n = static_cast<size_t>(
                    std::min<uint64_t>(count - done, metadata_->size() - pos));
                const auto r = co_await metadata_->pread(out + done, n, pos);
                if (r < 0) co_return r;
                if (static_cast<size_t>(r) != n) co_return -EIO;
                done += n;
            }
        }
        co_return static_cast<ssize_t>(done);
    }
private:
    source::BlobSourcePtr metadata_, target_;
    uint64_t base_;
};

}  // namespace

// ---------------------------------------------------------------------------
// LsmtLayer
// ---------------------------------------------------------------------------

elio::coro::task<std::unique_ptr<LsmtLayer>> LsmtLayer::open(
    source::BlobSourcePtr src) {
    co_return co_await open_impl(std::move(src), nullptr);
}

elio::coro::task<std::unique_ptr<LsmtLayer>> LsmtLayer::open_warp(
    source::BlobSourcePtr metadata, source::BlobSourcePtr target) {
    if (!target) throw error(EINVAL, "lsmt warp open with null target");
    co_return co_await open_impl(std::move(metadata), std::move(target));
}

elio::coro::task<std::unique_ptr<LsmtLayer>> LsmtLayer::open_impl(
    source::BlobSourcePtr src, source::BlobSourcePtr target) {
    if (!src) throw error(EINVAL, "lsmt open with null source");
    const uint64_t file_size = src->size();
    if (file_size < lsmt::kSpace * 2) {
        throw format_error("file too small to be an lsmt layer (" +
                           std::to_string(file_size) + " bytes)");
    }

    // verify_ht(header): magic + is_header (file.cpp:1329-1341); the trailer
    // branch of do_load_index additionally requires the header to be a data
    // file (file.cpp:1379-1381).
    uint8_t region[lsmt::kSpace];
    ssize_t r = co_await src->pread(region, sizeof(region), 0);
    if (r != static_cast<ssize_t>(sizeof(region))) {
        throw error(EIO, "lsmt: short read on header region");
    }
    auto hht = lsmt::HeaderTrailer::parse(region);
    if (!hht.is_header()) {
        throw format_error("lsmt: first region is not a header");
    }
    if (!hht.is_data_file()) {
        throw format_error("lsmt: unrecognized file type");
    }

    // verify_ht(trailer): magic + is_trailer + is_data_file + is_sealed +
    // index_size bound; trailer fields are authoritative (file.cpp:1343-1356).
    const uint64_t trailer_offset = file_size - lsmt::kSpace;
    r = co_await src->pread(region, sizeof(region), trailer_offset);
    if (r != static_cast<ssize_t>(sizeof(region))) {
        throw error(EIO, "lsmt: short read on trailer region");
    }
    auto tht = lsmt::HeaderTrailer::parse(region);
    if (!tht.is_trailer() || !tht.is_data_file() || !tht.is_sealed()) {
        throw format_error(
            "lsmt: trailer magic, type or sealedness doesn't match");
    }
    if (tht.index_size > lsmt::kMaxRoIndexSize) {
        throw format_error("lsmt: RO index size " +
                           std::to_string(tht.index_size) +
                           " exceeds maximum");
    }
    const uint64_t index_bytes =
        tht.index_size * bytes::segment_mapping::kEncodedSize;
    // index_offset bounds BEFORE the subtraction: an out-of-range
    // index_offset would underflow it (and with index_size == 0 silently
    // load an empty index).
    if (tht.index_offset < lsmt::kSpace ||
        tht.index_offset > trailer_offset ||
        index_bytes > trailer_offset - tht.index_offset) {
        throw format_error("lsmt: invalid index bytes or size");
    }

    // do_load_index: read, drop INVALID_OFFSET entries, clear tags
    // (file.cpp:1404-1428).
    std::vector<uint8_t> raw(index_bytes);
    if (index_bytes > 0) {
        r = co_await src->pread(raw.data(), index_bytes, tht.index_offset);
        if (r != static_cast<ssize_t>(index_bytes)) {
            throw error(EIO, "lsmt: short read on index region");
        }
    }
    std::vector<bytes::segment_mapping> segments;
    segments.reserve(tht.index_size);
    for (uint64_t i = 0; i < tht.index_size; i++) {
        auto s = bytes::load_segment_le(
            raw.data() + i * bytes::segment_mapping::kEncodedSize);
        if (s.offset == bytes::segment_mapping::kInvalidOffset) continue;
        if (!target) s.tag = 0;
        segments.push_back(s);
    }

    // create_memory_index validations (index.cpp:795-841): strict ordering
    // under `a < b  <=>  a.end() <= b.offset`, and moffset within the data
    // region [8, index_offset/512) sectors (zeroed: 8 <= moffset <= end).
    for (size_t i = 1; i < segments.size(); i++) {
        if (segments[i - 1].end() > segments[i].offset) {
            throw format_error("lsmt: incorrect segment mappings (disordered)");
        }
    }
    uint64_t target_base = 0;
    uint8_t min_tag = 255;
    if (target) {
        if (tht.virtual_size % lsmt::kAlignment != 0)
            throw format_error("lsmt warp: unaligned virtual size");
        constexpr auto max = std::numeric_limits<uint64_t>::max();
        if (file_size > max - (lsmt::kAlignment - 1))
            throw format_error("lsmt warp: metadata offset overflow");
        target_base = (file_size + lsmt::kAlignment - 1) /
                      lsmt::kAlignment * lsmt::kAlignment;
        if (target->size() > max - target_base)
            throw format_error("lsmt warp: combined source size overflow");
        for (const auto& m : segments) min_tag = std::min(min_tag, m.tag);
    }
    const uint64_t moffset_begin = lsmt::kDataStartSector;
    const uint64_t moffset_end = tht.index_offset / lsmt::kAlignment;
    for (auto& m : segments) {
        if (target) {
            const auto source_tag = m.tag - min_tag;
            if (source_tag > 1)
                throw format_error("lsmt warp: unsupported normalized source tag");
            if (m.length == 0 || m.end() > bytes::segment_mapping::kMaxOffset ||
                tht.virtual_size % lsmt::kAlignment != 0 ||
                m.end() > tht.virtual_size / lsmt::kAlignment)
                throw format_error("lsmt warp: invalid logical extent");
            m.tag = 0;
            if (m.zeroed) continue;
            if (source_tag == 1) {
                const uint64_t sectors = target->size() / lsmt::kAlignment;
                if (m.moffset > sectors || m.length > sectors - m.moffset)
                    throw format_error("lsmt warp: target extent out of range");
                // Combined byte-size validation above also proves these
                // additions and subsequent sector-to-byte reads cannot wrap.
                m.moffset += target_base / lsmt::kAlignment;
                continue;
            }
        }
        const bool ok = m.zeroed
                            ? (moffset_begin <= m.moffset &&
                               m.moffset <= moffset_end)
                            : (moffset_begin <= m.moffset &&
                               m.moffset < moffset_end &&
                               moffset_begin < m.mend() &&
                               m.mend() <= moffset_end);
        if (!ok) {
            throw format_error("lsmt: mapped offset out of range");
        }
    }

    tht.index_size = segments.size();
    auto layer = std::unique_ptr<LsmtLayer>(new LsmtLayer());
    layer->label_ = "lsmt(" + std::string(src->label()) + ")";
    if (target) {
        layer->label_ = "warp(" + std::string(src->label()) + ")";
        layer->src_ = std::make_unique<WarpSource>(
            std::move(src), std::move(target), target_base);
    } else {
        layer->src_ = std::move(src);
    }
    layer->ht_ = tht;
    layer->segments_ = std::move(segments);
    co_return layer;
}

// ---------------------------------------------------------------------------
// MergedLsmt
// ---------------------------------------------------------------------------

void MergedLsmt::merge_range(
    const std::vector<const std::vector<bytes::segment_mapping>*>& layers,
    size_t level, uint64_t lo, uint64_t hi,
    std::vector<bytes::segment_mapping>& out) {
    if (level >= layers.size() || lo >= hi) return;
    const auto& segs = *layers[level];
    // First segment with end() > lo (segments are sorted and disjoint, so
    // end() is strictly increasing).
    auto it = std::upper_bound(
        segs.begin(), segs.end(), lo,
        [](uint64_t x, const bytes::segment_mapping& s) { return x < s.end(); });
    for (; it != segs.end() && it->offset < hi; ++it) {
        if (it->offset > lo) {
            // Gap not covered by this level: recurse into lower levels.
            merge_range(layers, level + 1, lo, it->offset, out);
        }
        bytes::segment_mapping s = *it;
        if (s.offset < lo) {  // clip head
            const uint64_t delta = lo - s.offset;
            s.offset = lo;
            s.length = static_cast<uint32_t>(s.length - delta);
            if (!s.zeroed) s.moffset += delta;
        }
        if (s.end() > hi) {  // clip tail
            s.length = static_cast<uint32_t>(hi - s.offset);
        }
        if (s.length == 0) continue;
        s.tag = static_cast<uint8_t>(level);
        out.push_back(s);
        lo = s.end();
        if (lo >= hi) break;
    }
    if (lo < hi) {
        merge_range(layers, level + 1, lo, hi, out);
    }
}

void MergedLsmt::merge_indexes(
    const std::vector<const std::vector<bytes::segment_mapping>*>& layers,
    std::vector<bytes::segment_mapping>& out) {
    merge_range(layers, 0, 0, bytes::segment_mapping::kMaxOffset, out);
    if (out.size() > lsmt::kMaxRoIndexSize) {
        throw format_error("lsmt: merged index size " + std::to_string(out.size()) +
                           " exceeds maximum");
    }
}

elio::coro::task<std::unique_ptr<MergedLsmt>> MergedLsmt::open(
    std::vector<std::unique_ptr<LsmtLayer>> layers_bottom_up) {
    if (layers_bottom_up.empty()) {
        throw error(EINVAL, "lsmt merge with no layers");
    }
    if (layers_bottom_up.size() > lsmt::kMaxStackLayers) {
        throw format_error("lsmt: too many layers (" +
                           std::to_string(layers_bottom_up.size()) + " > " +
                           std::to_string(lsmt::kMaxStackLayers) + ")");
    }

    // overlaybd load_merge_index: virtual size = topmost non-zero
    // virtual_size (file.cpp:1777-1782).
    uint64_t vsize = 0;
    for (auto it = layers_bottom_up.rbegin(); it != layers_bottom_up.rend();
         ++it) {
        if ((*it)->virtual_size() != 0) {
            vsize = (*it)->virtual_size();
            break;
        }
    }
    if (vsize == 0) {
        throw format_error("lsmt: no layer reports a non-zero virtual size");
    }

    // reverse: layers_[0] = topmost (file.cpp:1784-1786).
    std::vector<std::unique_ptr<LsmtLayer>> layers;
    layers.reserve(layers_bottom_up.size());
    for (auto it = layers_bottom_up.rbegin(); it != layers_bottom_up.rend();
         ++it) {
        layers.push_back(std::move(*it));
    }

    std::vector<const std::vector<bytes::segment_mapping>*> seg_ptrs;
    seg_ptrs.reserve(layers.size());
    for (const auto& l : layers) seg_ptrs.push_back(&l->segments());

    auto merged = std::unique_ptr<MergedLsmt>(new MergedLsmt());
    merge_indexes(seg_ptrs, merged->index_);
    merged->layers_ = std::move(layers);
    merged->vsize_ = vsize;
    merged->label_ = "lsmt-merged(" +
                     std::to_string(merged->layers_.size()) + " layers)";
    co_return merged;
}

elio::coro::task<ssize_t> MergedLsmt::pread(void* buf, size_t count,
                                            uint64_t offset) {
    constexpr uint64_t kSector = lsmt::kAlignment;
    if ((offset % kSector) != 0 || (count % kSector) != 0) {
        co_return -EINVAL;  // LSMT reads are sector-aligned (file.cpp:566-571)
    }
    if (offset >= vsize_) co_return 0;
    if (count > vsize_ - offset) count = static_cast<size_t>(vsize_ - offset);
    if (count == 0) co_return 0;

    const uint64_t start_sec = offset / kSector;
    const uint64_t end_sec = (offset + count) / kSector;
    uint8_t* out = static_cast<uint8_t*>(buf);

    // First merged segment with end() > start_sec (index.cpp lookup
    // semantics: lower_bound under `a.end() <= b.offset`).
    auto it = std::upper_bound(
        index_.begin(), index_.end(), start_sec,
        [](uint64_t x, const bytes::segment_mapping& s) { return x < s.end(); });

    uint64_t cur = start_sec;
    while (cur < end_sec) {
        if (it == index_.end() || it->offset >= end_sec) {
            // Trailing hole.
            std::memset(out + (cur - start_sec) * kSector, 0,
                        static_cast<size_t>(end_sec - cur) * kSector);
            cur = end_sec;
            break;
        }
        if (it->offset > cur) {
            // Hole before the next segment.
            const uint64_t h = std::min(it->offset, end_sec);
            std::memset(out + (cur - start_sec) * kSector, 0,
                        static_cast<size_t>(h - cur) * kSector);
            cur = h;
            continue;
        }
        const uint64_t seg_from = std::max(it->offset, cur);
        const uint64_t seg_to = std::min(it->end(), end_sec);
        const size_t piece_bytes =
            static_cast<size_t>(seg_to - seg_from) * kSector;
        uint8_t* piece = out + (seg_from - start_sec) * kSector;
        if (it->zeroed) {
            std::memset(piece, 0, piece_bytes);
        } else {
            const uint64_t data_off =
                (it->moffset + (seg_from - it->offset)) * kSector;
            const ssize_t r =
                co_await layers_[it->tag]->data_source().pread(piece,
                                                               piece_bytes,
                                                               data_off);
            if (r < 0) co_return r;
            if (static_cast<size_t>(r) < piece_bytes) {
                // overlaybd: retry once, then zero-fill the tail
                // (file.cpp:604-618). Sources already loop over short reads,
                // so a short result means the layer blob ended early:
                // zero-fill and carry on.
                const ssize_t r2 = co_await layers_[it->tag]->data_source().pread(
                    piece + r, piece_bytes - static_cast<size_t>(r),
                    data_off + static_cast<uint64_t>(r));
                size_t filled = static_cast<size_t>(std::max<ssize_t>(r2, 0)) +
                                static_cast<size_t>(r);
                if (filled < piece_bytes) {
                    std::memset(piece + filled, 0, piece_bytes - filled);
                }
            }
        }
        cur = seg_to;
        if (it->end() <= cur) ++it;
    }
    co_return static_cast<ssize_t>(count);
}

}  // namespace obd::format
