// Trace blob codec: reader and writer for the upstream OverlayBD prefetch
// trace blob (docs/trace-format.md, ADR-0013 proposed).
//
// The blob is a raw LP64 little-endian struct image: a 24-byte header
// (magic, data_size, checksum) followed by a stream of fixed 24-byte
// records, all padding bytes included in the running CRC-32C. This codec
// is a pure in-memory parser/serializer — no IO, no coroutines. The wire
// authority is docs/trace-format.md; this header only restates the API
// contract.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace obd::format::trace {

/// Header magic: CRC32 (IEEE) of "Container Image Trace Format"
/// (trace-format.md §2.1; upstream prefetch.cpp TRACE_MAGIC).
inline constexpr uint32_t kMagic = 3270449184;  // 0xC2EF1820

/// Fixed sizes of the wire structs (LP64 layout, trace-format.md §1-§2).
inline constexpr size_t kHeaderSize = 24;
inline constexpr size_t kRecordSize = 24;

/// Replay-buffer safety cap on `count` (upstream MAX_IO_SIZE; a conforming
/// writer MUST split larger ranges — trace-format.md §8, §10 rule 4).
inline constexpr uint64_t kMaxRecordCount = 1048576;  // 1 MiB

/// The only op byte a conforming writer emits (trace-format.md §10 rule 4).
inline constexpr char kOpRead = 'R';

/// One decoded record (trace-format.md §2.2). Field widths match the wire
/// layout; the parser validates nothing beyond framing and checksum, so
/// `op` may hold any byte and semantics are the caller's (replay skips
/// anything but 'R').
struct TraceRecord {
    char op = 0;              ///< 'R' (READ) or 'W' (WRITE); any byte parses
    uint32_t layer_index = 0; ///< 0-based index into the image's lower list
    uint64_t count = 0;       ///< read length in bytes
    int64_t offset = 0;       ///< byte offset within the layer blob file

    bool operator==(const TraceRecord&) const = default;
};

/// Precise parse-failure kinds, in the §7 acceptance order they can occur.
enum class TraceError {
    Truncated,         ///< fewer than kHeaderSize bytes (header unreadable)
    BadMagic,          ///< magic != kMagic
    BadSize,           ///< blob size != kHeaderSize + data_size (exact rule)
    ChecksumMismatch,  ///< running CRC-32C over the records != header checksum
};

/// Parse failure: which acceptance rule fired plus a human-readable detail.
struct TraceParseError {
    TraceError kind;
    std::string message;
};

/// Result of parse(): either the decoded record list or a precise failure.
/// std::expected-shaped, hand-rolled because the toolchain floor (GCC 12)
/// predates <expected>.
class ParseResult {
public:
    /*implicit*/ ParseResult(std::vector<TraceRecord> records)
        : data_(std::move(records)) {}
    /*implicit*/ ParseResult(TraceParseError error) : data_(std::move(error)) {}

    bool has_value() const noexcept {
        return std::holds_alternative<std::vector<TraceRecord>>(data_);
    }
    explicit operator bool() const noexcept { return has_value(); }

    /// Record list; only valid when has_value().
    const std::vector<TraceRecord>& value() const {
        return std::get<std::vector<TraceRecord>>(data_);
    }
    const std::vector<TraceRecord>& operator*() const { return value(); }
    const std::vector<TraceRecord>* operator->() const { return &value(); }

    /// Failure detail; only valid when !has_value().
    const TraceParseError& error() const {
        return std::get<TraceParseError>(data_);
    }

private:
    std::variant<std::vector<TraceRecord>, TraceParseError> data_;
};

/// Parses and validates a complete trace blob held in memory, applying the
/// parser acceptance rules of trace-format.md §7 in check order: readable
/// 24-byte header, magic, exact size (blob == 24 + data_size), then the
/// raw-chaining CRC-32C (seed 0, no complements) over the
/// floor(data_size/24) records INCLUDING padding bytes. A non-multiple
/// data_size tail is accepted and ignored (§7 rule 4). Any failure rejects
/// the whole blob (deliberately stricter than upstream's partial-queue
/// quirk, §8). Record fields are not validated; unknown `op` bytes are
/// exposed to the caller verbatim.
ParseResult parse(std::span<const uint8_t> blob);

/// Conforming-writer implementation (trace-format.md §10). Accumulates
/// records into an in-memory blob laid out exactly as upstream's
/// PrefetcherImpl::dump produces it: the 24-byte header is present from
/// construction with checksum = 0, records are appended with zero padding
/// bytes while a running raw-chaining CRC-32C is maintained, and
/// finalize() rewrites the header checksum in place (mirroring upstream's
/// header rewrite pass). Append enforces the writer contract: op == 'R'
/// only, 1 <= count <= 1048576, offset >= 0.
class TraceWriter {
public:
    /// Constructs an empty trace: buffer holds the 24-byte header with
    /// magic set, data_size = 0, checksum = 0.
    TraceWriter();

    /// Appends one record under the conforming-writer contract
    /// (trace-format.md §10 rule 4): `rec.op` must be 'R',
    /// 1 <= rec.count <= kMaxRecordCount, rec.offset >= 0. Returns false
    /// (record rejected, blob unchanged) on any violation.
    bool append(const TraceRecord& rec) noexcept;

    size_t record_count() const noexcept { return records_; }

    /// Running raw-chaining CRC-32C over the record bytes written so far.
    uint32_t checksum() const noexcept { return crc_; }

    /// Patches the header checksum in place with the current running CRC
    /// (upstream's second header pass) and returns the complete blob,
    /// 24 + 24 * record_count() bytes. Idempotent; appending after
    /// finalize() is allowed and the next finalize() re-patches. The
    /// returned span borrows the writer and is invalidated by append() or
    /// destruction.
    std::span<const uint8_t> finalize() noexcept;

private:
    std::vector<uint8_t> buf_;  // header + record stream, zero padding
    uint32_t crc_ = 0;          // raw-chaining CRC-32C over the records
    size_t records_ = 0;
};

}  // namespace obd::format::trace
