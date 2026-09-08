// Unit tests: writable layers (ADR-0008) — sparse and LSMT-RW
// implementations, in-place edit semantics, seal compaction, and the
// merged writable view.
#include "common/sha256.hpp"
#include "format/lsmt.hpp"
#include "format/lsmt_rw.hpp"
#include "format/merged_writable.hpp"
#include "format/sparse_rw.hpp"
#include "format/writer.hpp"
#include "image/image_file.hpp"

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/stat.h>

using namespace obd;
using obd::test::TempDir;

namespace {

uint64_t file_bytes(const std::string& path) {
    struct stat st {};
    REQUIRE(::stat(path.c_str(), &st) == 0);
    return static_cast<uint64_t>(st.st_size);
}

std::vector<uint8_t> sectors_pattern(uint64_t first_sector, uint64_t n,
                                     uint32_t seed) {
    return test::pattern_bytes(static_cast<size_t>(n) * 512,
                               seed + static_cast<uint32_t>(first_sector));
}

void make_lsmt_lower(const std::string& dir_path, const std::string& name,
                     const std::vector<uint8_t>& raw, std::string* out) {
    const std::string rawp =
        test::write_file(dir_path + "/" + name + ".img", raw);
    const int fd = ::open(rawp.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    *out = dir_path + "/" + name + ".lsmt";
    format::write_lsmt_single_layer(fd, raw.size(), *out, {});
    ::close(fd);
}

/// Synchronous whole-file sha256 (test-side digest oracle for the
/// determinism assertions).
std::string file_sha256(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    common::Sha256 h;
    std::vector<uint8_t> buf(1 << 16);
    for (;;) {
        const ssize_t r = ::read(fd, buf.data(), buf.size());
        REQUIRE(r >= 0);
        if (r == 0) break;
        h.update(buf.data(), static_cast<size_t>(r));
    }
    ::close(fd);
    return h.final_hex();
}

/// The uuid recorded in a sealed LSMT file's header.
std::string sealed_header_uuid(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    uint8_t region[4096];
    REQUIRE(::read(fd, region, sizeof(region)) ==
            static_cast<ssize_t>(sizeof(region)));
    ::close(fd);
    return format::lsmt::HeaderTrailer::parse(region).uuid;
}

}  // namespace

TEST_CASE("format: sparse layer writes, reads and recovers extents",
          "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.sparse";
    const auto a = sectors_pattern(8, 8, 100);   // sectors [8,16)
    const auto b = sectors_pattern(10, 4, 200);  // sectors [10,14)

    int rc = test::run_coro([&]() -> elio::coro::task<int> {
        {
            auto layer =
                co_await format::SparseRwLayer::open(path, 512 * 64);
            ssize_t r =
                co_await layer->pwrite(a.data(), a.size(), 8 * 512);
            REQUIRE(r == static_cast<ssize_t>(a.size()));
            // Partial rewrite inside the extent: identity segments merge.
            r = co_await layer->pwrite(b.data(), b.size(), 10 * 512);
            REQUIRE(r == static_cast<ssize_t>(b.size()));
            REQUIRE(layer->segments().size() == 1);
            REQUIRE(layer->segments()[0].offset == 8);
            REQUIRE(layer->segments()[0].length == 8);
            // Unaligned writes are rejected.
            r = co_await layer->pwrite(a.data(), 100, 3);
            REQUIRE(r == -EINVAL);
            // Holes read as zeroes; written data reads back.
            std::vector<uint8_t> buf(512 * 16);
            r = co_await layer->pread(buf.data(), buf.size(), 4 * 512);
            REQUIRE(r == static_cast<ssize_t>(buf.size()));
            for (size_t i = 0; i < 4 * 512; ++i) REQUIRE(buf[i] == 0);
            REQUIRE(std::memcmp(buf.data() + 4 * 512, a.data(), 2 * 512) ==
                    0);
            REQUIRE(std::memcmp(buf.data() + 6 * 512, b.data(), 4 * 512) ==
                    0);
            REQUIRE(std::memcmp(buf.data() + 10 * 512, a.data() + 6 * 512,
                                2 * 512) == 0);
            int frc = co_await layer->flush();
            REQUIRE(frc == 0);
        }
        // Reopen: coverage must be recovered from the fiemap.
        {
            auto layer =
                co_await format::SparseRwLayer::open(path, 512 * 64);
            REQUIRE(layer->segments().size() == 1);
            REQUIRE(layer->segments()[0].offset == 8);
            REQUIRE(layer->segments()[0].length == 8);
            std::vector<uint8_t> buf(4 * 512);
            ssize_t r =
                co_await layer->pread(buf.data(), buf.size(), 10 * 512);
            REQUIRE(r == static_cast<ssize_t>(buf.size()));
            REQUIRE(buf == b);
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: lsmt rw overwrites covered data in place", "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const auto a = sectors_pattern(0, 16, 300);   // sectors [0,16)
    const auto b = sectors_pattern(4, 4, 400);    // sectors [4,8)
    const auto c = sectors_pattern(32, 8, 500);   // sectors [32,40)
    const auto d = sectors_pattern(36, 8, 600);   // sectors [36,44)

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtRwLayer::create(path, 512 * 64);
        ssize_t r = co_await layer->pwrite(a.data(), a.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(a.size()));
        const uint64_t after_a = file_bytes(path);  // header + 16 sectors

        // In-place edit: fully covered subrange must NOT grow the file.
        r = co_await layer->pwrite(b.data(), b.size(), 4 * 512);
        REQUIRE(r == static_cast<ssize_t>(b.size()));
        REQUIRE(file_bytes(path) == after_a);
        // Index coalesces back to a single contiguous segment.
        REQUIRE(layer->segments().size() == 1);
        REQUIRE(layer->segments()[0].offset == 0);
        REQUIRE(layer->segments()[0].length == 16);

        // Disjoint write appends exactly its own data.
        r = co_await layer->pwrite(c.data(), c.size(), 32 * 512);
        REQUIRE(r == static_cast<ssize_t>(c.size()));
        REQUIRE(file_bytes(path) == after_a + 8 * 512);
        REQUIRE(layer->segments().size() == 2);

        // Straddling write: covered part in place, tail appended.
        r = co_await layer->pwrite(d.data(), d.size(), 36 * 512);
        REQUIRE(r == static_cast<ssize_t>(d.size()));
        REQUIRE(file_bytes(path) == after_a + 8 * 512 + 4 * 512);
        // [32,44) now one contiguous segment.
        REQUIRE(layer->segments().size() == 2);
        REQUIRE(layer->segments()[1].offset == 32);
        REQUIRE(layer->segments()[1].length == 12);

        // Read the whole patched view back.
        std::vector<uint8_t> buf(512 * 64);
        r = co_await layer->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(std::memcmp(buf.data(), a.data(), 4 * 512) == 0);
        REQUIRE(std::memcmp(buf.data() + 4 * 512, b.data(), 4 * 512) == 0);
        REQUIRE(std::memcmp(buf.data() + 8 * 512, a.data() + 8 * 512,
                            8 * 512) == 0);
        for (size_t i = 16 * 512; i < 32 * 512; ++i) REQUIRE(buf[i] == 0);
        REQUIRE(std::memcmp(buf.data() + 32 * 512, c.data(), 4 * 512) == 0);
        REQUIRE(std::memcmp(buf.data() + 36 * 512, d.data(), 8 * 512) == 0);
        int frc = co_await layer->flush();
        REQUIRE(frc == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: lsmt rw seal compacts into a standard sealed layer",
          "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const auto a = sectors_pattern(0, 16, 700);
    const auto b = sectors_pattern(4, 4, 800);
    const auto d = sectors_pattern(32, 8, 900);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtRwLayer::create(path, 512 * 64);
        ssize_t r = co_await layer->pwrite(a.data(), a.size(), 0);
        REQUIRE(r > 0);
        r = co_await layer->pwrite(b.data(), b.size(), 4 * 512);
        REQUIRE(r > 0);
        r = co_await layer->pwrite(d.data(), d.size(), 32 * 512);
        REQUIRE(r > 0);
        int src = co_await layer->seal("test-seal");
        REQUIRE(src == 0);
        REQUIRE(layer->sealed());
        r = co_await layer->pwrite(a.data(), 512, 0);
        REQUIRE(r == -EROFS);

        // Sealed geometry: header 8s + data 24s = 32 sectors (already
        // aligned); index 2 entries (32B) padded to 4096; trailer 4096.
        REQUIRE(file_bytes(path) == 32 * 512 + 4096 + 4096);

        // The sealed file must load through the read-only path.
        auto ro = co_await source::LocalFileSource::open(path);
        source::BlobSourcePtr base = std::move(ro);
        auto ro_layer = co_await format::LsmtLayer::open(std::move(base));
        REQUIRE(ro_layer->virtual_size() == 512 * 64);
        REQUIRE(ro_layer->segments().size() == 2);
        std::vector<uint8_t> buf(512 * 64);
        r = co_await ro_layer->data_source().pread(buf.data(), 16 * 512,
                                                   8 * 512);
        REQUIRE(r == 16 * 512);
        REQUIRE(std::memcmp(buf.data(), a.data(), 4 * 512) == 0);
        REQUIRE(std::memcmp(buf.data() + 4 * 512, b.data(), 4 * 512) == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: lsmt rw seal is deterministic for identical content",
          "[format]") {
    // ADR-0014 seal determinism: identical upper content must seal to
    // identical bytes (the sealed uuid is content-derived, no clock or
    // randomness). Also catches accidental time/random header fields.
    TempDir dir;
    const std::string p1 = dir / "u1.rw";
    const std::string p2 = dir / "u2.rw";
    const std::string p3 = dir / "u3.rw";
    const auto a = sectors_pattern(0, 16, 700);
    const auto b = sectors_pattern(4, 4, 800);
    const auto d = sectors_pattern(32, 8, 900);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        for (const std::string* p : {&p1, &p2}) {
            auto layer = co_await format::LsmtRwLayer::create(*p, 512 * 64);
            ssize_t r = co_await layer->pwrite(a.data(), a.size(), 0);
            REQUIRE(r > 0);
            r = co_await layer->pwrite(b.data(), b.size(), 4 * 512);
            REQUIRE(r > 0);
            r = co_await layer->pwrite(d.data(), d.size(), 32 * 512);
            REQUIRE(r > 0);
            const int src = co_await layer->seal("det-seal");
            REQUIRE(src == 0);
        }
        // A different write sequence (b missing) must yield a different
        // digest — the determinism is over content, not constant output.
        auto layer = co_await format::LsmtRwLayer::create(p3, 512 * 64);
        ssize_t r = co_await layer->pwrite(a.data(), a.size(), 0);
        REQUIRE(r > 0);
        r = co_await layer->pwrite(d.data(), d.size(), 32 * 512);
        REQUIRE(r > 0);
        const int src3 = co_await layer->seal("det-seal");
        REQUIRE(src3 == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);

    // Double-seal determinism: byte-identical sealed files.
    REQUIRE(file_sha256(p1) == file_sha256(p2));
    REQUIRE(file_sha256(p1) != file_sha256(p3));
    // The sealed uuid is content-derived: equal for equal content, and
    // distinct from the random create-time uuid (36-char uuid shape).
    const std::string u1 = sealed_header_uuid(p1);
    REQUIRE(u1.size() == 36);
    REQUIRE(u1 == sealed_header_uuid(p2));
    REQUIRE(u1 != sealed_header_uuid(p3));
}

TEST_CASE("format: lsmt rw checkpoint persists the index for offline seal",
          "[format]") {
    // ADR-0014 offline commit: the device checkpoints its in-memory index
    // on graceful shutdown; seal_file() then seals the upper from another
    // process (here: a fresh open) without any device alive.
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const auto a = sectors_pattern(0, 16, 700);
    const auto b = sectors_pattern(4, 4, 800);
    const auto d = sectors_pattern(32, 8, 900);
    std::string sha;
    uint64_t size = 0;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        {
            auto layer =
                co_await format::LsmtRwLayer::create(path, 512 * 64);
            ssize_t r = co_await layer->pwrite(a.data(), a.size(), 0);
            REQUIRE(r > 0);
            r = co_await layer->pwrite(b.data(), b.size(), 4 * 512);
            REQUIRE(r > 0);
            r = co_await layer->pwrite(d.data(), d.size(), 32 * 512);
            REQUIRE(r > 0);
            int crc = co_await layer->checkpoint();
            REQUIRE(crc == 0);
            // Terminal: no writes after a checkpoint.
            r = co_await layer->pwrite(a.data(), 512, 0);
            REQUIRE(r == -EROFS);
            crc = co_await layer->discard(0, 512);
            REQUIRE(crc == -EROFS);
        }  // destruction ~ the device process exiting after checkpoint

        // Offline seal: a fresh open over the checkpointed file.
        int src = co_await format::LsmtRwLayer::seal_file(path, "offline",
                                                          &sha, &size);
        REQUIRE(src == 0);
        REQUIRE(sha.size() == 64);
        REQUIRE(size == file_bytes(path));
        REQUIRE(sha == file_sha256(path));

        // The sealed output re-opens as a valid standard RO layer with
        // the patched content (data region starts at sector 8).
        auto ro = co_await source::LocalFileSource::open(path);
        source::BlobSourcePtr base = std::move(ro);
        auto ro_layer = co_await format::LsmtLayer::open(std::move(base));
        REQUIRE(ro_layer->virtual_size() == 512 * 64);
        REQUIRE(ro_layer->segments().size() == 2);
        std::vector<uint8_t> buf(16 * 512);
        ssize_t r = co_await ro_layer->data_source().pread(
            buf.data(), buf.size(), 8 * 512);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(std::memcmp(buf.data(), a.data(), 4 * 512) == 0);
        REQUIRE(std::memcmp(buf.data() + 4 * 512, b.data(), 4 * 512) == 0);

        // Precise error channels.
        src = co_await format::LsmtRwLayer::seal_file(path, "", &sha, &size);
        REQUIRE(src == -EALREADY);
        src = co_await format::LsmtRwLayer::seal_file(dir / "nope.rw", "",
                                                      &sha, &size);
        REQUIRE(src == -ENOENT);
        // A crashed device (no checkpoint) leaves an unsealable file.
        const std::string raw = dir / "crashed.rw";
        auto l2 = co_await format::LsmtRwLayer::create(raw, 512 * 64);
        r = co_await l2->pwrite(a.data(), a.size(), 0);
        REQUIRE(r > 0);
        l2.reset();
        src = co_await format::LsmtRwLayer::seal_file(raw, "", &sha, &size);
        REQUIRE(src == -EINVAL);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: merged writable falls through and copy-on-writes",
          "[format]") {
    TempDir dir;
    const auto lower_raw = test::pattern_bytes(512 * 32, 31);
    std::string lower_lsmt;
    make_lsmt_lower(dir.str(), "lower", lower_raw, &lower_lsmt);
    const auto patch = sectors_pattern(8, 8, 950);
    const std::string upper = dir / "upper.rw";

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
        {
            auto s = co_await source::LocalFileSource::open(lower_lsmt);
            source::BlobSourcePtr b = std::move(s);
            layers.push_back(
                co_await format::LsmtLayer::open(std::move(b)));
        }
        auto top = co_await format::LsmtRwLayer::create(upper, 512 * 32);
        auto merged = co_await format::MergedWritable::open(
            std::move(layers), std::move(top));
        REQUIRE(merged->size() == lower_raw.size());
        // Before any write: pure fall-through.
        std::vector<uint8_t> buf(512 * 32);
        ssize_t r = co_await merged->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == lower_raw);
        // Write a patch: reads see it, the lower file stays untouched.
        r = co_await merged->pwrite(patch.data(), patch.size(), 8 * 512);
        REQUIRE(r == static_cast<ssize_t>(patch.size()));
        r = co_await merged->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(std::memcmp(buf.data() + 8 * 512, patch.data(),
                            patch.size()) == 0);
        REQUIRE(std::memcmp(buf.data(), lower_raw.data(), 8 * 512) == 0);
        REQUIRE(std::memcmp(buf.data() + 16 * 512,
                            lower_raw.data() + 16 * 512, 16 * 512) == 0);
        int frc = co_await merged->flush();
        REQUIRE(frc == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
    // Copy-on-write: the lower blob is byte-identical afterwards.
    const int fd = ::open(lower_lsmt.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    struct stat st {};
    REQUIRE(::fstat(fd, &st) == 0);
    std::vector<uint8_t> lower_now(static_cast<size_t>(st.st_size));
    REQUIRE(::read(fd, lower_now.data(), lower_now.size()) ==
            static_cast<ssize_t>(lower_now.size()));
    ::close(fd);
    // The data region of the lower (from sector 8) still holds lower_raw.
    REQUIRE(std::memcmp(lower_now.data() + 8 * 512, lower_raw.data(),
                        lower_raw.size()) == 0);
}

TEST_CASE("format: lsmt rw discard masks coverage with zeroed segments",
          "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const auto a = sectors_pattern(0, 16, 1100);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtRwLayer::create(path, 512 * 64);
        ssize_t r = co_await layer->pwrite(a.data(), a.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(a.size()));
        int dr = co_await layer->discard(4 * 512, 8 * 512);  // [4,12)
        REQUIRE(dr == 0);
        // Index: data [0,4) | zeroed [4,12) | data [12,16).
        REQUIRE(layer->segments().size() == 3);
        REQUIRE(layer->segments()[1].zeroed);
        REQUIRE(layer->segments()[1].offset == 4);
        REQUIRE(layer->segments()[1].length == 8);
        std::vector<uint8_t> buf(512 * 16);
        r = co_await layer->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(std::memcmp(buf.data(), a.data(), 4 * 512) == 0);
        for (size_t i = 4 * 512; i < 12 * 512; ++i) REQUIRE(buf[i] == 0);
        REQUIRE(std::memcmp(buf.data() + 12 * 512, a.data() + 12 * 512,
                            4 * 512) == 0);
        // Alignment and range rules match pwrite.
        dr = co_await layer->discard(3, 100);
        REQUIRE(dr == -EINVAL);

        // Seal: zeroed segments survive into a valid standard LSMT file.
        int src = co_await layer->seal("discard-test");
        REQUIRE(src == 0);
        auto ro = co_await source::LocalFileSource::open(path);
        source::BlobSourcePtr base = std::move(ro);
        auto ro_layer = co_await format::LsmtLayer::open(std::move(base));
        REQUIRE(ro_layer->segments().size() == 3);
        REQUIRE(ro_layer->segments()[1].zeroed);
        // Live data is only 8 sectors; the sealed file must load and the
        // discarded range reads as zeroes through the read-only path.
        std::vector<uint8_t> buf2(512 * 16);
        std::vector<bytes::segment_mapping> idx = ro_layer->segments();
        source::BlobSource& ds = ro_layer->data_source();
        for (const auto& seg : idx) {
            if (seg.zeroed) continue;
            r = co_await ds.pread(buf2.data() + seg.offset * 512,
                                  seg.length * 512, seg.moffset * 512);
            REQUIRE(r == static_cast<ssize_t>(seg.length * 512));
        }
        REQUIRE(std::memcmp(buf2.data(), a.data(), 4 * 512) == 0);
        REQUIRE(std::memcmp(buf2.data() + 12 * 512, a.data() + 12 * 512,
                            4 * 512) == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: sparse layer discard punches holes and recovers",
          "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.sparse";
    const auto a = sectors_pattern(8, 8, 1200);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        {
            auto layer =
                co_await format::SparseRwLayer::open(path, 512 * 64);
            ssize_t r =
                co_await layer->pwrite(a.data(), a.size(), 8 * 512);
            REQUIRE(r == static_cast<ssize_t>(a.size()));
            int dr = co_await layer->discard(10 * 512, 4 * 512);
            REQUIRE(dr == 0);
            REQUIRE(layer->segments().size() == 2);
            REQUIRE(layer->segments()[0].offset == 8);
            REQUIRE(layer->segments()[0].length == 2);
            REQUIRE(layer->segments()[1].offset == 14);
            REQUIRE(layer->segments()[1].length == 2);
            std::vector<uint8_t> buf(512 * 8);
            r = co_await layer->pread(buf.data(), buf.size(), 8 * 512);
            REQUIRE(r == static_cast<ssize_t>(buf.size()));
            REQUIRE(std::memcmp(buf.data(), a.data(), 2 * 512) == 0);
            for (size_t i = 2 * 512; i < 6 * 512; ++i) REQUIRE(buf[i] == 0);
            REQUIRE(std::memcmp(buf.data() + 6 * 512, a.data() + 6 * 512,
                                2 * 512) == 0);
            int frc = co_await layer->flush();
            REQUIRE(frc == 0);
        }
        // Reopen: fiemap recovery is filesystem-block granular (a
        // sub-block punch zeroes but cannot deallocate), so the recovered
        // index may be fatter than the in-memory one — but reads must be
        // identical: data at [8,10) and [14,16), zeroes in between.
        {
            auto layer =
                co_await format::SparseRwLayer::open(path, 512 * 64);
            std::vector<uint8_t> buf(512 * 8);
            ssize_t r =
                co_await layer->pread(buf.data(), buf.size(), 8 * 512);
            REQUIRE(r == static_cast<ssize_t>(buf.size()));
            REQUIRE(std::memcmp(buf.data(), a.data(), 2 * 512) == 0);
            for (size_t i = 2 * 512; i < 6 * 512; ++i) REQUIRE(buf[i] == 0);
            REQUIRE(std::memcmp(buf.data() + 6 * 512, a.data() + 6 * 512,
                                2 * 512) == 0);
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: merged writable discard masks the lower layer",
          "[format]") {
    TempDir dir;
    const auto lower_raw = test::pattern_bytes(512 * 32, 71);
    std::string lower_lsmt;
    make_lsmt_lower(dir.str(), "lower", lower_raw, &lower_lsmt);
    const std::string upper = dir / "upper.rw";

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
        {
            auto s = co_await source::LocalFileSource::open(lower_lsmt);
            source::BlobSourcePtr b = std::move(s);
            layers.push_back(
                co_await format::LsmtLayer::open(std::move(b)));
        }
        auto top = co_await format::LsmtRwLayer::create(upper, 512 * 32);
        auto merged = co_await format::MergedWritable::open(
            std::move(layers), std::move(top));
        // Discard a range the upper never wrote: reads must return zeroes,
        // NOT the lower's data (ADR-0009).
        int dr = co_await merged->discard(8 * 512, 8 * 512);
        REQUIRE(dr == 0);
        std::vector<uint8_t> buf(512 * 32);
        ssize_t r = co_await merged->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        for (size_t i = 8 * 512; i < 16 * 512; ++i) REQUIRE(buf[i] == 0);
        REQUIRE(std::memcmp(buf.data(), lower_raw.data(), 8 * 512) == 0);
        REQUIRE(std::memcmp(buf.data() + 16 * 512,
                            lower_raw.data() + 16 * 512, 16 * 512) == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("image: writable upper assembles and serves writes", "[image]") {
    TempDir dir;
    const auto lower_raw = test::pattern_bytes(512 * 32, 61);
    std::string lower_lsmt;
    make_lsmt_lower(dir.str(), "lower", lower_raw, &lower_lsmt);
    nlohmann::json cfgj;
    cfgj["repoBlobUrl"] = "";
    cfgj["lowers"] = nlohmann::json::array(
        {nlohmann::json{{"digest", "sha256:b"}, {"file", lower_lsmt}}});
    cfgj["upper"] =
        nlohmann::json{{"dir", dir / "upper"}, {"type", "lsmt"}};
    const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
    REQUIRE(cfg.writable());

    const auto patch = sectors_pattern(4, 4, 990);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        const image::GlobalConfig global;
        auto opened = co_await image::open_image(cfg, global);
        REQUIRE(opened.writable);
        REQUIRE(opened.virtual_size == lower_raw.size());
        auto* w =
            dynamic_cast<source::WritableBlobSource*>(opened.root.get());
        REQUIRE(w != nullptr);
        ssize_t r = co_await w->pwrite(patch.data(), patch.size(), 4 * 512);
        REQUIRE(r == static_cast<ssize_t>(patch.size()));
        std::vector<uint8_t> buf(512 * 32);
        r = co_await w->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(std::memcmp(buf.data() + 4 * 512, patch.data(),
                            patch.size()) == 0);
        REQUIRE(std::memcmp(buf.data() + 8 * 512,
                            lower_raw.data() + 8 * 512, 24 * 512) == 0);
        int frc = co_await w->flush();
        REQUIRE(frc == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}
