// Unit tests: common module (crc32c, bytes/segment packing, base64).
#include "common/bytes.hpp"
#include "common/crc32c.hpp"
#include "source/base64.hpp"

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace obd;

TEST_CASE("common: crc32c matches the OverlayBD raw running CRC", "[common]") {
    // OverlayBD's crc32c (zfile/crc32/crc32c.cpp) is a RAW running CRC-32C:
    // reflected Castagnoli polynomial, seed passed through directly, no init
    // inversion and no final XOR. The golden values below were computed with
    // an independent table implementation, not from our own code:
    //   raw('123456789')      = 0x58E3FA20   (standard check value 0xE3069283
    //                                          applies to init/xorout form)
    //   salted('123456789')   = 0x3C9BD4E0   (seed = NOI_WELL_KNOWN_PRIME)
    const char* s = "123456789";
    REQUIRE(crc32::crc32c(s, 9) == 0x58E3FA20u);
    // Empty input with a seed returns the seed (raw, no inversion).
    REQUIRE(crc32::crc32c_extend("", 0, 7) == 7u);
    // Extend must equal the one-shot over the concatenation.
    const std::string a = "hello ", b = "world";
    const uint32_t two =
        crc32::crc32c_extend(b.data(), b.size(), crc32::crc32c(a.data(), a.size()));
    const std::string ab = a + b;
    REQUIRE(two == crc32::crc32c(ab.data(), ab.size()));
    // Salted variant uses the documented ZFile block seed (100007).
    REQUIRE(crc32::crc32c_salt(s, 9) == 0x3C9BD4E0u);
    REQUIRE(crc32::crc32c_salt(s, 9) ==
            crc32::crc32c_extend(s, 9, 100007u));
}

TEST_CASE("common: little-endian load/store round-trips", "[common]") {
    uint8_t buf[8];
    bytes::store_u16_le(buf, 0xBEEF);
    REQUIRE(buf[0] == 0xEF);
    REQUIRE(buf[1] == 0xBE);
    REQUIRE(bytes::load_u16_le(buf) == 0xBEEF);
    bytes::store_u32_le(buf, 0xDEADBEEF);
    REQUIRE(bytes::load_u32_le(buf) == 0xDEADBEEFu);
    bytes::store_u64_le(buf, 0x0123456789ABCDEFULL);
    REQUIRE(bytes::load_u64_le(buf) == 0x0123456789ABCDEFULL);
}

TEST_CASE("common: segment_mapping encodes OverlayBD bit layout", "[common]") {
    bytes::segment_mapping s;
    s.offset = 0x12345;      // 50 bits
    s.length = 0x2345;       // 14 bits
    s.moffset = 0x3456789;   // 55 bits
    s.zeroed = true;
    s.tag = 0xAB;
    uint8_t enc[16];
    bytes::store_segment_le(enc, s);
    // Golden bit layout: word0 = offset | length<<50;
    // word1 = moffset | zeroed<<55 | tag<<56.
    const uint64_t w0 = bytes::load_u64_le(enc);
    const uint64_t w1 = bytes::load_u64_le(enc + 8);
    REQUIRE(w0 == (uint64_t{0x12345} | (uint64_t{0x2345} << 50)));
    REQUIRE(w1 == (uint64_t{0x3456789} | (1ULL << 55) |
                  (uint64_t{0xAB} << 56)));

    const auto d = bytes::load_segment_le(enc);
    REQUIRE(d.offset == s.offset);
    REQUIRE(d.length == s.length);
    REQUIRE(d.moffset == s.moffset);
    REQUIRE(d.zeroed);
    REQUIRE(d.tag == 0xAB);
    REQUIRE(d.end() == s.offset + s.length);
}

TEST_CASE("common: base64 round-trips and decodes cred.json form", "[common]") {
    REQUIRE(source::base64_encode("") == "");
    REQUIRE(source::base64_encode("f") == "Zg==");
    REQUIRE(source::base64_encode("fo") == "Zm8=");
    REQUIRE(source::base64_encode("foo") == "Zm9v");
    REQUIRE(source::base64_encode("user:pass") == "dXNlcjpwYXNz");
    REQUIRE(source::base64_decode("dXNlcjpwYXNz") == "user:pass");
    const std::string blob(257, '\xAB');
    REQUIRE(source::base64_decode(source::base64_encode(blob)) == blob);
}
