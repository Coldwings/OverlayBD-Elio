#include "format/hybrid_writer.hpp"
#include "../support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

namespace obd::format::hybrid {
struct FilePairTestAccess {
    static void fail_replace_at(int stage) { FilePair::replace_fail_stage_.store(stage); }
    static int exchange_directory(FilePair& pair, int fd) {
        return std::exchange(pair.index_directory_fd_, fd);
    }
};
}
using namespace obd;
namespace {
std::vector<uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
struct ResetFault {
    ~ResetFault() { format::hybrid::FilePairTestAccess::fail_replace_at(-1); }
};
}
TEST_CASE("format: hybrid compaction keeps the published index across rename and sync failures",
          "[format][hybrid]") {
    const auto fixtures = std::filesystem::path(__FILE__).parent_path().parent_path() /
                          "fixtures/hybrid";
    for (const auto* producer : {"overlaybd", "agentenv"}) {
        for (const int stage : {-1, 1, 2, 3, 4}) {
            DYNAMIC_SECTION(producer << " stage=" << stage) {
                ResetFault reset;
                test::TempDir temporary;
                const auto data = temporary / "data", index = temporary / "index";
                std::filesystem::copy_file(fixtures / producer / "writable.data", data);
                std::filesystem::copy_file(fixtures / producer / "writable.index", index);
                auto expected = read_bytes(fixtures / producer / "expected.raw");
                const auto initial_data_size = std::filesystem::file_size(data);
                const int rc = test::run_coro([&]() -> elio::coro::task<int> {
                    auto pair = co_await format::hybrid::FilePair::open_existing(data, index, true);
                    format::hybrid::Writer writer(std::move(pair));
                    format::hybrid::FilePairTestAccess::fail_replace_at(stage);
                    int compacted = 0;
                    bool allocation_failure = false;
                    try { compacted = co_await writer.compact(); }
                    catch (const std::bad_alloc&) { allocation_failure = true; }
                    REQUIRE(allocation_failure == (stage == 4));
                    if (stage != 4) REQUIRE(compacted == (stage == -1 ? 0 : -EIO));
                    format::hybrid::FilePairTestAccess::fail_replace_at(-1);
                    const auto mappings = writer.snapshot();
                    if (stage != 1) {
                        for (const auto& m : mappings) {
                            REQUIRE(m.moffset != format::hybrid::kNoPhysicalOffset);
                        }
                        REQUIRE(std::filesystem::file_size(index) == 4096 + mappings.size() * 16);
                    }
                    std::vector<uint8_t> bytes(512, 0x5a);
                    const auto wrote = co_await writer.pwrite(bytes.data(), bytes.size(), 0);
                    REQUIRE(wrote == 512);
                    std::copy(bytes.begin(), bytes.end(), expected.begin());
                    const int flushed = co_await writer.flush();
                    REQUIRE(flushed == 0);
                    co_return 0;
                });
                REQUIRE(rc == 0);
                REQUIRE(std::filesystem::file_size(data) <= initial_data_size + 4 * 512);
                REQUIRE(std::distance(std::filesystem::directory_iterator(temporary.path()),
                                      std::filesystem::directory_iterator()) == 2);
                const int reopened = test::run_coro([&]() -> elio::coro::task<int> {
                    auto pair = co_await format::hybrid::FilePair::open_existing(data, index, true);
                    format::hybrid::Writer writer(std::move(pair));
                    std::vector<uint8_t> actual(expected.size());
                    const auto read = co_await writer.pread(actual.data(), actual.size(), 0);
                    REQUIRE(read == static_cast<ssize_t>(actual.size()));
                    REQUIRE(actual == expected);
                    co_return 0;
                });
                REQUIRE(reopened == 0);
            }
        }
    }
}

TEST_CASE("format: hybrid automatically compacts padded history before appending",
          "[format][hybrid]") {
    const auto fixtures = std::filesystem::path(__FILE__).parent_path().parent_path() /
                          "fixtures/hybrid/overlaybd";
    test::TempDir temporary;
    const auto data = temporary / "data", index = temporary / "index";
    std::filesystem::copy_file(fixtures / "writable.data", data);
    std::filesystem::copy_file(fixtures / "writable.index", index);
    {
        std::ofstream append(index, std::ios::binary | std::ios::app);
        std::array<uint8_t, 16> padding{};
        bytes::store_segment_le(padding.data(),
            {bytes::segment_mapping::kInvalidOffset, 0, 0, false, 0});
        for (size_t i = 0; i < 4096; ++i)
            append.write(reinterpret_cast<const char*>(padding.data()), padding.size());
        REQUIRE(append.good());
    }
    auto expected = read_bytes(fixtures / "expected.raw");
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto pair = co_await format::hybrid::FilePair::open_existing(data, index, true);
        format::hybrid::Writer writer(std::move(pair));
        std::vector<uint8_t> bytes(512, 0x5a);
        const auto write = co_await writer.pwrite(bytes.data(), bytes.size(), 0);
        REQUIRE(write == 512);
        std::copy(bytes.begin(), bytes.end(), expected.begin());
        const int flushed = co_await writer.flush();
        REQUIRE(flushed == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(std::filesystem::file_size(index) == 4096 + 7 * 16);
    const int reopened = test::run_coro([&]() -> elio::coro::task<int> {
        auto pair = co_await format::hybrid::FilePair::open_existing(data, index, true);
        format::hybrid::Writer writer(std::move(pair));
        std::vector<uint8_t> actual(expected.size());
        const auto read = co_await writer.pread(actual.data(), actual.size(), 0);
        REQUIRE(read == static_cast<ssize_t>(actual.size()));
        REQUIRE(actual == expected);
        co_return 0;
    });
    REQUIRE(reopened == 0);
}

TEST_CASE("format: hybrid checkpoint retries a failed directory durability barrier",
          "[format][hybrid]") {
    test::TempDir temporary;
    const auto directory = temporary / "pair";
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto pair = co_await format::hybrid::FilePair::create(directory, 32768);
        auto* owned = pair.get();
        format::hybrid::Writer writer(std::move(pair));
        std::vector<uint8_t> payload(512, 0x64);
        const auto first = co_await writer.pwrite(payload.data(), payload.size(), 512);
        REQUIRE(first == 512);
        {
            // Keep the actual descriptor open; only this owned pair sees EBADF.
            struct RestoreDirectory {
                format::hybrid::FilePair& pair;
                int fd;
                ~RestoreDirectory() {
                    format::hybrid::FilePairTestAccess::exchange_directory(pair, fd);
                }
            } restore{*owned, format::hybrid::FilePairTestAccess::exchange_directory(*owned, -1)};
            const int checkpoint = co_await writer.checkpoint();
            REQUIRE(checkpoint == -EBADF);
        }
        const auto second = co_await writer.pwrite(payload.data(), payload.size(), 1024);
        REQUIRE(second == 512);
        const int retried = co_await writer.checkpoint();
        REQUIRE(retried == 0);
        const auto refused = co_await writer.pwrite(payload.data(), payload.size(), 1536);
        REQUIRE(refused == -EROFS);
        co_return 0;
    });
    REQUIRE(rc == 0);
    const int reopened = test::run_coro([&]() -> elio::coro::task<int> {
        auto pair = co_await format::hybrid::FilePair::open_existing(
            directory + "/data", directory + "/index", true);
        format::hybrid::Writer writer(std::move(pair));
        std::vector<uint8_t> actual(1536), expected(1536, 0);
        std::fill_n(expected.begin(), 1024, 0x64);
        const auto read = co_await writer.pread(actual.data(), actual.size(), 512);
        REQUIRE(read == 1536);
        REQUIRE(actual == expected);
        co_return 0;
    });
    REQUIRE(reopened == 0);
}
