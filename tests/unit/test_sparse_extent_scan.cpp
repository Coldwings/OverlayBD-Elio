#include "format/sparse_extent_scan.hpp"
#include "common/errors.hpp"
#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <algorithm>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
struct File {
    int fd;
    ~File() { if (fd >= 0) ::close(fd); }
};
const obd::bytes::segment_mapping* at(
    const std::vector<obd::bytes::segment_mapping>& segments, uint64_t sector) {
    for (const auto& s : segments)
        if (s.offset <= sector && sector < s.end()) return &s;
    return nullptr;
}
}

TEST_CASE("Sparse extent scan preserves unsynchronized data and holes", "[sparse-extents]") {
    obd::test::TempDir dir;
    File f{::open((dir.path() / "upper").c_str(), O_CREAT | O_EXCL | O_RDWR, 0600)};
    REQUIRE(f.fd >= 0);
    REQUIRE(::ftruncate(f.fd, 16384) == 0);
    REQUIRE(obd::format::scan_sparse_extents(f.fd, 16384).empty());
    std::array<unsigned char, 4096> payload;
    payload.fill(0x5b);
    REQUIRE(::pwrite(f.fd, payload.data(), payload.size(), 4096) == 4096);
    const auto segments = obd::format::scan_sparse_extents(f.fd, 16384);
    REQUIRE(at(segments, 8) != nullptr);
    REQUIRE_FALSE(at(segments, 8)->zeroed);
    REQUIRE(at(segments, 15) != nullptr);
    REQUIRE(at(segments, 0) == nullptr);
    REQUIRE(at(segments, 16) == nullptr);
    std::array<unsigned char, 4096> readback;
    REQUIRE(::pread(f.fd, readback.data(), readback.size(), 4096) == 4096);
    REQUIRE(readback == payload);
}

TEST_CASE("Sparse extent scan rejects invalid backing geometry", "[sparse-extents]") {
    obd::test::TempDir dir;
    File f{::open((dir.path() / "upper").c_str(), O_CREAT | O_EXCL | O_RDWR, 0600)};
    REQUIRE(f.fd >= 0);
    REQUIRE(::ftruncate(f.fd, 8192) == 0);
    REQUIRE_THROWS_AS(obd::format::scan_sparse_extents(f.fd, 0), obd::error);
    REQUIRE_THROWS_AS(obd::format::scan_sparse_extents(f.fd, 512), obd::error);
    REQUIRE_THROWS_AS(obd::format::scan_sparse_extents(f.fd, 4096), obd::error);
    REQUIRE_THROWS_AS(obd::format::scan_sparse_extents(f.fd, 12288), obd::error);
}

TEST_CASE("Sparse extent scan rejects inherited oversized allocation hints", "[sparse-extents]") {
    obd::test::TempDir dir;
    File directory{::open(dir.path().c_str(), O_RDONLY | O_DIRECTORY)};
    REQUIRE(directory.fd >= 0);
    struct fsxattr attr {};
    if (::ioctl(directory.fd, FS_IOC_FSGETXATTR, &attr) != 0)
        SKIP("Backing filesystem has no allocation hints");
    attr.fsx_xflags |= FS_XFLAG_EXTSZINHERIT;
    attr.fsx_extsize = 65536;
    if (::ioctl(directory.fd, FS_IOC_FSSETXATTR, &attr) != 0)
        SKIP("Backing filesystem cannot set directory allocation hints");
    File f{::open((dir.path() / "upper").c_str(), O_CREAT | O_EXCL | O_RDWR, 0600)};
    REQUIRE(f.fd >= 0);
    REQUIRE(::ioctl(f.fd, FS_IOC_FSGETXATTR, &attr) == 0);
    if (attr.fsx_extsize <= 4096)
        SKIP("Backing filesystem does not inherit oversized allocation hints");
    REQUIRE(::ftruncate(f.fd, 1048576) == 0);
    REQUIRE_THROWS_AS(obd::format::scan_sparse_extents(f.fd, 1048576), obd::error);
    // Rejection happens before allocating any payload or changing the policy.
    struct stat st {};
    REQUIRE(::fstat(f.fd, &st) == 0);
    REQUIRE(st.st_blocks == 0);
    REQUIRE(::ioctl(f.fd, FS_IOC_FSGETXATTR, &attr) == 0);
    REQUIRE(attr.fsx_extsize == 65536);
}

TEST_CASE("Sparse extent scan distinguishes cached zero-range writes from inherited holes",
          "[sparse-extents]") {
    obd::test::TempDir dir;
    File f{::open((dir.path() / "upper").c_str(), O_CREAT | O_EXCL | O_RDWR, 0600)};
    REQUIRE(f.fd >= 0);
    REQUIRE(::ftruncate(f.fd, 32768) == 0);
    if (::fallocate(f.fd, FALLOC_FL_ZERO_RANGE | FALLOC_FL_KEEP_SIZE, 8192, 16384) != 0) {
        if (errno == EOPNOTSUPP || errno == ENOSYS) SKIP("Backing filesystem has no ZERO_RANGE");
        FAIL("Unexpected ZERO_RANGE failure");
    }
    std::array<unsigned char, 512> payload;
    payload.fill(0x7c);
    REQUIRE(::pwrite(f.fd, payload.data(), payload.size(), 12288 + 512) == 512);
    // Recover before any pread: the scan must drain buffered DATA before
    // interpreting UNWRITTEN as ZERO.
    const auto segments = obd::format::scan_sparse_extents(f.fd, 32768);
    REQUIRE(at(segments, 25) != nullptr);
    REQUIRE_FALSE(at(segments, 25)->zeroed);
    REQUIRE(at(segments, 0) == nullptr);
    REQUIRE(at(segments, 48) == nullptr);
    std::array<unsigned char, 512> sector;
    for (uint64_t i = 16; i < 48; ++i) {
        const auto* s = at(segments, i);
        REQUIRE(s != nullptr);
        if (s->zeroed) sector.fill(0);
        else REQUIRE(::pread(f.fd, sector.data(), sector.size(), i * 512) == 512);
        const unsigned char expected = i == 25 ? 0x7c : 0;
        REQUIRE(std::all_of(sector.begin(), sector.end(), [expected](auto b) { return b == expected; }));
    }
}
