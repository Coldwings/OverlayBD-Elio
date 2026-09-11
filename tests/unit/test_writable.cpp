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

#include <elio/runtime/spawn_blocking.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace obd;
using obd::test::TempDir;

namespace obd::format {

struct LsmtRwLayerTestAccess {
    static int begin_grow(LsmtRwLayer& layer) {
        return layer.begin_grow_op();
    }
    static void end_grow(LsmtRwLayer& layer) { layer.end_grow_op(); }
    static int begin_terminal(LsmtRwLayer& layer, bool allow_checkpointed) {
        return layer.begin_terminal_op(allow_checkpointed);
    }
    static void end_terminal(LsmtRwLayer& layer) {
        layer.end_terminal_op();
    }
};

struct SparseRwLayerTestAccess {
    static int begin_data(SparseRwLayer& layer, uint64_t offset,
                          uint64_t len) {
        return layer.begin_data_op(offset, len);
    }
    static void end_data(SparseRwLayer& layer) { layer.end_data_op(); }
    static void end_active(SparseRwLayer& layer) { layer.end_data_op(); }
    static int begin_flush(SparseRwLayer& layer, bool* already_checkpointed) {
        return layer.begin_flush_op(already_checkpointed);
    }
    static int begin_grow(SparseRwLayer& layer) {
        return layer.begin_grow_op();
    }
    static void end_grow(SparseRwLayer& layer) { layer.end_grow_op(); }
    static int begin_checkpoint(SparseRwLayer& layer) {
        return layer.begin_checkpoint_op();
    }
    static void end_checkpoint(SparseRwLayer& layer) {
        layer.end_checkpoint_op(SparseRwLayer::Lifecycle::kOpen);
    }
    static int persist_zero_masks(
        SparseRwLayer& layer,
        const std::vector<bytes::segment_mapping>& zeroes,
        uint64_t vsize) {
        return layer.persist_zero_masks(zeroes, vsize).rc;
    }
};

struct MergedWritableTestAccess {
    static int begin_data(MergedWritable& merged, uint64_t offset,
                          uint64_t len) {
        return merged.begin_data_op(offset, len);
    }
    static void end_data(MergedWritable& merged) { merged.end_data_op(); }
    static int begin_flush(MergedWritable& merged) {
        return merged.begin_flush_op();
    }
    static int begin_grow(MergedWritable& merged) {
        return merged.begin_grow_op();
    }
    static void end_grow(MergedWritable& merged) { merged.end_grow_op(); }
    static void invalidate_index(MergedWritable& merged) {
        merged.invalidate_index();
    }
};

}  // namespace obd::format

namespace {

uint64_t file_bytes(const std::string& path) {
    struct stat st {};
    REQUIRE(::stat(path.c_str(), &st) == 0);
    return static_cast<uint64_t>(st.st_size);
}

std::vector<uint8_t> make_sparse_zero_mask(
    uint64_t vsize,
    std::initializer_list<std::pair<uint64_t, uint64_t>> zeroes) {
    constexpr size_t kHeaderSize = 32;
    constexpr char kMagic[8] = {'O', 'B', 'D', 'S', 'P', 'Z', 'M', '1'};
    std::vector<uint8_t> raw(
        kHeaderSize + zeroes.size() * bytes::segment_mapping::kEncodedSize,
        0);
    std::memcpy(raw.data(), kMagic, sizeof(kMagic));
    bytes::store_u64_le(raw.data() + 8, vsize);
    bytes::store_u64_le(raw.data() + 16, zeroes.size());
    size_t i = 0;
    for (const auto& [offset, length] : zeroes) {
        bytes::segment_mapping z;
        z.offset = offset;
        z.length = static_cast<uint32_t>(length);
        z.moffset = 0;
        z.zeroed = true;
        z.tag = 0;
        bytes::store_segment_le(
            raw.data() + kHeaderSize +
                i * bytes::segment_mapping::kEncodedSize,
            z);
        ++i;
    }
    return raw;
}

void write_sparse_zero_mask(const std::string& path, uint64_t vsize,
                            std::initializer_list<std::pair<uint64_t, uint64_t>>
                                zeroes) {
    const auto raw = make_sparse_zero_mask(vsize, zeroes);
    const std::string sidecar = path + ".zeroes";
    const int fd =
        ::open(sidecar.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    REQUIRE(fd >= 0);
    REQUIRE(::pwrite(fd, raw.data(), raw.size(), 0) ==
            static_cast<ssize_t>(raw.size()));
    REQUIRE(::close(fd) == 0);
}

uint64_t sparse_zero_mask_vsize(const std::string& path) {
    constexpr size_t kHeaderSize = 32;
    constexpr char kMagic[8] = {'O', 'B', 'D', 'S', 'P', 'Z', 'M', '1'};
    const std::string sidecar = path + ".zeroes";
    const int fd = ::open(sidecar.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    std::vector<uint8_t> hdr(kHeaderSize);
    REQUIRE(::pread(fd, hdr.data(), hdr.size(), 0) ==
            static_cast<ssize_t>(hdr.size()));
    REQUIRE(::close(fd) == 0);
    REQUIRE(std::memcmp(hdr.data(), kMagic, sizeof(kMagic)) == 0);
    return bytes::load_u64_le(hdr.data() + 8);
}

elio::coro::task<int> expect_sparse_zero_mask_reject(
    std::string path, std::vector<uint8_t> raw, std::string needle,
    uint64_t open_vsize = 512 * 64) {
    test::write_file(path + ".zeroes", raw);
    bool threw = false;
    try {
        auto reopened = co_await format::SparseRwLayer::open(path, open_vsize);
        (void)reopened;
    } catch (const std::exception& e) {
        threw = true;
        REQUIRE(std::string(e.what()).find(needle) != std::string::npos);
    }
    REQUIRE(threw);
    co_return 0;
}

// Match the backing inode, including after seal() replaces its pathname.
// Runtime descriptors and the /proc directory iterator cannot affect this count.
struct BackingInode {
    dev_t device;
    ino_t inode;

    explicit BackingInode(const std::string& path) {
        struct stat st {};
        REQUIRE(::stat(path.c_str(), &st) == 0);
        device = st.st_dev;
        inode = st.st_ino;
    }

    size_t descriptors() const {
        size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
            struct stat st {};
            // Descriptors unrelated to this fixture may disappear during the scan.
            if (::stat(entry.path().c_str(), &st) == 0 &&
                st.st_dev == device && st.st_ino == inode) {
                ++count;
            }
        }
        return count;
    }
};

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

TEST_CASE("format: lsmt rw seal rebinds live reads to the compacted inode",
          "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const auto low = sectors_pattern(0, 1, 720);
    const auto high = sectors_pattern(16, 1, 721);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtRwLayer::create(path, 512 * 64);
        const BackingInode original(path);

        // Allocate the old inode in the reverse physical order: high, then low.
        ssize_t r = co_await layer->pwrite(high.data(), high.size(), 16 * 512);
        REQUIRE(r == static_cast<ssize_t>(high.size()));
        r = co_await layer->pwrite(low.data(), low.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(low.size()));
        const int drc = co_await layer->discard(32 * 512, 512);
        REQUIRE(drc == 0);

        std::vector<uint8_t> buf(512);
        r = co_await layer->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == low);
        r = co_await layer->pread(buf.data(), buf.size(), 16 * 512);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == high);

        const auto& before = layer->segments();
        REQUIRE(before.size() == 3);
        REQUIRE(before[0].offset == 0);
        REQUIRE(before[1].offset == 16);
        REQUIRE(before[2].zeroed);
        source::BlobSource& live_before = layer->data_source();
        r = co_await live_before.pread(buf.data(), buf.size(),
                                       before[0].moffset * 512);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == low);
        r = co_await live_before.pread(buf.data(), buf.size(),
                                       before[1].moffset * 512);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == high);
        const uint64_t old_size = live_before.size();

        const int src = co_await layer->seal("live-rebind");
        REQUIRE(src == 0);
        REQUIRE(layer->sealed());
        const BackingInode published(path);
        REQUIRE(original.descriptors() == 0);
        REQUIRE(published.descriptors() == 1);

        r = co_await layer->pwrite(low.data(), low.size(), 0);
        REQUIRE(r == -EROFS);
        r = co_await layer->discard(32 * 512, 512);
        REQUIRE(r == -EROFS);
        REQUIRE(layer->grow(512 * 128) == -EROFS);
        const int repeat_seal = co_await layer->seal("again");
        REQUIRE(repeat_seal == -EROFS);

        r = co_await layer->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == low);
        r = co_await layer->pread(buf.data(), buf.size(), 16 * 512);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == high);
        r = co_await layer->pread(buf.data(), buf.size(), 32 * 512);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(std::all_of(buf.begin(), buf.end(),
                            [](uint8_t v) { return v == 0; }));

        const auto& after = layer->segments();
        REQUIRE(after.size() == 3);
        REQUIRE(after[0].offset == 0);
        REQUIRE(after[1].offset == 16);
        REQUIRE(after[2].zeroed);
        r = co_await live_before.pread(buf.data(), buf.size(),
                                       after[0].moffset * 512);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == low);
        r = co_await live_before.pread(buf.data(), buf.size(),
                                       after[1].moffset * 512);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == high);
        source::BlobSource& live_after = layer->data_source();
        r = co_await live_after.pread(buf.data(), buf.size(),
                                      after[0].moffset * 512);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == low);
        r = co_await live_after.pread(buf.data(), buf.size(),
                                      after[1].moffset * 512);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == high);
        REQUIRE(live_after.size() == file_bytes(path));
        REQUIRE(live_after.size() > old_size);

        auto ro_source = co_await source::LocalFileSource::open(path);
        auto reopened =
            co_await format::LsmtLayer::open(std::move(ro_source));
        REQUIRE(reopened->segments().size() == 3);
        source::BlobSource& reopened_data = reopened->data_source();
        r = co_await reopened_data.pread(buf.data(), buf.size(),
                                         reopened->segments()[0].moffset * 512);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == low);
        r = co_await reopened_data.pread(buf.data(), buf.size(),
                                         reopened->segments()[1].moffset * 512);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == high);
        REQUIRE(reopened->segments()[2].zeroed);
        reopened.reset();
        layer.reset();
        REQUIRE(published.descriptors() == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: lsmt rw failed live seal keeps original backing",
          "[format][lsmt-fd]") {
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const auto payload = sectors_pattern(0, 2, 722);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtRwLayer::create(path, 512 * 64);
        const BackingInode original(path);
        const ssize_t wrote =
            co_await layer->pwrite(payload.data(), payload.size(), 0);
        REQUIRE(wrote == static_cast<ssize_t>(payload.size()));
        const std::string before = file_sha256(path);
        const std::string tmp = path + ".sealing." + std::to_string(::getpid());

        bool threw = false;
        try {
            const int unexpected =
                co_await layer->seal(std::string(257, 'x'));
            (void)unexpected;
        } catch (const std::system_error& e) {
            REQUIRE(e.code().value() == EINVAL);
            threw = true;
        }
        REQUIRE(threw);
        REQUIRE_FALSE(std::filesystem::exists(tmp));
        REQUIRE_FALSE(layer->sealed());
        REQUIRE(file_sha256(path) == before);
        REQUIRE(original.descriptors() == 1);

        std::vector<uint8_t> buf(payload.size());
        const ssize_t got = co_await layer->pread(buf.data(), buf.size(), 0);
        REQUIRE(got == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == payload);
        const int checkpointed = co_await layer->checkpoint();
        REQUIRE(checkpointed == 0);
        layer.reset();
        REQUIRE(original.descriptors() == 0);

        std::string sha;
        uint64_t size = 0;
        const int sealed = co_await format::LsmtRwLayer::seal_file(
            path, "after-failed-live-seal", &sha, &size);
        REQUIRE(sealed == 0);
        REQUIRE(sha == file_sha256(path));
        REQUIRE(size == file_bytes(path));

        auto ro_source = co_await source::LocalFileSource::open(path);
        auto reopened =
            co_await format::LsmtLayer::open(std::move(ro_source));
        source::BlobSource& reopened_data = reopened->data_source();
        const ssize_t reopened_got =
            co_await reopened_data.pread(buf.data(), buf.size(),
                                         reopened->segments()[0].moffset * 512);
        REQUIRE(reopened_got == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == payload);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: lsmt rw direct terminal and grow gates reject overlaps",
          "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const std::string path2 = dir / "upper2.rw";

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtRwLayer::create(path, 512 * 64);

        int gate = format::LsmtRwLayerTestAccess::begin_terminal(*layer,
                                                                 false);
        REQUIRE(gate == 0);
        int grow_busy = co_await elio::spawn_blocking(
            [&] { return layer->grow(512 * 128); });
        REQUIRE(grow_busy == -EBUSY);
        const int checkpoint_busy = co_await layer->checkpoint();
        REQUIRE(checkpoint_busy == -EBUSY);
        const int seal_busy = co_await layer->seal("busy");
        REQUIRE(seal_busy == -EBUSY);
        format::LsmtRwLayerTestAccess::end_terminal(*layer);

        int grow_retry = co_await elio::spawn_blocking(
            [&] { return layer->grow(512 * 128); });
        REQUIRE(grow_retry == 0);
        const int checkpoint_retry = co_await layer->checkpoint();
        REQUIRE(checkpoint_retry == 0);

        auto layer2 = co_await format::LsmtRwLayer::create(path2, 512 * 64);
        gate = format::LsmtRwLayerTestAccess::begin_grow(*layer2);
        REQUIRE(gate == 0);
        const int checkpoint_while_grow = co_await layer2->checkpoint();
        REQUIRE(checkpoint_while_grow == -EBUSY);
        const int seal_while_grow = co_await layer2->seal("busy");
        REQUIRE(seal_while_grow == -EBUSY);
        grow_busy = co_await elio::spawn_blocking(
            [&] { return layer2->grow(512 * 128); });
        REQUIRE(grow_busy == -EBUSY);
        format::LsmtRwLayerTestAccess::end_grow(*layer2);

        const int seal_retry = co_await layer2->seal("after-busy");
        REQUIRE(seal_retry == 0);
        grow_busy = co_await elio::spawn_blocking(
            [&] { return layer2->grow(512 * 256); });
        REQUIRE(grow_busy == -EROFS);
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
        // p4: same data writes as p1 plus a discard of an UNCOVERED range
        // (inserts zeroed segments — a no-op for reads, but changes the
        // packed index). A different digest pins that the packed index
        // (not only the data) feeds the content digest.
        const std::string p4 = dir / "u4.rw";
        auto l4 = co_await format::LsmtRwLayer::create(p4, 512 * 64);
        r = co_await l4->pwrite(a.data(), a.size(), 0);
        REQUIRE(r > 0);
        r = co_await l4->pwrite(b.data(), b.size(), 4 * 512);
        REQUIRE(r > 0);
        r = co_await l4->pwrite(d.data(), d.size(), 32 * 512);
        REQUIRE(r > 0);
        int drc = co_await l4->discard(48 * 512, 8 * 512);
        REQUIRE(drc == 0);
        const int src4 = co_await l4->seal("det-seal");
        REQUIRE(src4 == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);

    // Double-seal determinism: byte-identical sealed files.
    REQUIRE(file_sha256(p1) == file_sha256(p2));
    REQUIRE(file_sha256(p1) != file_sha256(p3));
    // Same data, different packed index (zeroed segments): different
    // digest — the index is hashed, not only the data.
    REQUIRE(file_sha256(p1) != file_sha256(dir / "u4.rw"));
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

    // Tampered checkpoint: a checkpointed index entry whose moffset lies
    // outside the data region must fail validation (-EINVAL), not seal.
    const std::string tampered = dir / "tampered.rw";
    int crc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtRwLayer::create(tampered,
                                                          512 * 64);
        ssize_t r = co_await layer->pwrite(a.data(), a.size(), 0);
        REQUIRE(r > 0);
        crc = co_await layer->checkpoint();
        REQUIRE(crc == 0);
        co_return 0;
    });
    REQUIRE(crc == 0);
    // Locate the checkpointed index from the trailer and overwrite the
    // first entry with an out-of-range moffset.
    {
        struct stat st {};
        REQUIRE(::stat(tampered.c_str(), &st) == 0);
        const uint64_t trailer_off =
            static_cast<uint64_t>(st.st_size) - format::lsmt::kSpace;
        std::vector<uint8_t> region(format::lsmt::kSpace);
        int fd = ::open(tampered.c_str(), O_RDWR);
        REQUIRE(fd >= 0);
        REQUIRE(::pread(fd, region.data(), region.size(),
                        static_cast<off_t>(trailer_off)) ==
                static_cast<ssize_t>(region.size()));
        const auto tht = format::lsmt::HeaderTrailer::parse(region.data());
        REQUIRE(tht.index_size >= 1);
        bytes::segment_mapping bogus;
        bogus.offset = 0;
        bogus.length = 1;
        bogus.moffset = 0xFFFFFFF;  // beyond index_offset / kSector
        bogus.zeroed = 0;
        uint8_t entry[bytes::segment_mapping::kEncodedSize];
        bytes::store_segment_le(entry, bogus);
        REQUIRE(::pwrite(fd, entry, sizeof(entry),
                         static_cast<off_t>(tht.index_offset)) ==
                static_cast<ssize_t>(sizeof(entry)));
        ::close(fd);
    }
    crc = test::run_coro([&]() -> elio::coro::task<int> {
        std::string s;
        uint64_t n = 0;
        const int src = co_await format::LsmtRwLayer::seal_file(
            tampered, "", &s, &n);
        REQUIRE(src == -EINVAL);
        co_return 0;
    });
    REQUIRE(crc == 0);
}

TEST_CASE("format: lsmt rw offline seal rejects a torn checkpoint trailer",
          "[format]") {
    // A trailer torn mid-write can parse as a VALID BUT EMPTY checkpoint
    // (magic/flags/virtual_size written, uuid and index fields still
    // zero). Without the header cross-check the offline seal would
    // silently seal an empty layer; with it, uuid agreement between the
    // header (offset 0) and the trailer is required → -EINVAL, and the
    // file is left untouched (still unsealed, repair possible).
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const auto a = sectors_pattern(0, 16, 700);

    int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtRwLayer::create(path, 512 * 64);
        ssize_t r = co_await layer->pwrite(a.data(), a.size(), 0);
        REQUIRE(r > 0);
        int crc = co_await layer->checkpoint();
        REQUIRE(crc == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);

    // Simulate the tear: keep magic/size/flags/virtual_size, zero
    // index_offset..(virtual_size) and everything from uuid on.
    {
        struct stat st {};
        REQUIRE(::stat(path.c_str(), &st) == 0);
        const uint64_t trailer_off =
            static_cast<uint64_t>(st.st_size) - format::lsmt::kSpace;
        std::vector<uint8_t> zeros(4096, 0);
        int fd = ::open(path.c_str(), O_RDWR);
        REQUIRE(fd >= 0);
        // bytes [32,48): index_offset + index_size
        REQUIRE(::pwrite(fd, zeros.data(), 16,
                         static_cast<off_t>(trailer_off + 32)) == 16);
        // bytes [56,4096): uuid, parent_uuid, version, user_tag, padding
        REQUIRE(::pwrite(fd, zeros.data(), 4096 - 56,
                         static_cast<off_t>(trailer_off + 56)) ==
                4096 - 56);
        ::close(fd);
    }

    rc = test::run_coro([&]() -> elio::coro::task<int> {
        std::string sha;
        uint64_t size = 0;
        const int src =
            co_await format::LsmtRwLayer::seal_file(path, "", &sha, &size);
        REQUIRE(src == -EINVAL);
        co_return 0;
    });
    REQUIRE(rc == 0);

    // The file was NOT sealed: its header is still the unsealed RW one.
    {
        int fd = ::open(path.c_str(), O_RDONLY);
        REQUIRE(fd >= 0);
        std::vector<uint8_t> region(format::lsmt::kSpace);
        REQUIRE(::pread(fd, region.data(), region.size(), 0) ==
                static_cast<ssize_t>(region.size()));
        ::close(fd);
        const auto hht = format::lsmt::HeaderTrailer::parse(region.data());
        REQUIRE(hht.is_header());
        REQUIRE(!hht.is_sealed());
    }

    // Same malformed-checkpoint family, one field over: an index_offset
    // beyond the trailer with index_size == 0 must NOT underflow the
    // bounds check into sealing an empty layer. Here uuid and
    // virtual_size are intact, so the header cross-check alone would
    // pass — the index_offset bounds guard is what rejects it.
    const std::string path2 = dir / "upper2.rw";
    rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtRwLayer::create(path2, 512 * 64);
        ssize_t r = co_await layer->pwrite(a.data(), a.size(), 0);
        REQUIRE(r > 0);
        int crc = co_await layer->checkpoint();
        REQUIRE(crc == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
    {
        struct stat st {};
        REQUIRE(::stat(path2.c_str(), &st) == 0);
        const uint64_t trailer_off =
            static_cast<uint64_t>(st.st_size) - format::lsmt::kSpace;
        int fd = ::open(path2.c_str(), O_RDWR);
        REQUIRE(fd >= 0);
        uint8_t le[8];
        bytes::store_u64_le(le, static_cast<uint64_t>(st.st_size) +
                                    format::lsmt::kSpace);  // beyond EOF
        REQUIRE(::pwrite(fd, le, 8, static_cast<off_t>(trailer_off + 32)) ==
                8);
        bytes::store_u64_le(le, 0);  // index_size == 0
        REQUIRE(::pwrite(fd, le, 8, static_cast<off_t>(trailer_off + 40)) ==
                8);
        ::close(fd);
    }
    rc = test::run_coro([&]() -> elio::coro::task<int> {
        std::string sha;
        uint64_t size = 0;
        const int src =
            co_await format::LsmtRwLayer::seal_file(path2, "", &sha, &size);
        REQUIRE(src == -EINVAL);
        co_return 0;
    });
    REQUIRE(rc == 0);
    {
        int fd = ::open(path2.c_str(), O_RDONLY);
        REQUIRE(fd >= 0);
        std::vector<uint8_t> region(format::lsmt::kSpace);
        REQUIRE(::pread(fd, region.data(), region.size(), 0) ==
                static_cast<ssize_t>(region.size()));
        ::close(fd);
        const auto hht = format::lsmt::HeaderTrailer::parse(region.data());
        REQUIRE(hht.is_header());
        REQUIRE(!hht.is_sealed());
    }
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

TEST_CASE("format: lsmt rw pwrite over a discarded range appends fresh data",
          "[format]") {
    // Virtual sectors deliberately differ from the physical data start (8).
    // Reusing the discarded tail's placeholder overwrites the live head.
    TempDir dir;
    const auto a = sectors_pattern(24, 16, 1300);  // v[24,40)
    const auto b = sectors_pattern(32, 8, 1400);   // v[32,40)
    REQUIRE(std::memcmp(b.data(), a.data(), b.size()) != 0);
    const std::string path = dir / "upper.rw";
    const std::string control = dir / "control.rw";

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtRwLayer::create(path, 512 * 64);
        ssize_t r = co_await layer->pwrite(a.data(), a.size(), 24 * 512);
        REQUIRE(r == static_cast<ssize_t>(a.size()));
        const int dr = co_await layer->discard(32 * 512, 8 * 512);
        REQUIRE(dr == 0);
        REQUIRE(layer->segments().size() == 2);
        REQUIRE_FALSE(layer->segments()[0].zeroed);
        REQUIRE(layer->segments()[1].zeroed);
        const uint64_t before = file_bytes(path);

        r = co_await layer->pwrite(b.data(), b.size(), 32 * 512);
        REQUIRE(r == static_cast<ssize_t>(b.size()));
        // Check virtual content first: the regression must demonstrate data
        // corruption, not merely a different allocation strategy/file size.
        std::vector<uint8_t> buf(16 * 512, 0xff);
        r = co_await layer->pread(buf.data(), buf.size(), 24 * 512);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(std::memcmp(buf.data(), a.data(), 8 * 512) == 0);
        REQUIRE(std::memcmp(buf.data() + 8 * 512, b.data(), b.size()) == 0);
        REQUIRE(file_bytes(path) == before + b.size());

        std::vector<std::pair<uint64_t, uint64_t>> phys;
        for (const auto& seg : layer->segments()) {
            if (seg.zeroed) continue;
            for (const auto& [lo, hi] : phys) {
                REQUIRE_FALSE((seg.moffset < hi && lo < seg.mend()));
            }
            phys.emplace_back(seg.moffset, seg.mend());
        }
        const int sealed = co_await layer->seal("issue-13");
        REQUIRE(sealed == 0);
        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
        layers.push_back(co_await format::LsmtLayer::open(
            co_await source::LocalFileSource::open(path)));
        auto ro = co_await format::MergedLsmt::open(std::move(layers));
        // Include the leading hole: physical pread at virtual offset 16
        // cannot accidentally stand in for the layer's mapping here.
        std::vector<uint8_t> actual(24 * 512, 0xff);
        r = co_await ro->pread(actual.data(), actual.size(), 16 * 512);
        REQUIRE(r == static_cast<ssize_t>(actual.size()));
        const std::vector<uint8_t> zeros(8 * 512, 0);
        REQUIRE(std::memcmp(actual.data(), zeros.data(), zeros.size()) == 0);
        REQUIRE(std::memcmp(actual.data() + 8 * 512, a.data(), 8 * 512) == 0);
        REQUIRE(std::memcmp(actual.data() + 16 * 512, b.data(), b.size()) == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);

    const int crc = test::run_coro([&]() -> elio::coro::task<int> {
        auto ctl = co_await format::LsmtRwLayer::create(control, 512 * 64);
        ssize_t r = co_await ctl->pwrite(a.data(), a.size(), 24 * 512);
        REQUIRE(r == static_cast<ssize_t>(a.size()));
        r = co_await ctl->pwrite(b.data(), b.size(), 32 * 512);
        REQUIRE(r == static_cast<ssize_t>(b.size()));
        const int sealed = co_await ctl->seal("issue-13");
        REQUIRE(sealed == 0);
        co_return 0;
    });
    REQUIRE(crc == 0);
    // Packing coalesces both histories to the same live index and bytes.
    REQUIRE(file_sha256(path) == file_sha256(control));
}

TEST_CASE("format: lsmt rw straddling pwrite keeps live and discarded parts apart",
          "[format]") {
    TempDir dir;
    const auto a = sectors_pattern(24, 16, 1300);  // v[24,40)
    const auto c = sectors_pattern(30, 6, 1500);   // v[30,36)
    REQUIRE(std::memcmp(c.data(), a.data() + 6 * 512, 2 * 512) != 0);
    const std::string path = dir / "upper.rw";

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtRwLayer::create(path, 512 * 64);
        ssize_t r = co_await layer->pwrite(a.data(), a.size(), 24 * 512);
        REQUIRE(r == static_cast<ssize_t>(a.size()));
        const int dr = co_await layer->discard(32 * 512, 8 * 512);
        REQUIRE(dr == 0);
        const uint64_t before = file_bytes(path);
        // [30,32) stays in place; only discarded [32,36) appends.
        r = co_await layer->pwrite(c.data(), c.size(), 30 * 512);
        REQUIRE(r == static_cast<ssize_t>(c.size()));
        std::vector<uint8_t> expected(16 * 512, 0);
        std::memcpy(expected.data(), a.data(), 6 * 512);
        std::memcpy(expected.data() + 6 * 512, c.data(), c.size());
        std::vector<uint8_t> actual(expected.size(), 0xff);
        r = co_await layer->pread(actual.data(), actual.size(), 24 * 512);
        REQUIRE(r == static_cast<ssize_t>(actual.size()));
        REQUIRE(actual == expected);
        REQUIRE(file_bytes(path) == before + 4 * 512);
        std::vector<std::pair<uint64_t, uint64_t>> phys;
        for (const auto& seg : layer->segments()) {
            if (seg.zeroed) continue;
            for (const auto& [lo, hi] : phys) {
                REQUIRE_FALSE((seg.moffset < hi && lo < seg.mend()));
            }
            phys.emplace_back(seg.moffset, seg.mend());
        }
        const int sealed = co_await layer->seal("issue-13");
        REQUIRE(sealed == 0);
        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
        layers.push_back(co_await format::LsmtLayer::open(
            co_await source::LocalFileSource::open(path)));
        auto ro = co_await format::MergedLsmt::open(std::move(layers));
        std::fill(actual.begin(), actual.end(), 0xff);
        r = co_await ro->pread(actual.data(), actual.size(), 24 * 512);
        REQUIRE(r == static_cast<ssize_t>(actual.size()));
        REQUIRE(actual == expected);  // includes the still-discarded tail
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: lsmt rw partial zeroed rewrites survive offline sealing",
          "[format]") {
    // A large virtual zero range owns no matching physical extent. Trimming
    // it must not advance its placeholder beyond the real data region.
    bool discard_again = false;
    uint64_t live_sector = 1000;
    uint64_t virtual_sectors = 1024;
    uint64_t patch_sector = 500;
    uint64_t patch_sectors = 1;
    SECTION("pwrite splits zeroed coverage") {}
    SECTION("discard splits zeroed coverage before pwrite") {
        discard_again = true;
    }
    SECTION("rewrite crosses the maximum segment and write-piece length") {
        live_sector = 32768;
        virtual_sectors = live_sector + 16;
        patch_sector = 8190;
        patch_sectors = bytes::segment_mapping::kMaxLength + 1;
    }
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const auto live = sectors_pattern(live_sector, 8, 1600);
    const auto patch = sectors_pattern(patch_sector, patch_sectors, 1700);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtRwLayer::create(
            path, virtual_sectors * 512);
        ssize_t r = co_await layer->pwrite(
            live.data(), live.size(), live_sector * 512);
        REQUIRE(r == static_cast<ssize_t>(live.size()));
        int dr = co_await layer->discard(0, live_sector * 512);
        REQUIRE(dr == 0);
        if (discard_again) {
            dr = co_await layer->discard(100 * 512, 100 * 512);
            REQUIRE(dr == 0);
        }
        r = co_await layer->pwrite(
            patch.data(), patch.size(), patch_sector * 512);
        REQUIRE(r == static_cast<ssize_t>(patch.size()));
        const int checkpointed = co_await layer->checkpoint();
        REQUIRE(checkpointed == 0);
        layer.reset();
        std::string digest;
        uint64_t size = 0;
        const int sealed = co_await format::LsmtRwLayer::seal_file(
            path, "issue-13", &digest, &size);
        REQUIRE(sealed == 0);
        REQUIRE(size == file_bytes(path));
        REQUIRE(digest == file_sha256(path));
        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
        layers.push_back(co_await format::LsmtLayer::open(
            co_await source::LocalFileSource::open(path)));
        auto ro = co_await format::MergedLsmt::open(std::move(layers));
        std::vector<uint8_t> expected(virtual_sectors * 512, 0);
        std::memcpy(expected.data() + patch_sector * 512,
                    patch.data(), patch.size());
        std::memcpy(expected.data() + live_sector * 512,
                    live.data(), live.size());
        std::vector<uint8_t> actual(expected.size(), 0xff);
        r = co_await ro->pread(actual.data(), actual.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(actual.size()));
        REQUIRE(actual == expected);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: sparse layer discard keeps a durable zero mask",
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
            REQUIRE(layer->segments().size() == 3);
            REQUIRE(layer->segments()[0].offset == 8);
            REQUIRE(layer->segments()[0].length == 2);
            REQUIRE_FALSE(layer->segments()[0].zeroed);
            REQUIRE(layer->segments()[1].offset == 10);
            REQUIRE(layer->segments()[1].length == 4);
            REQUIRE(layer->segments()[1].zeroed);
            REQUIRE(layer->segments()[2].offset == 14);
            REQUIRE(layer->segments()[2].length == 2);
            REQUIRE_FALSE(layer->segments()[2].zeroed);
            std::vector<uint8_t> buf(512 * 8);
            r = co_await layer->pread(buf.data(), buf.size(), 8 * 512);
            REQUIRE(r == static_cast<ssize_t>(buf.size()));
            REQUIRE(std::memcmp(buf.data(), a.data(), 2 * 512) == 0);
            for (size_t i = 2 * 512; i < 6 * 512; ++i) REQUIRE(buf[i] == 0);
            REQUIRE(std::memcmp(buf.data() + 6 * 512, a.data() + 6 * 512,
                                2 * 512) == 0);
            REQUIRE(::access((path + ".zeroes").c_str(), F_OK) == 0);
            int frc = co_await layer->flush();
            REQUIRE(frc == 0);
            REQUIRE(::access((path + ".zeroes").c_str(), F_OK) == 0);
            int gr = co_await elio::spawn_blocking(
                [&] { return layer->grow(512 * 96); });
            REQUIRE(gr == 0);
        }
        // Reopen with the original configured size: the sparse file grew
        // online, so recovery must keep the larger file-backed window while
        // accepting the older sidecar header.
        {
            auto layer =
                co_await format::SparseRwLayer::open(path, 512 * 64);
            REQUIRE(layer->virtual_size() == 512 * 96);
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

TEST_CASE("format: sparse layer reopen accepts a published grown zero mask",
          "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.sparse";

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        {
            auto layer =
                co_await format::SparseRwLayer::open(path, 512 * 64);
            REQUIRE(layer->virtual_size() == 512 * 64);
        }
        REQUIRE(::truncate(path.c_str(), 512 * 96) == 0);
        write_sparse_zero_mask(path, 512 * 96, {{10, 4}});

        auto reopened =
            co_await format::SparseRwLayer::open(path, 512 * 64);
        REQUIRE(reopened->virtual_size() == 512 * 96);
        REQUIRE(reopened->segments().size() == 1);
        REQUIRE(reopened->segments()[0].offset == 10);
        REQUIRE(reopened->segments()[0].length == 4);
        REQUIRE(reopened->segments()[0].zeroed);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: sparse layer checkpoint persists a dirty zero mask",
          "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.sparse";

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::SparseRwLayer::open(path, 512 * 32);
        const int dr = co_await layer->discard(8 * 512, 4 * 512);
        REQUIRE(dr == 0);
        REQUIRE(::access((path + ".zeroes").c_str(), F_OK) == 0);
        REQUIRE(sparse_zero_mask_vsize(path) == 512 * 32);

        const int gr = co_await elio::spawn_blocking(
            [&] { return layer->grow(512 * 64); });
        REQUIRE(gr == 0);
        REQUIRE(layer->virtual_size() == 512 * 64);
        REQUIRE(sparse_zero_mask_vsize(path) == 512 * 32);

        const int cr = co_await layer->checkpoint();
        REQUIRE(cr == 0);
        REQUIRE(::access((path + ".zeroes").c_str(), F_OK) == 0);
        REQUIRE(sparse_zero_mask_vsize(path) == 512 * 64);

        auto reopened = co_await format::SparseRwLayer::open(path, 512 * 32);
        REQUIRE(reopened->virtual_size() == 512 * 64);
        REQUIRE(reopened->segments().size() == 1);
        REQUIRE(reopened->segments()[0].offset == 8);
        REQUIRE(reopened->segments()[0].length == 4);
        REQUIRE(reopened->segments()[0].zeroed);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: sparse layer checkpoint failure can retry", "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.sparse";
    const auto data = sectors_pattern(0, 1, 1207);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::SparseRwLayer::open(path, 512 * 32);
        const int dr = co_await layer->discard(8 * 512, 4 * 512);
        REQUIRE(dr == 0);
        const std::string sidecar = path + ".zeroes";
        REQUIRE(::unlink(sidecar.c_str()) == 0);
        REQUIRE(::mkdir(sidecar.c_str(), 0700) == 0);

        const int first_checkpoint = co_await layer->checkpoint();
        REQUIRE(first_checkpoint < 0);
        const ssize_t wr = co_await layer->pwrite(data.data(), data.size(), 0);
        REQUIRE(wr == static_cast<ssize_t>(data.size()));

        REQUIRE(::rmdir(sidecar.c_str()) == 0);
        const int retry_checkpoint = co_await layer->checkpoint();
        REQUIRE(retry_checkpoint == 0);
        const ssize_t after_checkpoint =
            co_await layer->pwrite(data.data(), data.size(), 0);
        REQUIRE(after_checkpoint == -EROFS);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: sparse layer checkpoint is terminal", "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.sparse";
    const auto data = sectors_pattern(0, 4, 1206);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::SparseRwLayer::open(path, 512 * 32);
        const int cr = co_await layer->checkpoint();
        REQUIRE(cr == 0);

        const ssize_t wr =
            co_await layer->pwrite(data.data(), data.size(), 0);
        REQUIRE(wr == -EROFS);
        const int dr = co_await layer->discard(8 * 512, 4 * 512);
        REQUIRE(dr == -EROFS);
        const int gr = co_await elio::spawn_blocking(
            [&] { return layer->grow(512 * 64); });
        REQUIRE(gr == -EROFS);
        const int cr2 = co_await layer->checkpoint();
        REQUIRE(cr2 == -EROFS);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: sparse layer grow republishes zero mask size",
          "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.sparse";

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        {
            auto layer =
                co_await format::SparseRwLayer::open(path, 512 * 32);
            const int dr = co_await layer->discard(8 * 512, 4 * 512);
            REQUIRE(dr == 0);
            REQUIRE(sparse_zero_mask_vsize(path) == 512 * 32);

            const int gr = co_await elio::spawn_blocking(
                [&] { return layer->grow(512 * 96); });
            REQUIRE(gr == 0);
            REQUIRE(layer->virtual_size() == 512 * 96);
            REQUIRE(sparse_zero_mask_vsize(path) == 512 * 32);
            int frc = co_await layer->flush();
            REQUIRE(frc == 0);
        }

        REQUIRE(sparse_zero_mask_vsize(path) == 512 * 96);
        auto reopened = co_await format::SparseRwLayer::open(path, 512 * 32);
        REQUIRE(reopened->virtual_size() == 512 * 96);
        REQUIRE(reopened->segments().size() == 1);
        REQUIRE(reopened->segments()[0].offset == 8);
        REQUIRE(reopened->segments()[0].length == 4);
        REQUIRE(reopened->segments()[0].zeroed);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: sparse zero mask sidecar overrides live fiemap coverage",
          "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.sparse";
    const auto live = sectors_pattern(8, 8, 1201);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        {
            auto layer =
                co_await format::SparseRwLayer::open(path, 512 * 64);
            const ssize_t r =
                co_await layer->pwrite(live.data(), live.size(), 8 * 512);
            REQUIRE(r == static_cast<ssize_t>(live.size()));
            const int frc = co_await layer->flush();
            REQUIRE(frc == 0);
        }

        write_sparse_zero_mask(path, 512 * 64, {{10, 4}});

        auto layer = co_await format::SparseRwLayer::open(path, 512 * 64);
        REQUIRE(layer->segments().size() == 3);
        REQUIRE(layer->segments()[0].offset == 8);
        REQUIRE(layer->segments()[0].length == 2);
        REQUIRE_FALSE(layer->segments()[0].zeroed);
        REQUIRE(layer->segments()[1].offset == 10);
        REQUIRE(layer->segments()[1].length == 4);
        REQUIRE(layer->segments()[1].zeroed);
        REQUIRE(layer->segments()[2].offset == 14);
        REQUIRE(layer->segments()[2].length == 2);
        REQUIRE_FALSE(layer->segments()[2].zeroed);

        std::vector<uint8_t> buf(512 * 8, 0xff);
        const ssize_t r = co_await layer->pread(buf.data(), buf.size(),
                                                8 * 512);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(std::memcmp(buf.data(), live.data(), 2 * 512) == 0);
        for (size_t i = 2 * 512; i < 6 * 512; ++i) REQUIRE(buf[i] == 0);
        REQUIRE(std::memcmp(buf.data() + 6 * 512, live.data() + 6 * 512,
                            2 * 512) == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: sparse zero mask sidecar preserves grown size",
          "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.sparse";
    const auto live = sectors_pattern(8, 8, 1202);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        {
            auto layer =
                co_await format::SparseRwLayer::open(path, 512 * 96);
            const ssize_t r =
                co_await layer->pwrite(live.data(), live.size(), 8 * 512);
            REQUIRE(r == static_cast<ssize_t>(live.size()));
            const int frc = co_await layer->flush();
            REQUIRE(frc == 0);
        }

        write_sparse_zero_mask(path, 512 * 96, {{10, 4}, {80, 4}});

        {
            auto layer =
                co_await format::SparseRwLayer::open(path, 512 * 64);
            REQUIRE(layer->virtual_size() == 512 * 96);
            REQUIRE(layer->segments().size() == 4);
            REQUIRE(layer->segments()[0].offset == 8);
            REQUIRE(layer->segments()[0].length == 2);
            REQUIRE_FALSE(layer->segments()[0].zeroed);
            REQUIRE(layer->segments()[1].offset == 10);
            REQUIRE(layer->segments()[1].length == 4);
            REQUIRE(layer->segments()[1].zeroed);
            REQUIRE(layer->segments()[2].offset == 14);
            REQUIRE(layer->segments()[2].length == 2);
            REQUIRE_FALSE(layer->segments()[2].zeroed);
            REQUIRE(layer->segments()[3].offset == 80);
            REQUIRE(layer->segments()[3].length == 4);
            REQUIRE(layer->segments()[3].zeroed);

            std::vector<uint8_t> buf(512 * 8, 0xff);
            const ssize_t r = co_await layer->pread(buf.data(), buf.size(),
                                                    8 * 512);
            REQUIRE(r == static_cast<ssize_t>(buf.size()));
            REQUIRE(std::memcmp(buf.data(), live.data(), 2 * 512) == 0);
            for (size_t i = 2 * 512; i < 6 * 512; ++i) REQUIRE(buf[i] == 0);
            REQUIRE(std::memcmp(buf.data() + 6 * 512, live.data() + 6 * 512,
                                2 * 512) == 0);
            const int frc = co_await layer->flush();
            REQUIRE(frc == 0);
        }

        auto reopened = co_await format::SparseRwLayer::open(path, 512 * 64);
        REQUIRE(reopened->virtual_size() == 512 * 96);
        REQUIRE(reopened->segments().size() == 4);
        REQUIRE(reopened->segments()[1].offset == 10);
        REQUIRE(reopened->segments()[1].length == 4);
        REQUIRE(reopened->segments()[1].zeroed);
        REQUIRE(reopened->segments()[3].offset == 80);
        REQUIRE(reopened->segments()[3].length == 4);
        REQUIRE(reopened->segments()[3].zeroed);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: sparse zero mask writer rejects oversized vsize",
          "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.sparse";

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::SparseRwLayer::open(path, 512 * 64);
        bytes::segment_mapping z;
        z.offset = 8;
        z.length = 1;
        z.zeroed = true;
        const std::vector<bytes::segment_mapping> zeroes{z};
        const int prc = format::SparseRwLayerTestAccess::persist_zero_masks(
            *layer, zeroes,
            (bytes::segment_mapping::kInvalidOffset + 1) * 512);
        REQUIRE(prc == -EFBIG);
        REQUIRE(::access((path + ".zeroes").c_str(), F_OK) != 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: sparse zero mask sidecar rejects malformed metadata",
          "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.sparse";
    auto bad_magic_seed = make_sparse_zero_mask(512 * 64, {{10, 4}});
    bad_magic_seed[0] = 'X';
    std::vector<uint8_t> truncated_seed = {'O', 'B', 'D', 'S',
                                           'P', 'Z', 'M', '1'};
    auto bad_vsize_seed = make_sparse_zero_mask(512 * 64, {{10, 4}});
    bytes::store_u64_le(bad_vsize_seed.data() + 8, 123);
    auto bad_size_seed = make_sparse_zero_mask(512 * 64, {{10, 4}});
    bytes::store_u64_le(bad_size_seed.data() + 16, 2);
    auto huge_count_seed = make_sparse_zero_mask(512 * 64, {});
    bytes::store_u64_le(huge_count_seed.data() + 16,
                        format::lsmt::kMaxRoIndexSize + 1);
    auto oversized_vsize_seed = make_sparse_zero_mask(
        (bytes::segment_mapping::kInvalidOffset + 2) * 512, {});
    auto untrusted_large_vsize_seed = make_sparse_zero_mask(
        (bytes::segment_mapping::kInvalidOffset + 1) * 512, {});
    auto overlapping_seed =
        make_sparse_zero_mask(512 * 64, {{10, 4}, {12, 1}});
    auto overflow_seed = make_sparse_zero_mask(512 * 64, {{63, 2}});
    auto invalid_offset_seed = make_sparse_zero_mask(
        bytes::segment_mapping::kInvalidOffset * 512,
        {{bytes::segment_mapping::kInvalidOffset, 1}});

    const int rc = test::run_coro([&, bad_magic = std::move(bad_magic_seed),
                                    truncated = std::move(truncated_seed),
                                    bad_vsize = std::move(bad_vsize_seed),
                                    bad_size = std::move(bad_size_seed),
                                     huge_count = std::move(huge_count_seed),
                                     oversized_vsize =
                                         std::move(oversized_vsize_seed),
                                     untrusted_large_vsize =
                                         std::move(untrusted_large_vsize_seed),
                                     overlapping = std::move(overlapping_seed),
                                     overflow = std::move(overflow_seed),
                                     invalid_offset =
                                        std::move(invalid_offset_seed)]()
                                       mutable -> elio::coro::task<int> {
        {
            auto layer =
                co_await format::SparseRwLayer::open(path, 512 * 64);
            REQUIRE(layer->virtual_size() == 512 * 64);
        }

        int rejected = co_await expect_sparse_zero_mask_reject(
            path, std::move(bad_magic), std::string("bad magic"));
        REQUIRE(rejected == 0);

        rejected = co_await expect_sparse_zero_mask_reject(
            path, std::move(truncated), std::string("truncated"));
        REQUIRE(rejected == 0);

        rejected = co_await expect_sparse_zero_mask_reject(
            path, std::move(bad_vsize), std::string("vsize mismatch"));
        REQUIRE(rejected == 0);

        rejected = co_await expect_sparse_zero_mask_reject(
            path, std::move(bad_size), std::string("size mismatch"));
        REQUIRE(rejected == 0);

        rejected = co_await expect_sparse_zero_mask_reject(
            path, std::move(huge_count), std::string("exceeds maximum"));
        REQUIRE(rejected == 0);

        rejected = co_await expect_sparse_zero_mask_reject(
            path, std::move(oversized_vsize), std::string("vsize mismatch"));
        REQUIRE(rejected == 0);

        rejected = co_await expect_sparse_zero_mask_reject(
            path, std::move(untrusted_large_vsize),
            std::string("vsize mismatch"),
            (bytes::segment_mapping::kInvalidOffset + 1) * 512);
        REQUIRE(rejected == 0);

        rejected = co_await expect_sparse_zero_mask_reject(
            path, std::move(overlapping), std::string("invalid segment"));
        REQUIRE(rejected == 0);

        rejected = co_await expect_sparse_zero_mask_reject(
            path, std::move(overflow), std::string("invalid segment"));
        REQUIRE(rejected == 0);

        rejected = co_await expect_sparse_zero_mask_reject(
            path, std::move(invalid_offset), std::string("invalid segment"),
            bytes::segment_mapping::kInvalidOffset * 512);
        REQUIRE(rejected == 0);

        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: fresh sparse layer ignores stale zero mask sidecar",
          "[format]") {
    TempDir dir;
    const auto lower_raw = test::pattern_bytes(512 * 32, 74);
    std::string lower_lsmt;
    make_lsmt_lower(dir.str(), "lower", lower_raw, &lower_lsmt);
    const std::string upper = dir / "upper.sparse";

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        {
            auto top = co_await format::SparseRwLayer::open(upper, 512 * 32);
            const int dr = co_await top->discard(8 * 512, 8 * 512);
            REQUIRE(dr == 0);
            const int frc = co_await top->flush();
            REQUIRE(frc == 0);
        }
        REQUIRE(::truncate(upper.c_str(), 0) == 0);

        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
        auto s = co_await source::LocalFileSource::open(lower_lsmt);
        source::BlobSourcePtr b = std::move(s);
        layers.push_back(co_await format::LsmtLayer::open(std::move(b)));
        auto top = co_await format::SparseRwLayer::open(upper, 512 * 32);
        auto merged = co_await format::MergedWritable::open(
            std::move(layers), std::move(top));
        std::vector<uint8_t> buf(512 * 32);
        const ssize_t r = co_await merged->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == lower_raw);
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

TEST_CASE("format: merged writable sparse discard masks the lower layer",
          "[format]") {
    TempDir dir;
    const auto lower_raw = test::pattern_bytes(512 * 32, 73);
    const auto patch = test::pattern_bytes(2 * 512, 91);
    std::string lower_lsmt;
    make_lsmt_lower(dir.str(), "lower", lower_raw, &lower_lsmt);
    const std::string upper = dir / "upper.sparse";

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto open_merged = [&]() ->
            elio::coro::task<std::unique_ptr<format::MergedWritable>> {
            std::vector<std::unique_ptr<format::LsmtLayer>> layers;
            auto s = co_await source::LocalFileSource::open(lower_lsmt);
            source::BlobSourcePtr b = std::move(s);
            layers.push_back(co_await format::LsmtLayer::open(std::move(b)));
            auto top = co_await format::SparseRwLayer::open(upper, 512 * 32);
            co_return co_await format::MergedWritable::open(
                std::move(layers), std::move(top));
        };

        {
            auto merged = co_await open_merged();
            // Discard lower-only coverage through a sparse upper. The upper
            // must retain zero-mask coverage instead of letting the merged
            // index fall through to the lower layer again.
            const int dr = co_await merged->discard(8 * 512, 8 * 512);
            REQUIRE(dr == 0);
            std::vector<uint8_t> buf(512 * 32);
            const ssize_t r = co_await merged->pread(buf.data(), buf.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(buf.size()));
            REQUIRE(std::memcmp(buf.data(), lower_raw.data(), 8 * 512) == 0);
            for (size_t i = 8 * 512; i < 16 * 512; ++i) REQUIRE(buf[i] == 0);
            REQUIRE(std::memcmp(buf.data() + 16 * 512,
                                lower_raw.data() + 16 * 512, 16 * 512) == 0);
            const int frc = co_await merged->flush();
            REQUIRE(frc == 0);
        }

        {
            auto merged = co_await open_merged();
            std::vector<uint8_t> buf(512 * 32);
            const ssize_t r = co_await merged->pread(buf.data(), buf.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(buf.size()));
            REQUIRE(std::memcmp(buf.data(), lower_raw.data(), 8 * 512) == 0);
            for (size_t i = 8 * 512; i < 16 * 512; ++i) REQUIRE(buf[i] == 0);
            REQUIRE(std::memcmp(buf.data() + 16 * 512,
                                lower_raw.data() + 16 * 512, 16 * 512) == 0);
            const ssize_t wr =
                co_await merged->pwrite(patch.data(), patch.size(), 10 * 512);
            REQUIRE(wr == static_cast<ssize_t>(patch.size()));
            const int frc = co_await merged->flush();
            REQUIRE(frc == 0);
        }

        {
            auto merged = co_await open_merged();
            std::vector<uint8_t> buf(512 * 32);
            const ssize_t r = co_await merged->pread(buf.data(), buf.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(buf.size()));
            REQUIRE(std::memcmp(buf.data(), lower_raw.data(), 8 * 512) == 0);
            for (size_t i = 8 * 512; i < 10 * 512; ++i) REQUIRE(buf[i] == 0);
            REQUIRE(std::memcmp(buf.data() + 10 * 512, patch.data(),
                                patch.size()) == 0);
            for (size_t i = 12 * 512; i < 16 * 512; ++i) REQUIRE(buf[i] == 0);
            REQUIRE(std::memcmp(buf.data() + 16 * 512,
                                lower_raw.data() + 16 * 512, 16 * 512) == 0);
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: merged writable sparse discard survives reopen without flush",
          "[format]") {
    TempDir dir;
    const auto lower_raw = test::pattern_bytes(512 * 32, 75);
    std::string lower_lsmt;
    make_lsmt_lower(dir.str(), "lower", lower_raw, &lower_lsmt);
    const std::string upper = dir / "upper.sparse";

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto open_merged = [&]() ->
            elio::coro::task<std::unique_ptr<format::MergedWritable>> {
            std::vector<std::unique_ptr<format::LsmtLayer>> layers;
            auto s = co_await source::LocalFileSource::open(lower_lsmt);
            source::BlobSourcePtr b = std::move(s);
            layers.push_back(co_await format::LsmtLayer::open(std::move(b)));
            auto top = co_await format::SparseRwLayer::open(upper, 512 * 32);
            co_return co_await format::MergedWritable::open(
                std::move(layers), std::move(top));
        };

        {
            auto merged = co_await open_merged();
            const int dr = co_await merged->discard(8 * 512, 8 * 512);
            REQUIRE(dr == 0);
            REQUIRE(::access((upper + ".zeroes").c_str(), F_OK) == 0);
        }

        {
            auto merged = co_await open_merged();
            std::vector<uint8_t> buf(512 * 32);
            const ssize_t r = co_await merged->pread(buf.data(), buf.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(buf.size()));
            REQUIRE(std::memcmp(buf.data(), lower_raw.data(), 8 * 512) == 0);
            for (size_t i = 8 * 512; i < 16 * 512; ++i) REQUIRE(buf[i] == 0);
            REQUIRE(std::memcmp(buf.data() + 16 * 512,
                                lower_raw.data() + 16 * 512, 16 * 512) == 0);
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: sparse write retries pending zero-mask removal before later writes",
          "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.sparse";
    const auto first = test::pattern_bytes(8 * 512, 125);
    const auto second = test::pattern_bytes(512, 126);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        const std::string sidecar = path + ".zeroes";
        {
            auto layer = co_await format::SparseRwLayer::open(path, 512 * 32);
            const int dr = co_await layer->discard(8 * 512, 8 * 512);
            REQUIRE(dr == 0);
            const int initial_flush = co_await layer->flush();
            REQUIRE(initial_flush == 0);
            REQUIRE(::unlink(sidecar.c_str()) == 0);
            REQUIRE(::mkdir(sidecar.c_str(), 0700) == 0);

            const ssize_t first_wr =
                co_await layer->pwrite(first.data(), first.size(), 8 * 512);
            REQUIRE(first_wr < 0);

            const ssize_t second_wr =
                co_await layer->pwrite(second.data(), second.size(), 20 * 512);
            REQUIRE(second_wr < 0);

            REQUIRE(::rmdir(sidecar.c_str()) == 0);
            const int fr = co_await layer->flush();
            REQUIRE(fr == 0);
            const ssize_t retry_wr =
                co_await layer->pwrite(second.data(), second.size(), 20 * 512);
            REQUIRE(retry_wr == static_cast<ssize_t>(second.size()));
            const int final_flush = co_await layer->flush();
            REQUIRE(final_flush == 0);
        }

        auto reopened = co_await format::SparseRwLayer::open(path, 512 * 32);
        std::vector<uint8_t> first_buf(first.size());
        ssize_t r =
            co_await reopened->pread(first_buf.data(), first_buf.size(), 8 * 512);
        REQUIRE(r == static_cast<ssize_t>(first_buf.size()));
        REQUIRE(std::memcmp(first_buf.data(), first.data(), first_buf.size()) ==
                0);
        std::vector<uint8_t> second_buf(second.size());
        r = co_await reopened->pread(second_buf.data(), second_buf.size(),
                                     20 * 512);
        REQUIRE(r == static_cast<ssize_t>(second_buf.size()));
        REQUIRE(std::memcmp(second_buf.data(), second.data(),
                            second_buf.size()) == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: merged writable sparse write over discard survives reopen without flush",
          "[format]") {
    TempDir dir;
    const auto lower_raw = test::pattern_bytes(512 * 32, 77);
    const auto patch = test::pattern_bytes(2 * 512, 92);
    std::string lower_lsmt;
    make_lsmt_lower(dir.str(), "lower", lower_raw, &lower_lsmt);
    const std::string upper = dir / "upper.sparse";

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto open_merged = [&]() ->
            elio::coro::task<std::unique_ptr<format::MergedWritable>> {
            std::vector<std::unique_ptr<format::LsmtLayer>> layers;
            auto s = co_await source::LocalFileSource::open(lower_lsmt);
            source::BlobSourcePtr b = std::move(s);
            layers.push_back(co_await format::LsmtLayer::open(std::move(b)));
            auto top = co_await format::SparseRwLayer::open(upper, 512 * 32);
            co_return co_await format::MergedWritable::open(
                std::move(layers), std::move(top));
        };

        {
            auto merged = co_await open_merged();
            const int dr = co_await merged->discard(8 * 512, 8 * 512);
            REQUIRE(dr == 0);
            const ssize_t wr =
                co_await merged->pwrite(patch.data(), patch.size(), 10 * 512);
            REQUIRE(wr == static_cast<ssize_t>(patch.size()));
        }

        auto merged = co_await open_merged();
        std::vector<uint8_t> buf(512 * 32);
        const ssize_t r = co_await merged->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(std::memcmp(buf.data(), lower_raw.data(), 8 * 512) == 0);
        for (size_t i = 8 * 512; i < 10 * 512; ++i) REQUIRE(buf[i] == 0);
        REQUIRE(std::memcmp(buf.data() + 10 * 512, patch.data(),
                            patch.size()) == 0);
        for (size_t i = 12 * 512; i < 16 * 512; ++i) REQUIRE(buf[i] == 0);
        REQUIRE(std::memcmp(buf.data() + 16 * 512,
                            lower_raw.data() + 16 * 512, 16 * 512) == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: merged writable invalid index fails closed after sparse discard",
          "[format]") {
    TempDir dir;
    const auto lower_raw = test::pattern_bytes(512 * 32, 76);
    std::string lower_lsmt;
    make_lsmt_lower(dir.str(), "lower", lower_raw, &lower_lsmt);
    const std::string upper = dir / "upper.sparse";

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
        auto s = co_await source::LocalFileSource::open(lower_lsmt);
        source::BlobSourcePtr b = std::move(s);
        layers.push_back(co_await format::LsmtLayer::open(std::move(b)));
        auto top = co_await format::SparseRwLayer::open(upper, 512 * 32);
        auto merged = co_await format::MergedWritable::open(
            std::move(layers), std::move(top));

        const int dr = co_await merged->discard(8 * 512, 8 * 512);
        REQUIRE(dr == 0);
        format::MergedWritableTestAccess::invalidate_index(*merged);

        std::vector<uint8_t> buf(512 * 32, 0xff);
        const ssize_t r = co_await merged->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == -EIO);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: sparse grow rejects oversized zero-mask window",
          "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.sparse";
    const uint64_t oversized =
        (bytes::segment_mapping::kInvalidOffset + 1) * 512;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::SparseRwLayer::open(path, 512 * 32);
        const int dr = co_await layer->discard(8 * 512, 4 * 512);
        REQUIRE(dr == 0);

        const int gr = co_await elio::spawn_blocking(
            [&] { return layer->grow(oversized); });
        REQUIRE(gr == -EFBIG);
        REQUIRE(layer->virtual_size() == 512 * 32);
        const int fr = co_await layer->flush();
        REQUIRE(fr == 0);
        REQUIRE(sparse_zero_mask_vsize(path) == 512 * 32);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: sparse grow preserves an unflushed discard mask on reopen",
          "[format]") {
    TempDir dir;
    const auto lower_raw = test::pattern_bytes(512 * 96, 74);
    std::string lower_lsmt;
    make_lsmt_lower(dir.str(), "lower", lower_raw, &lower_lsmt);
    const std::string upper = dir / "upper.sparse";

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        {
            auto top = co_await format::SparseRwLayer::open(upper, 512 * 64);
            const int dr = co_await top->discard(8 * 512, 8 * 512);
            REQUIRE(dr == 0);
            const int gr = co_await elio::spawn_blocking(
                [&] { return top->grow(512 * 96); });
            REQUIRE(gr == 0);
            REQUIRE(top->virtual_size() == 512 * 96);
        }

        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
        auto s = co_await source::LocalFileSource::open(lower_lsmt);
        source::BlobSourcePtr b = std::move(s);
        layers.push_back(co_await format::LsmtLayer::open(std::move(b)));
        auto top = co_await format::SparseRwLayer::open(upper, 512 * 64);
        auto merged = co_await format::MergedWritable::open(
            std::move(layers), std::move(top));

        std::vector<uint8_t> buf(512 * 32);
        const ssize_t r = co_await merged->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(std::memcmp(buf.data(), lower_raw.data(), 8 * 512) == 0);
        for (size_t i = 8 * 512; i < 16 * 512; ++i) REQUIRE(buf[i] == 0);
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

TEST_CASE("format: offline seal rejects a virtual_size below the declared size",
          "[format]") {
    // D3 commit re-baseline, rejection side: seal_file with a
    // virtual_size override smaller than the layer's declared size (the
    // image-declared size at create) is rejected with -EINVAL and a
    // precise reason BEFORE any compaction — the upper stays unsealed
    // and remains committable at its declared size.
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const auto payload = sectors_pattern(0, 16, 701);
    std::string reject;
    std::string sha;
    uint64_t size = 0;
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        {
            auto layer = co_await format::LsmtRwLayer::create(path, 512 * 64);
            const ssize_t r =
                co_await layer->pwrite(payload.data(), payload.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(payload.size()));
            const int crc = co_await layer->checkpoint();
            REQUIRE(crc == 0);
        }
        // Smaller than declared (512*32 < 512*64): grow-only rejection.
        int src = co_await format::LsmtRwLayer::seal_file(
            path, "", &sha, &size, 512 * 32, &reject);
        REQUIRE(src == -EINVAL);
        REQUIRE(reject.find("declared size") != std::string::npos);
        REQUIRE(reject.find("grow-only") != std::string::npos);
        // Misaligned: alignment rejection with its own reason.
        src = co_await format::LsmtRwLayer::seal_file(
            path, "", &sha, &size, 1000, &reject);
        REQUIRE(src == -EINVAL);
        REQUIRE(reject.find("multiple of 512") != std::string::npos);
        // The upper was NOT sealed by the rejected attempts: a plain
        // seal (no override) still succeeds with the declared size and
        // the full payload.
        src = co_await format::LsmtRwLayer::seal_file(path, "", &sha,
                                                      &size);
        REQUIRE(src == 0);
        auto ro = co_await source::LocalFileSource::open(path);
        source::BlobSourcePtr base = std::move(ro);
        auto layer = co_await format::LsmtLayer::open(std::move(base));
        REQUIRE(layer->virtual_size() == 512 * 64);
        std::vector<uint8_t> buf(payload.size());
        const ssize_t r = co_await layer->data_source().pread(
            buf.data(), buf.size(), 8 * 512);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(std::memcmp(buf.data(), payload.data(), buf.size()) == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: offline seal re-baselines the sealed virtual size grow-only",
          "[format]") {
    // D3 commit re-baseline, acceptance side: a virtual_size override
    // >= both the declared size and the content extent is written into
    // the sealed header/trailer (and content digest), so the next create
    // from this layer yields the larger device. The content itself is
    // untouched and still reads back byte-exactly.
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const auto payload = sectors_pattern(0, 16, 702);
    std::string reject;
    std::string sha;
    uint64_t size = 0;
    const uint64_t kRebased = 512 * 96;  // 1.5x the declared 512*64
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        {
            auto layer = co_await format::LsmtRwLayer::create(path, 512 * 64);
            const ssize_t r =
                co_await layer->pwrite(payload.data(), payload.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(payload.size()));
            const int crc = co_await layer->checkpoint();
            REQUIRE(crc == 0);
        }
        const int src = co_await format::LsmtRwLayer::seal_file(
            path, "rebase", &sha, &size, kRebased, &reject);
        REQUIRE(src == 0);
        REQUIRE(reject.empty());
        REQUIRE(sha.size() == 64);
        auto ro = co_await source::LocalFileSource::open(path);
        source::BlobSourcePtr base = std::move(ro);
        auto layer = co_await format::LsmtLayer::open(std::move(base));
        REQUIRE(layer->virtual_size() == kRebased);
        REQUIRE(layer->header().user_tag == "rebase");
        REQUIRE(layer->segments().size() == 1);
        std::vector<uint8_t> buf(payload.size());
        const ssize_t r = co_await layer->data_source().pread(
            buf.data(), buf.size(), 8 * 512);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(std::memcmp(buf.data(), payload.data(), buf.size()) == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: lsmt rw grow extends the write window and persists the size",
          "[format]") {
    // D3 data-plane grow: after grow(), pwrite/discard accept offsets past
    // the original declared size and new data lands in the layer; the
    // on-disk declared-size header is rewritten, so a graceful-shutdown
    // checkpoint and the offline seal stay consistent with the grown size
    // (a plain commit seals the grown declared size). Grow-only: smaller
    // is -EINVAL, equal is an idempotent no-op, misaligned is -EINVAL.
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const auto a = sectors_pattern(0, 16, 710);
    const auto b = sectors_pattern(512 * 64 / 512, 8, 711);  // 8 sectors @ N
    std::string sha;
    uint64_t size = 0;
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        {
            auto layer = co_await format::LsmtRwLayer::create(path, 512 * 64);
            const ssize_t ra =
                co_await layer->pwrite(a.data(), a.size(), 0);
            REQUIRE(ra == static_cast<ssize_t>(a.size()));
            // BLOCKING grow (header rewrite + fsync): must run off the
            // scheduler, exactly like the device resize executor does.
            int g = co_await elio::spawn_blocking(
                [&] { return layer->grow(512 * 96); });
            REQUIRE(g == 0);
            REQUIRE(layer->virtual_size() == 512 * 96);
            // Grow-only: a shrink and a misaligned request are rejected;
            // an equal request is an idempotent no-op.
            g = co_await elio::spawn_blocking(
                [&] { return layer->grow(512 * 64); });
            REQUIRE(g == -EINVAL);
            g = co_await elio::spawn_blocking(
                [&] { return layer->grow(1000); });
            REQUIRE(g == -EINVAL);
            g = co_await elio::spawn_blocking(
                [&] { return layer->grow(512 * 96); });
            REQUIRE(g == 0);
            // Write INTO the grown region (beyond the original 512*64):
            // before the grow this was -EINVAL.
            const ssize_t rb =
                co_await layer->pwrite(b.data(), b.size(), 512 * 64);
            REQUIRE(rb == static_cast<ssize_t>(b.size()));
            std::vector<uint8_t> buf(b.size());
            ssize_t r = co_await layer->pread(buf.data(), buf.size(),
                                              512 * 64);
            REQUIRE(r == static_cast<ssize_t>(buf.size()));
            REQUIRE(std::memcmp(buf.data(), b.data(), b.size()) == 0);
            // The pre-grow content is intact.
            std::vector<uint8_t> buf_a(a.size());
            r = co_await layer->pread(buf_a.data(), buf_a.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(buf_a.size()));
            REQUIRE(std::memcmp(buf_a.data(), a.data(), a.size()) == 0);
            const int crc = co_await layer->checkpoint();
            REQUIRE(crc == 0);
        }
        // Offline seal with NO override keeps the (grown) declared size —
        // this is what makes a post-resize commit seal the larger device.
        const int src = co_await format::LsmtRwLayer::seal_file(
            path, "grown", &sha, &size);
        REQUIRE(src == 0);
        REQUIRE(sha.size() == 64);
        auto ro = co_await source::LocalFileSource::open(path);
        source::BlobSourcePtr base = std::move(ro);
        auto layer = co_await format::LsmtLayer::open(std::move(base));
        REQUIRE(layer->virtual_size() == 512 * 96);
        REQUIRE(layer->header().user_tag == "grown");
        // Re-merge the sealed layer and read BOTH regions back through
        // virtual offsets (a plain single-layer merge).
        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
        layers.push_back(std::move(layer));
        auto merged = co_await format::MergedLsmt::open(std::move(layers));
        std::vector<uint8_t> va(a.size());
        const ssize_t rv =
            co_await merged->pread(va.data(), va.size(), 0);
        REQUIRE(rv == static_cast<ssize_t>(va.size()));
        REQUIRE(std::memcmp(va.data(), a.data(), a.size()) == 0);
        std::vector<uint8_t> vb(b.size());
        const ssize_t rw2 = co_await merged->pread(vb.data(), vb.size(),
                                                    512 * 64);
        REQUIRE(rw2 == static_cast<ssize_t>(vb.size()));
        REQUIRE(std::memcmp(vb.data(), b.data(), b.size()) == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: empty sealed lsmt layer is a zero base of its virtual size",
          "[format]") {
    // ADR-0014 blank-device zero base: a sealed LSMT RO layer with no
    // segments reads as zeroes across its whole virtual size through the
    // ordinary merge path (holes zero-fill), which is what makes a blank
    // raw disk read zeroed from birth.
    TempDir dir;
    const std::string base_path = dir / "zero.lsmt";
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        const uint64_t vsize = 512 * 128;
        const int crc = co_await format::create_empty_lsmt_layer(base_path,
                                                                 vsize);
        REQUIRE(crc == 0);
        // Minimal sealed geometry: header region + trailer region, no data
        // and no index region (index_offset = 4096, index_size = 0).
        REQUIRE(file_bytes(base_path) == 2 * 4096);

        auto ro = co_await source::LocalFileSource::open(base_path);
        source::BlobSourcePtr base = std::move(ro);
        auto layer = co_await format::LsmtLayer::open(std::move(base));
        REQUIRE(layer->virtual_size() == vsize);
        REQUIRE(layer->segments().empty());
        REQUIRE(layer->header().is_sealed());

        // A merged view over the single empty layer serves zeroes across
        // the requested range — including the tail, which never maps to a
        // segment of any layer.
        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
        layers.push_back(std::move(layer));
        auto merged = co_await format::MergedLsmt::open(std::move(layers));
        REQUIRE(merged->size() == vsize);
        // Prefilled with 0xFF (not zeroes): a no-op pread must fail the
        // all-zero assertion instead of passing on a pre-zeroed buffer.
        std::vector<uint8_t> head(512 * 16, 0xFF);
        const ssize_t r =
            co_await merged->pread(head.data(), head.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(head.size()));
        REQUIRE(std::all_of(head.begin(), head.end(),
                            [](uint8_t b) { return b == 0; }));
        std::vector<uint8_t> tail(512, 0xFF);
        const ssize_t r2 = co_await merged->pread(
            tail.data(), tail.size(), vsize - tail.size());
        REQUIRE(r2 == static_cast<ssize_t>(tail.size()));
        REQUIRE(std::all_of(tail.begin(), tail.end(),
                            [](uint8_t b) { return b == 0; }));
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: sparse layer grow extends the write window", "[format]") {
    // D3 data-plane grow for the sparse upper: ftruncate extends the
    // sparse file; pwrite/pread accept the new range (sparse uppers never
    // seal, so the size lives in the file's extent).
    TempDir dir;
    const std::string path = dir / "upper.sparse";
    const auto a = sectors_pattern(0, 16, 712);
    const auto b = sectors_pattern(64, 8, 713);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::SparseRwLayer::open(path, 512 * 64);
        const ssize_t ra = co_await layer->pwrite(a.data(), a.size(), 0);
        REQUIRE(ra == static_cast<ssize_t>(a.size()));
        int g = co_await elio::spawn_blocking(
            [&] { return layer->grow(512 * 96); });
        REQUIRE(g == 0);
        REQUIRE(layer->virtual_size() == 512 * 96);
        g = co_await elio::spawn_blocking(
            [&] { return layer->grow(512 * 64); });
        REQUIRE(g == -EINVAL);  // shrink rejected
        const ssize_t rb =
            co_await layer->pwrite(b.data(), b.size(), 512 * 64);
        REQUIRE(rb == static_cast<ssize_t>(b.size()));
        std::vector<uint8_t> buf(b.size());
        const ssize_t r = co_await layer->pread(buf.data(), buf.size(),
                                                512 * 64);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(std::memcmp(buf.data(), b.data(), b.size()) == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: sparse layer direct grow gates reject overlaps",
          "[format]") {
    TempDir dir;
    const std::string data_path = dir / "sparse-data.sparse";
    const std::string flush_path = dir / "sparse-flush.sparse";
    const std::string checkpoint_path = dir / "sparse-checkpoint.sparse";

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto data_layer =
            co_await format::SparseRwLayer::open(data_path, 512 * 64);
        int gate =
            format::SparseRwLayerTestAccess::begin_data(*data_layer, 0, 512);
        REQUIRE(gate == 0);
        int grow_busy = co_await elio::spawn_blocking(
            [&] { return data_layer->grow(512 * 128); });
        REQUIRE(grow_busy == -EBUSY);
        format::SparseRwLayerTestAccess::end_active(*data_layer);
        int grow_retry = co_await elio::spawn_blocking(
            [&] { return data_layer->grow(512 * 128); });
        REQUIRE(grow_retry == 0);

        auto flush_layer =
            co_await format::SparseRwLayer::open(flush_path, 512 * 64);
        bool already_checkpointed = true;
        gate = format::SparseRwLayerTestAccess::begin_flush(
            *flush_layer, &already_checkpointed);
        REQUIRE(gate == 0);
        REQUIRE_FALSE(already_checkpointed);
        grow_busy = co_await elio::spawn_blocking(
            [&] { return flush_layer->grow(512 * 128); });
        REQUIRE(grow_busy == -EBUSY);
        format::SparseRwLayerTestAccess::end_active(*flush_layer);
        grow_retry = co_await elio::spawn_blocking(
            [&] { return flush_layer->grow(512 * 128); });
        REQUIRE(grow_retry == 0);

        auto checkpoint_layer =
            co_await format::SparseRwLayer::open(checkpoint_path, 512 * 64);
        gate =
            format::SparseRwLayerTestAccess::begin_checkpoint(*checkpoint_layer);
        REQUIRE(gate == 0);
        grow_busy = co_await elio::spawn_blocking(
            [&] { return checkpoint_layer->grow(512 * 128); });
        REQUIRE(grow_busy == -EBUSY);
        format::SparseRwLayerTestAccess::end_checkpoint(*checkpoint_layer);
        grow_retry = co_await elio::spawn_blocking(
            [&] { return checkpoint_layer->grow(512 * 128); });
        REQUIRE(grow_retry == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: sparse layer grow rejects direct operation overlaps",
          "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.sparse";
    const auto payload = sectors_pattern(0, 1, 718);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::SparseRwLayer::open(path, 512 * 64);

        int gate = format::SparseRwLayerTestAccess::begin_grow(*layer);
        REQUIRE(gate == 0);
        ssize_t wr = co_await layer->pwrite(payload.data(), payload.size(), 0);
        REQUIRE(wr == -EBUSY);
        int dr = co_await layer->discard(0, 512);
        REQUIRE(dr == -EBUSY);
        int fr = co_await layer->flush();
        REQUIRE(fr == -EBUSY);
        int cr = co_await layer->checkpoint();
        REQUIRE(cr == -EBUSY);
        format::SparseRwLayerTestAccess::end_grow(*layer);

        gate = format::SparseRwLayerTestAccess::begin_data(*layer, 0, 512);
        REQUIRE(gate == 0);
        int gr = co_await elio::spawn_blocking(
            [&] { return layer->grow(512 * 96); });
        REQUIRE(gr == -EBUSY);
        format::SparseRwLayerTestAccess::end_data(*layer);

        gate = format::SparseRwLayerTestAccess::begin_checkpoint(*layer);
        REQUIRE(gate == 0);
        gr = co_await elio::spawn_blocking(
            [&] { return layer->grow(512 * 96); });
        REQUIRE(gr == -EBUSY);
        format::SparseRwLayerTestAccess::end_checkpoint(*layer);

        gr = co_await elio::spawn_blocking(
            [&] { return layer->grow(512 * 96); });
        REQUIRE(gr == 0);
        REQUIRE(layer->virtual_size() == 512 * 96);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: merged writable grows with its writable top", "[format]") {
    // D3 data-plane grow at the merged-view level: MergedWritable::grow
    // extends the writable top first, then the merged view — pwrite/
    // discard accept the new range and reads of the headroom gap return
    // zeroes until written.
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const auto a = sectors_pattern(0, 16, 714);
    const auto b = sectors_pattern(64, 8, 715);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto top = co_await format::LsmtRwLayer::create(path, 512 * 64);
        const ssize_t ra = co_await top->pwrite(a.data(), a.size(), 0);
        REQUIRE(ra == static_cast<ssize_t>(a.size()));
        std::vector<std::unique_ptr<format::LsmtLayer>> none;
        auto merged =
            co_await format::MergedWritable::open(std::move(none),
                                                  std::move(top));
        REQUIRE(merged->size() == 512 * 64);
        const int g = co_await elio::spawn_blocking(
            [&] { return merged->grow(512 * 96); });
        REQUIRE(g == 0);
        REQUIRE(merged->size() == 512 * 96);
        // Grow-only: shrink rejected.
        const int g2 = co_await elio::spawn_blocking(
            [&] { return merged->grow(512 * 64); });
        REQUIRE(g2 == -EINVAL);
        // Discard into the grown region is accepted too.
        const int drc = co_await merged->discard(512 * 96 - 4096, 4096);
        REQUIRE(drc == 0);
        // Write into the grown region and read it back through the merge.
        const ssize_t rb =
            co_await merged->pwrite(b.data(), b.size(), 512 * 64);
        REQUIRE(rb == static_cast<ssize_t>(b.size()));
        std::vector<uint8_t> buf(b.size());
        const ssize_t r = co_await merged->pread(buf.data(), buf.size(),
                                                 512 * 64);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(std::memcmp(buf.data(), b.data(), b.size()) == 0);
        // An unwritten headroom gap reads as zeroes.
        std::vector<uint8_t> gap(4096, 0xEE);
        const ssize_t rg = co_await merged->pread(gap.data(), gap.size(),
                                                   512 * 80);
        REQUIRE(rg == static_cast<ssize_t>(gap.size()));
        REQUIRE(std::memcmp(gap.data(), std::vector<uint8_t>(4096, 0).data(),
                            gap.size()) == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: merged writable grow rejects direct operation overlaps",
          "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const auto payload = sectors_pattern(0, 1, 717);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto top = co_await format::LsmtRwLayer::create(path, 512 * 64);
        std::vector<std::unique_ptr<format::LsmtLayer>> none;
        auto merged =
            co_await format::MergedWritable::open(std::move(none),
                                                  std::move(top));

        int gate = format::MergedWritableTestAccess::begin_grow(*merged);
        REQUIRE(gate == 0);
        ssize_t wr = co_await merged->pwrite(payload.data(), payload.size(), 0);
        REQUIRE(wr == -EBUSY);
        int dr = co_await merged->discard(0, 512);
        REQUIRE(dr == -EBUSY);
        int fr = co_await merged->flush();
        REQUIRE(fr == -EBUSY);
        format::MergedWritableTestAccess::end_grow(*merged);

        gate = format::MergedWritableTestAccess::begin_data(*merged, 0, 512);
        REQUIRE(gate == 0);
        int gr = co_await elio::spawn_blocking(
            [&] { return merged->grow(512 * 96); });
        REQUIRE(gr == -EBUSY);
        format::MergedWritableTestAccess::end_data(*merged);

        gate = format::MergedWritableTestAccess::begin_flush(*merged);
        REQUIRE(gate == 0);
        gr = co_await elio::spawn_blocking(
            [&] { return merged->grow(512 * 96); });
        REQUIRE(gr == -EBUSY);
        format::MergedWritableTestAccess::end_data(*merged);

        gr = co_await elio::spawn_blocking(
            [&] { return merged->grow(512 * 96); });
        REQUIRE(gr == 0);
        REQUIRE(merged->size() == 512 * 96);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: merged writable sparse grow reads new top data",
          "[format]") {
    TempDir dir;
    const std::string path = dir / "upper.sparse";
    const auto payload = sectors_pattern(64, 8, 716);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto top = co_await format::SparseRwLayer::open(path, 512 * 64);
        std::vector<std::unique_ptr<format::LsmtLayer>> none;
        auto merged =
            co_await format::MergedWritable::open(std::move(none),
                                                  std::move(top));
        const int g = co_await elio::spawn_blocking(
            [&] { return merged->grow(512 * 96); });
        REQUIRE(g == 0);

        const ssize_t wr =
            co_await merged->pwrite(payload.data(), payload.size(), 512 * 64);
        REQUIRE(wr == static_cast<ssize_t>(payload.size()));

        std::vector<uint8_t> buf(payload.size(), 0);
        const ssize_t rd = co_await merged->pread(buf.data(), buf.size(),
                                                  512 * 64);
        REQUIRE(rd == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == payload);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("image: writable assembly grows to the virtual_size headroom override",
          "[image]") {
    // D3 create-time headroom on the REAL writable assembly path:
    // open_image(..., override) sizes the writable top — and the merged
    // data plane — to the override, so writes into the headroom (past the
    // lowers' content) land in the upper and read back through the merge.
    TempDir dir;
    const auto lower_raw = test::pattern_bytes(512 * 32, 716);
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

    const auto patch = sectors_pattern(40, 8, 717);  // into the headroom
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        const image::GlobalConfig global;
        auto opened = co_await image::open_image(cfg, global, 512 * 64);
        REQUIRE(opened.writable);
        // The data plane (and device) are sized at the override.
        REQUIRE(opened.virtual_size == 512 * 64);
        auto* w =
            dynamic_cast<source::WritableBlobSource*>(opened.root.get());
        REQUIRE(w != nullptr);
        // Writing past the lowers' 512*32 content into the headroom:
        // before the override this was -EINVAL.
        const ssize_t r =
            co_await w->pwrite(patch.data(), patch.size(), 40 * 512);
        REQUIRE(r == static_cast<ssize_t>(patch.size()));
        std::vector<uint8_t> buf(512 * 48);
        const ssize_t rd = co_await w->pread(buf.data(), buf.size(), 0);
        REQUIRE(rd == static_cast<ssize_t>(buf.size()));
        // Patch present in the headroom; lower content intact below it.
        REQUIRE(std::memcmp(buf.data() + 40 * 512, patch.data(),
                            patch.size()) == 0);
        REQUIRE(std::memcmp(buf.data() + 8 * 512,
                            lower_raw.data() + 8 * 512, 24 * 512) == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: offline seal rejects a virtual_size below the content extent",
          "[format]") {
    // D3 commit re-baseline: the content-extent guard is defense against
    // a checkpoint whose declared virtual size lies BELOW what its index
    // actually covers (real writers keep extent <= declared, so the guard
    // needs a synthetic fixture). Build a checkpointed layer, patch BOTH
    // the on-disk header and the trailer to a smaller virtual size
    // (keeping uuid/flags, exactly what the cross-check authenticates),
    // then seal with an override >= the patched declared size but < the
    // real content extent: the guard must reject with the precise reason.
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const auto payload = sectors_pattern(0, 16, 718);  // extent = 16 sectors
    std::string reject;
    std::string sha;
    uint64_t size = 0;
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        {
            auto layer = co_await format::LsmtRwLayer::create(path, 512 * 64);
            const ssize_t r =
                co_await layer->pwrite(payload.data(), payload.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(payload.size()));
            const int crc = co_await layer->checkpoint();
            REQUIRE(crc == 0);
        }
        struct stat st {};
        REQUIRE(::stat(path.c_str(), &st) == 0);
        const uint64_t trailer_off =
            static_cast<uint64_t>(st.st_size) - format::lsmt::kSpace;
        const uint64_t patched_vsize = 512 * 8;  // below the 16-sector extent
        auto patch_region = [&](uint64_t off) {
            std::vector<uint8_t> region(format::lsmt::kSpace);
            const int fd = ::open(path.c_str(), O_RDWR);
            REQUIRE(fd >= 0);
            REQUIRE(::pread(fd, region.data(), region.size(),
                            static_cast<off_t>(off)) ==
                    static_cast<ssize_t>(region.size()));
            auto ht = format::lsmt::HeaderTrailer::parse(region.data());
            ht.virtual_size = patched_vsize;
            std::memset(region.data(), 0, region.size());
            ht.serialize(region.data());
            REQUIRE(::pwrite(fd, region.data(), region.size(),
                             static_cast<off_t>(off)) ==
                    static_cast<ssize_t>(region.size()));
            ::close(fd);
        };
        patch_region(0);          // header
        patch_region(trailer_off);  // checkpoint trailer
        // Override between the patched declared size and the content
        // extent: >= declared passes, < extent trips the guard.
        const int src = co_await format::LsmtRwLayer::seal_file(
            path, "", &sha, &size, 512 * 12, &reject);
        REQUIRE(src == -EINVAL);
        REQUIRE(reject.find("content extent") != std::string::npos);
        REQUIRE(reject.find("grow-only") != std::string::npos);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: empty lsmt exceptions release output ownership", "[format]") {
    TempDir dir;
    const std::string path = dir / "empty.lsmt";
    test::write_file(path, std::vector<uint8_t>{'x'});
    // Keep the inode alive even if the helper unlinks it during unwinding.
    const int sentinel = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(sentinel >= 0);
    struct CloseFd {
        int fd;
        ~CloseFd() { ::close(fd); }
    } owner{sentinel};
    const BackingInode backing(path);
    const size_t before = backing.descriptors();
    const std::string oversized_tag(256, 'x');
    bool threw = false;
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        try {
            (void)co_await format::create_empty_lsmt_layer(path, 512, oversized_tag);
        } catch (const std::exception&) {
            threw = true;
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(threw);
    CHECK(backing.descriptors() == before);
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("format: empty lsmt preserves validation and tag boundaries", "[format]") {
    TempDir dir;
    const std::string path = dir / "empty.lsmt";
    test::write_file(path, std::vector<uint8_t>{'x'});
    const std::string valid_tag(255, 't');
    const std::string missing = dir / "missing/empty.lsmt";
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        for (uint64_t size : {uint64_t{0}, uint64_t{513}}) {
            const int invalid = co_await format::create_empty_lsmt_layer(path, size);
            REQUIRE(invalid == -EINVAL);
            REQUIRE(file_bytes(path) == 1);
        }
        const int absent = co_await format::create_empty_lsmt_layer(missing, 512);
        REQUIRE(absent == -ENOENT);
        const int created = co_await format::create_empty_lsmt_layer(path, 512, valid_tag);
        REQUIRE(created == 0);
        REQUIRE(file_bytes(path) == 8192);
        auto source = co_await source::LocalFileSource::open(path);
        auto layer = co_await format::LsmtLayer::open(std::move(source));
        REQUIRE(layer->header().user_tag == valid_tag);
        REQUIRE(layer->virtual_size() == 512);
        REQUIRE(layer->segments().empty());
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: empty lsmt layer bytes are deterministic per virtual size",
          "[format]") {
    // ADR-0014: the empty layer's uuid is derived from the content digest
    // sha256(vsize as LE u64 || no data || no index), so the file bytes are
    // a pure function of vsize. The expected uuid is computed here with an
    // independent digest oracle (test-side), pinning the derivation rather
    // than asserting only by self-consistency.
    TempDir dir;
    const uint64_t vsize = 512 * 256;
    const std::string a = dir / "zero-a.lsmt";
    const std::string b = dir / "zero-b.lsmt";
    const std::string c = dir / "zero-c.lsmt";
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        const int ca = co_await format::create_empty_lsmt_layer(a, vsize);
        REQUIRE(ca == 0);
        const int cb = co_await format::create_empty_lsmt_layer(b, vsize);
        REQUIRE(cb == 0);
        const int cc =
            co_await format::create_empty_lsmt_layer(c, vsize * 2);
        REQUIRE(cc == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(file_sha256(a) == file_sha256(b));
    REQUIRE(file_sha256(a) != file_sha256(c));

    // Content-derived uuid: sha256(LE64 vsize), first 32 hex chars as a
    // 8-4-4-4-12 uuid string.
    common::Sha256 h;
    uint8_t le[8];
    bytes::store_u64_le(le, vsize);
    h.update(le, sizeof(le));
    std::string want = h.final_hex().substr(0, 32);
    want.insert(20, 1, '-');
    want.insert(16, 1, '-');
    want.insert(12, 1, '-');
    want.insert(8, 1, '-');
    REQUIRE(sealed_header_uuid(a) == want);
    REQUIRE(sealed_header_uuid(b) == want);
    REQUIRE(sealed_header_uuid(c) != want);
}

TEST_CASE("image: blank device rejects an unusable workspace path", "[image]") {
    // Review finding: a workspace path occupied by a REGULAR FILE sets
    // create_directories' error_code while exists() stays true, so the
    // old check fell through and the failure surfaced later as a confusing
    // overlaybd.zero error. Every unusable workspace must be reported as
    // such, before any layer work.
    TempDir dir;
    const std::string ws = dir / "not-a-dir";
    test::write_file(ws, std::vector<uint8_t>{'x'});
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        image::BlankDeviceSpec spec;
        spec.size = 512 * 64;
        spec.dir = ws;
        try {
            (void)co_await image::open_blank_device(spec);
            REQUIRE(false);  // a file as the workspace must not succeed
        } catch (const std::exception& e) {
            const std::string msg = e.what();
            REQUIRE(msg.find("workspace") != std::string::npos);
            REQUIRE(msg.find(ws) != std::string::npos);
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("image: blank device assembles a zeroed writable upper", "[image]") {
    // ADR-0014 mode 2: open_blank_device produces a MergedWritable over a
    // sealed EMPTY LSMT zero base + a fresh LSMT-RW upper, both sized to
    // the requested size — the device reads as zeroed from birth and
    // writes land in the upper (commit-able like any ADR-0008 upper).
    TempDir dir;
    const uint64_t vsize = 512 * 64;
    const std::string ws = dir / "blank-ws";
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        image::BlankDeviceSpec spec;
        spec.size = vsize;
        spec.dir = ws;
        auto opened = co_await image::open_blank_device(spec);
        REQUIRE(opened.writable);
        REQUIRE(opened.virtual_size == vsize);
        REQUIRE(opened.upper_path == ws + "/overlaybd.rw");

        // The zero base on disk is a sealed empty LSMT layer of the
        // requested size.
        auto ro = co_await source::LocalFileSource::open(ws +
                                                         "/overlaybd.zero");
        source::BlobSourcePtr base = std::move(ro);
        auto zero_layer =
            co_await format::LsmtLayer::open(std::move(base));
        REQUIRE(zero_layer->virtual_size() == vsize);
        REQUIRE(zero_layer->segments().empty());
        REQUIRE(zero_layer->header().is_sealed());

        auto* w =
            dynamic_cast<source::WritableBlobSource*>(opened.root.get());
        REQUIRE(w != nullptr);

        // A fresh blank device reads zeroes across its whole range.
        std::vector<uint8_t> expect(512 * 32, 0);
        std::vector<uint8_t> buf(512 * 32, 0xFF);
        const ssize_t r0 = co_await w->pread(buf.data(), buf.size(), 0);
        REQUIRE(r0 == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == expect);

        // Writes land in the upper and read back; the untouched regions
        // around them stay zero.
        const auto patch = sectors_pattern(4, 8, 555);
        const ssize_t r1 =
            co_await w->pwrite(patch.data(), patch.size(), 4 * 512);
        REQUIRE(r1 == static_cast<ssize_t>(patch.size()));
        buf.assign(buf.size(), 0xFF);
        const ssize_t r2 = co_await w->pread(buf.data(), buf.size(), 0);
        REQUIRE(r2 == static_cast<ssize_t>(buf.size()));
        REQUIRE(std::memcmp(buf.data() + 4 * 512, patch.data(),
                            patch.size()) == 0);
        REQUIRE(std::memcmp(buf.data(), expect.data(), 4 * 512) == 0);
        REQUIRE(std::memcmp(buf.data() + 4 * 512 + patch.size(),
                            expect.data(),
                            buf.size() - (4 * 512 + patch.size())) == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: lsmt rw destruction releases its backing descriptor", "[format][lsmt-fd]") {
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        for (int i = 0; i < 8; ++i) {
            auto layer = co_await format::LsmtRwLayer::create(path, 512 * 64);
            const BackingInode backing(path);
            REQUIRE(backing.descriptors() == 1);
            // All operations, including reads through the borrowing View, finish
            // before destruction. The layer does not drain work for its caller.
            std::vector<uint8_t> header(4096);
            const ssize_t got = co_await layer->data_source().pread(
                header.data(), header.size(), 0);
            REQUIRE(got == static_cast<ssize_t>(header.size()));
            layer.reset();
            REQUIRE(backing.descriptors() == 0);
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: lsmt rw offline seal releases replaced inode descriptors", "[format][lsmt-fd]") {
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const auto data = sectors_pattern(0, 2, 49);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtRwLayer::create(path, 512 * 64);
        const BackingInode original(path);
        const ssize_t wrote = co_await layer->pwrite(data.data(), data.size(), 0);
        REQUIRE(wrote == static_cast<ssize_t>(data.size()));
        const int checkpoint_rc = co_await layer->checkpoint();
        REQUIRE(checkpoint_rc == 0);
        // Keep the creator alive to distinguish the temporary reopened owner
        // from the creator's descriptor, even on the leaking baseline.
        REQUIRE(original.descriptors() == 1);
        std::string digest;
        uint64_t size = 0;
        const int seal_rc = co_await format::LsmtRwLayer::seal_file(
            path, "fd-regression", &digest, &size);
        REQUIRE(seal_rc == 0);
        const BackingInode sealed(path);
        REQUIRE(sealed.inode != original.inode);
        REQUIRE(original.descriptors() == 1);
        REQUIRE(sealed.descriptors() == 0);
        REQUIRE(digest == file_sha256(path));
        REQUIRE(size == file_bytes(path));
        layer.reset();
        REQUIRE(original.descriptors() == 0);

        auto source = co_await source::LocalFileSource::open(path);
        auto ro = co_await format::LsmtLayer::open(std::move(source));
        std::vector<uint8_t> readback(data.size());
        const ssize_t got = co_await ro->data_source().pread(
            readback.data(), readback.size(), 4096);
        REQUIRE(got == static_cast<ssize_t>(readback.size()));
        REQUIRE(readback == data);
        ro.reset();
        REQUIRE(sealed.descriptors() == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: lsmt rw rejected seal releases its reopened descriptor", "[format][lsmt-fd]") {
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtRwLayer::create(path, 512 * 64);
        const int checkpoint_rc = co_await layer->checkpoint();
        REQUIRE(checkpoint_rc == 0);
        const BackingInode backing(path);
        const std::string before = file_sha256(path);
        for (const uint64_t vsize : {uint64_t{513}, uint64_t{512 * 32}}) {
            std::string reject;
            const int seal_rc = co_await format::LsmtRwLayer::seal_file(
                path, "", nullptr, nullptr, vsize, &reject);
            REQUIRE(seal_rc == -EINVAL);
            REQUIRE_FALSE(reject.empty());
            REQUIRE(file_sha256(path) == before);
            REQUIRE(backing.descriptors() == 1);
        }
        // An ordinary output-open error must also release the reopened owner.
        const std::string tmp = path + ".sealing." + std::to_string(::getpid());
        REQUIRE(std::filesystem::create_directory(tmp));
        const int seal_rc = co_await format::LsmtRwLayer::seal_file(
            path, "", nullptr, nullptr);
        REQUIRE(seal_rc == -EISDIR);
        REQUIRE(file_sha256(path) == before);
        REQUIRE(backing.descriptors() == 1);
        layer.reset();
        REQUIRE(backing.descriptors() == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: lsmt rw seal exceptions release temporary descriptors", "[format][lsmt-fd]") {
    TempDir dir;
    const std::string path = dir / "upper.rw";
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto layer = co_await format::LsmtRwLayer::create(path, 512 * 64);
        const int checkpoint_rc = co_await layer->checkpoint();
        REQUIRE(checkpoint_rc == 0);
        const BackingInode backing(path);
        const std::string before = file_sha256(path);
        const std::string tmp = path + ".sealing." + std::to_string(::getpid());
        // Oversized metadata throws during seal header serialization, after
        // both the reopened backing fd and compaction output fd are acquired.
        test::write_file(tmp, {});
        const BackingInode output(tmp);
        bool threw = false;
        try {
            const int unexpected = co_await format::LsmtRwLayer::seal_file(
                path, std::string(257, 'x'), nullptr, nullptr);
            (void)unexpected;
        } catch (const std::system_error& e) {
            REQUIRE(e.code().value() == EINVAL);
            threw = true;
        }
        REQUIRE(threw);
        REQUIRE(output.descriptors() == 0);
        REQUIRE_FALSE(std::filesystem::exists(tmp));
        REQUIRE(backing.descriptors() == 1);
        REQUIRE(file_sha256(path) == before);
        layer.reset();
        REQUIRE(backing.descriptors() == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: lsmt rw setup failures release their descriptors", "[format][lsmt-fd]") {
    TempDir dir;
    const std::string short_path = test::write_file(dir / "short.rw", {1, 2, 3});
    const std::string corrupt_path = test::write_file(
        dir / "corrupt.rw", std::vector<uint8_t>(8192, 0));
    // Valid header/trailer framing with one invalid physical mapping reaches
    // the error return after the parser has allocated and decoded its index.
    std::vector<uint8_t> invalid_index(3 * format::lsmt::kSpace, 0);
    format::lsmt::HeaderTrailer ht;
    ht.set_flag_bit(format::lsmt::kFlagShiftType);
    ht.set_flag_bit(format::lsmt::kFlagShiftHeader);
    ht.virtual_size = 512 * 64;
    ht.uuid = "00000000-0000-0000-0000-000000000049";
    ht.serialize(invalid_index.data());
    ht.clr_flag_bit(format::lsmt::kFlagShiftHeader);
    ht.index_offset = format::lsmt::kSpace;
    ht.index_size = 1;
    ht.serialize(invalid_index.data() + 2 * format::lsmt::kSpace);
    bytes::segment_mapping mapping;
    mapping.offset = 0;
    mapping.length = 1;
    mapping.moffset = 0;  // inside the header, never a valid data mapping
    bytes::store_segment_le(invalid_index.data() + format::lsmt::kSpace, mapping);
    const std::string index_path = test::write_file(dir / "index.rw", invalid_index);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        for (const auto& path : {short_path, corrupt_path, index_path}) {
            const BackingInode backing(path);
            const std::string before = file_sha256(path);
            for (int i = 0; i < 4; ++i) {
                const int seal_rc = co_await format::LsmtRwLayer::seal_file(
                    path, "", nullptr, nullptr);
                REQUIRE(seal_rc == -EINVAL);
                REQUIRE(backing.descriptors() == 0);
                REQUIRE(file_sha256(path) == before);
            }
        }
        // /dev/full accepts open but deterministically fails the header write.
        // A separate live layer must remain usable through exception cleanup.
        auto live = co_await format::LsmtRwLayer::create(dir / "live.rw", 512 * 64);
        const BackingInode live_inode(dir / "live.rw");
        const BackingInode full("/dev/full");
        const size_t full_before = full.descriptors();
        for (int i = 0; i < 4; ++i) {
            bool threw = false;
            try {
                auto failed = co_await format::LsmtRwLayer::create("/dev/full", 512 * 64);
            } catch (const std::system_error& e) {
                REQUIRE(e.code().value() == ENOSPC);
                threw = true;
            }
            REQUIRE(threw);
            REQUIRE(full.descriptors() == full_before);
            REQUIRE(live_inode.descriptors() == 1);
            std::vector<uint8_t> header(4096);
            const ssize_t got = co_await live->data_source().pread(
                header.data(), header.size(), 0);
            REQUIRE(got == static_cast<ssize_t>(header.size()));
        }
        live.reset();
        co_return 0;
    });
    REQUIRE(rc == 0);
}
