// Trace blob codec implementation. Wire authority: docs/trace-format.md.
//
// Serialization uses explicit little-endian offset arithmetic
// (common/bytes.hpp), never struct dumps — a writer must control every
// byte, including the padding the checksum covers (trace-format.md §11).
// The checksum is the in-repo raw-chaining CRC-32C (common/crc32c.hpp),
// which already implements exactly the seed-0/no-complement convention
// this format requires (§4).
#include "format/trace.hpp"

#include "common/bytes.hpp"
#include "common/crc32c.hpp"

namespace obd::format::trace {

// Wire field offsets (trace-format.md §2.1 / §2.2).
namespace {
constexpr size_t kHdrMagic = 0;
constexpr size_t kHdrDataSize = 8;
constexpr size_t kHdrChecksum = 16;
constexpr size_t kRecOp = 0;
constexpr size_t kRecLayerIndex = 4;
constexpr size_t kRecCount = 8;
constexpr size_t kRecOffset = 16;

TraceParseError fail(TraceError kind, std::string message) {
    return {kind, std::move(message)};
}
}  // namespace

ParseResult parse(std::span<const uint8_t> blob) {
    // §7 rule 1: the 24-byte header must be readable.
    if (blob.size() < kHeaderSize) {
        return fail(TraceError::Truncated,
                    "blob of " + std::to_string(blob.size()) +
                        " bytes is smaller than the 24-byte header");
    }
    const uint8_t* hdr = blob.data();

    // §7 rule 2: magic.
    const uint32_t magic = bytes::load_u32_le(hdr + kHdrMagic);
    if (magic != kMagic) {
        return fail(TraceError::BadMagic, "magic mismatch");
    }

    // §7 rule 3: exact size — truncated and padded blobs are both rejected.
    const uint64_t data_size = bytes::load_u64_le(hdr + kHdrDataSize);
    const uint32_t checksum = bytes::load_u32_le(hdr + kHdrChecksum);
    if (data_size != static_cast<uint64_t>(blob.size()) - kHeaderSize) {
        return fail(TraceError::BadSize,
                    "blob size " + std::to_string(blob.size()) +
                        " != 24 + data_size " + std::to_string(data_size));
    }

    // §7 rule 4: floor(data_size / 24) records; a non-multiple tail is
    // accepted and ignored (never read, never checksummed).
    const size_t n = static_cast<size_t>(data_size / kRecordSize);
    const uint8_t* recs = hdr + kHeaderSize;

    // §7 rule 5: raw-chaining CRC-32C over the record bytes as stored,
    // padding included. The records are contiguous, so one chained call
    // over the whole stream equals upstream's per-record extend loop.
    const uint32_t actual =
        crc32::crc32c_extend(recs, n * kRecordSize, /*crc=*/0);
    if (actual != checksum) {
        return fail(TraceError::ChecksumMismatch,
                    "record stream checksum mismatch");
    }

    // No record field is validated at parse time (§7); unknown op bytes
    // and padding values are passed through verbatim.
    std::vector<TraceRecord> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = recs + i * kRecordSize;
        TraceRecord rec;
        rec.op = static_cast<char>(p[kRecOp]);
        rec.layer_index = bytes::load_u32_le(p + kRecLayerIndex);
        rec.count = bytes::load_u64_le(p + kRecCount);
        rec.offset = static_cast<int64_t>(bytes::load_u64_le(p + kRecOffset));
        out.push_back(rec);
    }
    return out;
}

TraceWriter::TraceWriter() : buf_(kHeaderSize, 0) {
    // First header pass: magic set, data_size = 0, checksum = 0
    // (PrefetcherImpl::dump writes the header before the records).
    bytes::store_u32_le(buf_.data() + kHdrMagic, kMagic);
}

bool TraceWriter::append(const TraceRecord& rec) noexcept {
    // Conforming-writer contract (trace-format.md §10 rule 4).
    if (rec.op != kOpRead) return false;
    if (rec.count < 1 || rec.count > kMaxRecordCount) return false;
    if (rec.offset < 0) return false;

    const size_t at = buf_.size();
    buf_.resize(at + kRecordSize, 0);  // zero padding bytes (§10 rule 5)
    uint8_t* p = buf_.data() + at;
    p[kRecOp] = static_cast<uint8_t>(rec.op);
    bytes::store_u32_le(p + kRecLayerIndex, rec.layer_index);
    bytes::store_u64_le(p + kRecCount, rec.count);
    bytes::store_u64_le(p + kRecOffset, static_cast<uint64_t>(rec.offset));

    crc_ = crc32::crc32c_extend(p, kRecordSize, crc_);
    ++records_;
    bytes::store_u64_le(buf_.data() + kHdrDataSize,
                        static_cast<uint64_t>(records_) * kRecordSize);
    return true;
}

std::span<const uint8_t> TraceWriter::finalize() noexcept {
    // Second header pass: rewrite the checksum field in place with the
    // final running CRC (PrefetcherImpl::dump rewrites the header after
    // appending all records).
    bytes::store_u32_le(buf_.data() + kHdrChecksum, crc_);
    return buf_;
}

}  // namespace obd::format::trace
