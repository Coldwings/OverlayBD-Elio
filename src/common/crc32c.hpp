// CRC-32C (Castagnoli, reflected polynomial 0x1EDC6F41).
//
// Semantics are byte-identical to the crc32c implementation vendored in
// overlaybd (src/overlaybd/zfile/crc32/, Facebook folly lineage):
//
//   crc32c(data, n)              == crc32c_extend(data, n, 0)
//   crc32c_extend(data, n, seed) continues the raw CRC from `seed` with NO
//                                pre- or post-conditioning (no ~ inversion).
//
// ZFile uses two flavors: plain crc32c (header digest, index crc) and the
// salted variant crc32c_salt(data, n) = crc32c_extend(data, n, 100007) for
// per-block verify checksums (NOI_WELL_KNOWN_PRIME).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace obd::crc32 {

/// Raw CRC-32C continuation from `crc` (no inversion), matching overlaybd's
/// crc32::crc32c_extend.
uint32_t crc32c_extend(const void* data, size_t nbytes, uint32_t crc);

inline uint32_t crc32c_extend(std::string_view text, uint32_t crc) {
    return crc32c_extend(text.data(), text.size(), crc);
}

/// Raw CRC-32C from seed 0, matching overlaybd's crc32::crc32c.
inline uint32_t crc32c(const void* data, size_t nbytes) {
    return crc32c_extend(data, nbytes, 0);
}

inline uint32_t crc32c(std::string_view text) {
    return crc32c_extend(text.data(), text.size(), 0);
}

/// ZFile per-block checksum: crc32c with seed 100007
/// (NOI_WELL_KNOWN_PRIME, overlaybd zfile.cpp crc32c_salt).
inline uint32_t crc32c_salt(const void* data, size_t nbytes) {
    return crc32c_extend(data, nbytes, 100007);
}

/// Software table-driven reference (always available; used by tests to pin
/// semantics independently of the hardware path).
uint32_t crc32c_sw(const void* data, size_t nbytes, uint32_t crc);

}  // namespace obd::crc32
