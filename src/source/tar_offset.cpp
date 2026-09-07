// TarOffsetSource. See tar_offset.hpp for the detection contract.
#include "source/tar_offset.hpp"

#include "common/errors.hpp"

#include <cstring>

namespace obd::source {

namespace {

constexpr size_t kBlock = 512;

// TarHeader field offsets (ustar, overlaybd tar/libtar.h TarHeader).
constexpr size_t kSizeField = 124;     // char size[12]
constexpr size_t kChksumField = 148;   // char chksum[8]
constexpr size_t kTypeflagField = 156;
constexpr size_t kMagicField = 257;    // char magic[6]
constexpr size_t kVersionField = 263;  // char version[2]

constexpr char kPaxHeader = 'x';        // PAX_HEADER
constexpr char kPaxGlobalHeader = 'g';  // PAX_GLOBAL_HEADER

/// Parses an octal field the way overlaybd's oct_to_int/oct_to_size do:
/// sscanf("%o"), 0 on failure. NUL- and space-terminated within the field.
uint64_t parse_octal(const uint8_t* field, size_t maxlen) {
    char tmp[16];
    const size_t n = std::min(maxlen, sizeof(tmp) - 1);
    std::memcpy(tmp, field, n);
    tmp[n] = '\0';
    unsigned long long v = 0;
    if (std::sscanf(tmp, "%llo", &v) != 1) return 0;
    return static_cast<uint64_t>(v);
}

/// overlaybd TarHeader::crc_ok(): the stored checksum equals the header
/// byte sum with the chksum field treated as spaces, computed either with
/// unsigned or signed chars (header.cpp crc_calc/signed_crc_calc).
bool tar_chksum_ok(const uint8_t* h) {
    const uint64_t stored = parse_octal(h + kChksumField, 8);
    uint32_t usum = 0;
    int32_t ssum = 0;
    for (size_t i = 0; i < kBlock; i++) {
        const bool in_chksum = (i >= kChksumField && i < kChksumField + 8);
        const uint8_t b = in_chksum ? static_cast<uint8_t>(' ') : h[i];
        usum += b;
        ssum += static_cast<int8_t>(in_chksum ? ' ' : h[i]);
    }
    return stored == usum || stored == static_cast<uint64_t>(ssum) ||
           (ssum >= 0 && stored == static_cast<uint64_t>(ssum));
}

bool is_ustar(const uint8_t* h) {
    return std::memcmp(h + kMagicField, "ustar", 5) == 0 &&
           std::memcmp(h + kVersionField, "00", 2) == 0;
}

bool is_new_tar_marker(const uint8_t* h) {
    // TMAGIC_EMPTY "xxtar" / TVERSION_EMPTY "xx" (overlaybd mark_new_tar).
    return std::memcmp(h + kMagicField, "xxtar", 5) == 0 &&
           std::memcmp(h + kVersionField, "xx", 2) == 0;
}

}  // namespace

elio::coro::task<BlobSourcePtr> TarOffsetSource::open(BlobSourcePtr src) {
    if (!src) throw error(EINVAL, "tar adapter with null source");
    if (src->size() < kBlock) co_return std::move(src);  // too small: plain

    uint8_t h[kBlock];
    ssize_t r = co_await src->pread(h, sizeof(h), 0);
    if (r < 0) co_return std::move(src);  // read failure: treat as plain
    if (static_cast<size_t>(r) != kBlock) co_return std::move(src);

    // is_tar_file(): magic + version + checksum; anything else is a plain
    // file (tar_file.cpp:308-330).
    if (!is_ustar(h) || !tar_chksum_ok(h)) co_return std::move(src);

    uint64_t base = kBlock;
    uint64_t size = parse_octal(h + kSizeField, 12);
    const char typeflag = static_cast<char>(h[kTypeflagField]);
    if (typeflag == kPaxHeader || typeflag == kPaxGlobalHeader) {
        // Pax-wrapped: the real header is the third block (overlaybd
        // TarFile::read_header: base_offset = 3 * T_BLOCKSIZE).
        base = 3 * kBlock;
        if (src->size() < base) {
            throw format_error("tar: truncated pax prefix");
        }
        uint8_t rh[kBlock];
        const ssize_t rr = co_await src->pread(rh, sizeof(rh), 2 * kBlock);
        if (rr != static_cast<ssize_t>(kBlock)) {
            throw error(EIO, "tar: short read on pax real header");
        }
        if (is_new_tar_marker(rh)) {
            // "new tar" marker: payload extends to the end of the underlying
            // blob (overlaybd TarFile::fstat is_new_tar branch).
            size = src->size() - base;
        } else {
            size = parse_octal(rh + kSizeField, 12);
        }
    }

    auto t = BlobSourcePtr(new TarOffsetSource());
    auto* self = static_cast<TarOffsetSource*>(t.get());
    self->base_ = base;
    self->size_ = size;
    self->label_ = "tar+" + std::string(src->label());
    self->src_ = std::move(src);
    co_return t;
}

elio::coro::task<ssize_t> TarOffsetSource::pread(void* buf, size_t count,
                                                 uint64_t offset) {
    if (offset >= size_) co_return 0;
    if (count > size_ - offset) count = static_cast<size_t>(size_ - offset);
    if (count == 0) co_return 0;
    const ssize_t r = co_await src_->pread(buf, count, offset + base_);
    co_return r;
}

}  // namespace obd::source
