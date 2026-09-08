// Unit tests: trace blob codec — golden vectors and the worked example
// from docs/trace-format.md (§4 CRC-32C vectors, §13 72-byte blob), plus
// writer round-trips, parser negatives, and writer-contract enforcement.
// Every golden byte value below is taken from the specification document,
// never from this project's own writer output.
#include "format/trace.hpp"

#include "common/bytes.hpp"
#include "common/crc32c.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

using namespace obd;

namespace {

// Serializes one record with zero padding, exactly as the wire format
// lays it out (trace-format.md §2.2). Test-local: golden blobs are built
// by hand so the codec under test never generates its own expectations.
void put_record(std::vector<uint8_t>& out, char op, uint32_t layer_index,
                uint64_t count, int64_t offset) {
    const size_t at = out.size();
    out.resize(at + format::trace::kRecordSize, 0);
    uint8_t* p = out.data() + at;
    p[0] = static_cast<uint8_t>(op);
    bytes::store_u32_le(p + 4, layer_index);
    bytes::store_u64_le(p + 8, count);
    bytes::store_u64_le(p + 16, static_cast<uint64_t>(offset));
}

// Builds a complete blob from hand-serialized records; the checksum is
// computed with the shared raw-chaining CRC-32C primitive
// (common/crc32c.hpp), pinned independently by the spec's §4 vectors.
std::vector<uint8_t> make_blob(std::vector<uint8_t> records,
                               uint64_t data_size_override = UINT64_MAX) {
    std::vector<uint8_t> blob(format::trace::kHeaderSize, 0);
    bytes::store_u32_le(blob.data(), format::trace::kMagic);
    const uint64_t data_size =
        data_size_override == UINT64_MAX ? records.size() : data_size_override;
    bytes::store_u64_le(blob.data() + 8, data_size);
    const size_t n = static_cast<size_t>(data_size) / format::trace::kRecordSize;
    bytes::store_u32_le(blob.data() + 16,
                        crc32::crc32c_extend(records.data(),
                                             n * format::trace::kRecordSize,
                                             /*crc=*/0));
    blob.insert(blob.end(), records.begin(), records.end());
    return blob;
}

}  // namespace

TEST_CASE("format: trace crc32c golden vectors match the spec", "[format]") {
    // Golden values: docs/trace-format.md §4 test vectors (raw-chaining
    // CRC-32C, seed 0, no complements, zero padding).
    REQUIRE(crc32::crc32c_extend(nullptr, 0, 0) == 0x00000000u);

    std::vector<uint8_t> stream;
    put_record(stream, 'R', 1, 4096, 0);
    REQUIRE(crc32::crc32c_extend(stream.data(), stream.size(), 0) ==
            0xBA691A13u);

    // Chaining property: the two-record CRC equals the one-shot CRC over
    // the 48-byte concatenation (§4, "verified").
    const uint32_t chained = crc32::crc32c_extend(stream.data(), 24, 0);
    put_record(stream, 'R', 0, 8192, 16384);
    const uint32_t extended =
        crc32::crc32c_extend(stream.data() + 24, 24, chained);
    REQUIRE(extended == 0xD29283DDu);
    REQUIRE(crc32::crc32c_extend(stream.data(), stream.size(), 0) ==
            0xD29283DDu);

    // The software table-driven reference (crc32c_sw) must hit the same §4
    // vectors and agree with the dispatching crc32c_extend: on SSE4.2
    // hosts the dispatch path uses hardware instructions, so without this
    // the table path would never execute in tests.
    REQUIRE(crc32::crc32c_sw(nullptr, 0, 0) == 0x00000000u);
    REQUIRE(crc32::crc32c_sw(stream.data(), 24, 0) == 0xBA691A13u);
    REQUIRE(crc32::crc32c_sw(stream.data(), stream.size(), 0) ==
            0xD29283DDu);
    // ... and on an unaligned buffer larger than one record.
    std::vector<uint8_t> big;
    for (uint32_t i = 0; i < 37; ++i)
        put_record(big, 'R', i, 100 + i, static_cast<int64_t>(i) * 13);
    REQUIRE(crc32::crc32c_sw(big.data() + 1, big.size() - 1, 0) ==
            crc32::crc32c_extend(big.data() + 1, big.size() - 1, 0));

    // The writer's incremental CRC lands on the same value.
    format::trace::TraceWriter w;
    REQUIRE(w.append({'R', 1, 4096, 0}));
    REQUIRE(w.checksum() == 0xBA691A13u);
    REQUIRE(w.append({'R', 0, 8192, 16384}));
    REQUIRE(w.checksum() == 0xD29283DDu);
}

TEST_CASE("format: trace decodes the spec worked example byte-for-byte",
          "[format]") {
    // Golden blob: docs/trace-format.md §13 appendix — the complete valid
    // 72-byte blob (two records, zero padding, checksum 0xD29283DD).
    const std::vector<uint8_t> golden = {
        0x20, 0x18, 0xef, 0xc2, 0x00, 0x00, 0x00, 0x00,  // magic + pad
        0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // data_size = 48
        0xdd, 0x83, 0x92, 0xd2, 0x00, 0x00, 0x00, 0x00,  // checksum + pad
        0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,  // R, layer 1
        0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // count = 4096
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // offset = 0
        0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // R, layer 0
        0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // count = 8192
        0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // offset = 16384
    };
    REQUIRE(golden.size() == 72);

    // Header fields at their documented offsets (§2.1).
    REQUIRE(bytes::load_u32_le(golden.data()) == 0xC2EF1820u);
    REQUIRE(bytes::load_u64_le(golden.data() + 8) == 48);
    REQUIRE(bytes::load_u32_le(golden.data() + 16) == 0xD29283DDu);

    // The parser decodes both records exactly.
    auto parsed = format::trace::parse(golden);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 2);
    REQUIRE((*parsed)[0] == format::trace::TraceRecord{'R', 1, 4096, 0});
    REQUIRE((*parsed)[1] == format::trace::TraceRecord{'R', 0, 8192, 16384});

    // The conforming writer reproduces the golden blob byte-for-byte.
    format::trace::TraceWriter w;
    REQUIRE(w.append({'R', 1, 4096, 0}));
    REQUIRE(w.append({'R', 0, 8192, 16384}));
    const auto produced = w.finalize();
    REQUIRE(produced.size() == golden.size());
    REQUIRE(std::memcmp(produced.data(), golden.data(), golden.size()) == 0);
}

TEST_CASE("format: trace writer round-trips through the parser", "[format]") {
    format::trace::TraceWriter w;
    std::vector<format::trace::TraceRecord> want;
    for (uint32_t i = 0; i < 64; ++i) {
        format::trace::TraceRecord rec{'R', i % 5,
                                       1 + (i * 777) % format::trace::kMaxRecordCount,
                                       static_cast<int64_t>(i) * 4096};
        REQUIRE(w.append(rec));
        want.push_back(rec);
    }
    const auto blob = w.finalize();
    REQUIRE(blob.size() ==
            format::trace::kHeaderSize + 64 * format::trace::kRecordSize);
    REQUIRE(bytes::load_u64_le(blob.data() + 8) ==
            64 * format::trace::kRecordSize);

    auto parsed = format::trace::parse(blob);
    REQUIRE(parsed.has_value());
    REQUIRE(*parsed == want);

    // Offset decode width (§2.2): offset is a signed 64-bit field. A
    // value >= 2^32 must not truncate through the u64/i64 decode...
    format::trace::TraceWriter wide;
    const int64_t wide_offset = (int64_t{1} << 40) + 4096;
    REQUIRE(wide.append({'R', 3, 4096, wide_offset}));
    auto wide_parsed = format::trace::parse(wide.finalize());
    REQUIRE(wide_parsed.has_value());
    REQUIRE((*wide_parsed)[0].offset == wide_offset);

    // ...and a negative offset bit pattern decodes back to the same i64
    // (the parser performs no offset validation, §7; the writer contract
    // rejects negatives, so this blob is hand-built).
    std::vector<uint8_t> neg_records;
    put_record(neg_records, 'R', 0, 512, -1);
    put_record(neg_records, 'R', 0, 512, INT64_MIN);
    auto neg_parsed = format::trace::parse(make_blob(neg_records));
    REQUIRE(neg_parsed.has_value());
    REQUIRE(neg_parsed->size() == 2);
    REQUIRE((*neg_parsed)[0].offset == -1);
    REQUIRE((*neg_parsed)[1].offset == INT64_MIN);

    // finalize() is idempotent, and appending after finalize() is
    // allowed: the next finalize() re-patches the header.
    format::trace::TraceWriter f;
    REQUIRE(f.append({'R', 1, 4096, 0}));
    const auto first = f.finalize();
    const std::vector<uint8_t> first_copy(first.begin(), first.end());
    const auto second = f.finalize();
    REQUIRE(second.size() == first_copy.size());
    REQUIRE(std::memcmp(second.data(), first_copy.data(),
                        first_copy.size()) == 0);
    REQUIRE(f.append({'R', 2, 8192, 4096}));
    auto grown = format::trace::parse(f.finalize());
    REQUIRE(grown.has_value());
    REQUIRE(grown->size() == 2);
    REQUIRE((*grown)[1] == format::trace::TraceRecord{'R', 2, 8192, 4096});

    // The empty trace is a valid 24-byte blob with zero records (§8).
    format::trace::TraceWriter empty;
    auto empty_parsed = format::trace::parse(empty.finalize());
    REQUIRE(empty_parsed.has_value());
    REQUIRE(empty_parsed->empty());
}

TEST_CASE("format: trace parser rejects corrupt headers and checksums",
          "[format]") {
    std::vector<uint8_t> records;
    put_record(records, 'R', 1, 4096, 0);
    const std::vector<uint8_t> good = make_blob(records);
    REQUIRE(format::trace::parse(good).has_value());

    // Truncated header: fewer than 24 bytes (§7 rule 1).
    for (size_t n : {size_t{0}, size_t{4}, size_t{23}}) {
        auto r = format::trace::parse(
            std::span<const uint8_t>(good.data(), n));
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == format::trace::TraceError::Truncated);
    }

    // Bad magic (§7 rule 2).
    {
        auto bad = good;
        bad[0] ^= 0xFF;
        auto r = format::trace::parse(bad);
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == format::trace::TraceError::BadMagic);
    }

    // Size mismatch, both directions (§7 rule 3): a truncated blob and a
    // padded blob are equally rejected.
    {
        auto r = format::trace::parse(
            std::span<const uint8_t>(good.data(), good.size() - 1));
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == format::trace::TraceError::BadSize);
    }
    {
        auto bad = good;
        bad.push_back(0x00);
        auto r = format::trace::parse(bad);
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == format::trace::TraceError::BadSize);
    }
    {
        auto bad = good;
        bytes::store_u64_le(bad.data() + 8, 72);  // lies about data_size
        auto r = format::trace::parse(bad);
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == format::trace::TraceError::BadSize);
    }

    // Check-order pin (§7): magic (rule 2) is reported before size
    // (rule 3) when both are wrong.
    {
        auto bad = good;
        bad[0] ^= 0xFF;                          // bad magic ...
        bad.push_back(0x00);                     // ... and bad size
        auto r = format::trace::parse(bad);
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == format::trace::TraceError::BadMagic);
    }

    // Checksum mismatch (§7 rule 5): flip one record byte; the header
    // checksum no longer matches the stream as stored.
    {
        auto bad = good;
        bad[format::trace::kHeaderSize + 9] ^= 0x01;  // inside count
        auto r = format::trace::parse(bad);
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == format::trace::TraceError::ChecksumMismatch);
    }
}

TEST_CASE("format: trace parser accepts and ignores a non-multiple tail",
          "[format]") {
    // trace-format.md §7 rule 4 / §12.2: data_size = 25 passes the exact
    // size rule; the one full record is decoded, the trailing byte is
    // ignored and not checksummed.
    std::vector<uint8_t> records;
    put_record(records, 'R', 1, 4096, 0);
    records.push_back(0xAB);  // 25 bytes of "record stream"
    const std::vector<uint8_t> blob = make_blob(records, /*data_size=*/25);
    auto parsed = format::trace::parse(blob);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 1);
    REQUIRE((*parsed)[0] == format::trace::TraceRecord{'R', 1, 4096, 0});
}

TEST_CASE("format: trace parser exposes unknown op bytes to the caller",
          "[format]") {
    // The parser performs no op validation (trace-format.md §7, §8):
    // 'W' and arbitrary bytes parse fine and reach the caller, which owns
    // the skip semantics (replay ignores anything but 'R').
    std::vector<uint8_t> records;
    put_record(records, 'W', 0, 4096, 0);
    put_record(records, '\x7F', 2, 512, 1024);
    auto parsed = format::trace::parse(make_blob(records));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 2);
    REQUIRE((*parsed)[0].op == 'W');
    REQUIRE((*parsed)[0] == format::trace::TraceRecord{'W', 0, 4096, 0});
    REQUIRE((*parsed)[1].op == '\x7F');
    REQUIRE((*parsed)[1] == format::trace::TraceRecord{'\x7F', 2, 512, 1024});

    // Nonzero padding is checksummed but not interpreted (§8): recompute
    // the CRC over the bytes as stored and the blob still parses.
    std::vector<uint8_t> padded;
    put_record(padded, 'R', 1, 4096, 0);
    padded[1] = 0x02;  // observed at upstream -O0 builds (§12.1)
    auto parsed2 = format::trace::parse(make_blob(padded));
    REQUIRE(parsed2.has_value());
    REQUIRE(parsed2->size() == 1);
    REQUIRE((*parsed2)[0] == format::trace::TraceRecord{'R', 1, 4096, 0});
}

TEST_CASE("format: trace writer enforces the conforming-writer contract",
          "[format]") {
    // trace-format.md §10 rule 4: op 'R' only, 1 <= count <= 1048576,
    // offset >= 0. Rejected appends leave the blob unchanged.
    format::trace::TraceWriter w;
    REQUIRE(w.append({'R', 0, 1, 0}));  // minimal valid record
    REQUIRE(w.record_count() == 1);

    REQUIRE(!w.append({'R', 0, 0, 0}));                          // count 0
    REQUIRE(!w.append({'R', 0, 1048577, 0}));                    // > 1 MiB
    REQUIRE(!w.append({'W', 0, 4096, 0}));                       // op 'W'
    REQUIRE(!w.append({'R', 0, 4096, -1}));                      // negative offset
    REQUIRE(w.record_count() == 1);

    // Boundary values pass.
    REQUIRE(w.append({'R', 0, format::trace::kMaxRecordCount, 0}));
    REQUIRE(w.record_count() == 2);

    // The blob with only the accepted records still parses.
    auto parsed = format::trace::parse(w.finalize());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 2);
}
