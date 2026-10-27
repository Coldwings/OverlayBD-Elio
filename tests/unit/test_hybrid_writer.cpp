#include "format/hybrid_writer.hpp"
#include "format/hybrid_io_fault.hpp"
#include "format/hybrid_log_state.hpp"
#include <sys/xattr.h>
#include "common/errors.hpp"
#include "../support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <elio/runtime/spawn_blocking.hpp>

using namespace obd;
namespace {
std::vector<uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}
TEST_CASE("format: hybrid imported ZERO rewrites preserve other data and reuse backing",
          "[format][hybrid]") {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      "fixtures/hybrid";
    for (const auto* producer : {"overlaybd", "agentenv"}) {
        DYNAMIC_SECTION(producer) {
            test::TempDir temporary;
            const auto data = temporary / "upper.data", index = temporary / "upper.index";
            std::filesystem::copy_file(root / producer / "writable.data", data);
            std::filesystem::copy_file(root / producer / "writable.index", index);
            auto expected = read_bytes(root / producer / "expected.raw");
            const auto initial_size = std::filesystem::file_size(data);
            uint64_t bounded_data = 0, index_after = 0;
            const int rc = test::run_coro([&]() -> elio::coro::task<int> {
                auto pair = co_await format::hybrid::FilePair::open_existing(data, index, true);
                auto writer = std::make_unique<format::hybrid::Writer>(std::move(pair));
                std::vector<uint8_t> bytes(512, 0x55);
                const auto write = co_await writer->pwrite(bytes.data(), bytes.size(), 10 * 512);
                REQUIRE(write == 512);
                std::fill_n(expected.begin() + 10 * 512, 512, 0x55);
                std::vector<uint8_t> actual(32768);
                const auto read = co_await writer->pread(actual.data(), actual.size(), 0);
                REQUIRE(read == 32768);
                REQUIRE(actual == expected); // In particular sector63 must remain0x77.
                for (int iteration = 0; iteration < 20; ++iteration) {
                    const int discard = co_await writer->discard(16 * 512, 2 * 512);
                    REQUIRE(discard == 0);
                    std::vector<uint8_t> rewritten(1024, static_cast<uint8_t>(iteration + 1));
                    const auto result = co_await writer->pwrite(rewritten.data(), rewritten.size(), 16 * 512);
                    REQUIRE(result == 1024);
                    std::copy(rewritten.begin(), rewritten.end(), expected.begin() + 16 * 512);
                }
                const int flushed = co_await writer->flush();
                REQUIRE(flushed == 0);
                writer.reset();
                co_return 0;
            });
            REQUIRE(rc == 0);
            bounded_data = std::filesystem::file_size(data);
            REQUIRE(bounded_data <= initial_size + 3 * 512);
            index_after = std::filesystem::file_size(index);
            const int reopened = test::run_coro([&]() -> elio::coro::task<int> {
                auto pair = co_await format::hybrid::FilePair::open_existing(data, index, true);
                format::hybrid::Writer writer(std::move(pair));
                std::vector<uint8_t> actual(32768);
                const auto read = co_await writer.pread(actual.data(), actual.size(), 0);
                REQUIRE(read == 32768);
                REQUIRE(actual == expected);
                std::vector<uint8_t> rewrite(512, 0x66);
                for (int i = 0; i < 20; ++i) {
                    const auto write = co_await writer.pwrite(rewrite.data(), rewrite.size(), 9 * 512);
                    REQUIRE(write == 512);
                }
                const int flushed = co_await writer.flush();
                REQUIRE(flushed == 0);
                co_return 0;
            });
            REQUIRE(reopened == 0);
            REQUIRE(std::filesystem::file_size(data) == bounded_data);
            REQUIRE(std::filesystem::file_size(index) == index_after);
        }
    }
}

TEST_CASE("format: hybrid new pair publishes complete headers and recovers mixed writes",
          "[format][hybrid]") {
    test::TempDir temporary;
    const auto directory = temporary / "new-upper";
    std::vector<uint8_t> expected(32768, 0);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto pair = co_await format::hybrid::FilePair::create(directory, 32768);
        REQUIRE(pair->mappings().empty());
        REQUIRE(pair->data_bytes() == 4096);
        REQUIRE(pair->index_bytes() == 4096);
        format::hybrid::Writer writer(std::move(pair));
        std::vector<uint8_t> first(1024, 0x11), overlap(1024, 0x44);
        const auto a = co_await writer.pwrite(first.data(), first.size(), 512);
        REQUIRE(a == 1024);
        const auto b = co_await writer.pwrite(overlap.data(), overlap.size(), 1024);
        REQUIRE(b == 1024);
        const int discard = co_await writer.discard(0, 3 * 512);
        REQUIRE(discard == 0);
        std::fill_n(expected.begin() + 3 * 512, 512, 0x44);
        const int flushed = co_await writer.flush();
        REQUIRE(flushed == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
    const int reopened = test::run_coro([&]() -> elio::coro::task<int> {
        auto pair = co_await format::hybrid::FilePair::open_existing(
            directory + "/data", directory + "/index", true);
        format::hybrid::Writer writer(std::move(pair));
        std::vector<uint8_t> actual(expected.size());
        const auto read = co_await writer.pread(actual.data(), actual.size(), 0);
        REQUIRE(read == static_cast<ssize_t>(actual.size()));
        REQUIRE(actual == expected);
        int refusal = 0;
        try {
            auto duplicate = co_await format::hybrid::FilePair::create(directory, 65536);
            (void)duplicate;
        } catch (const obd::error& e) { refusal = e.errno_value(); }
        REQUIRE(refusal == EEXIST);
        co_return 0;
    });
    REQUIRE(reopened == 0);
    REQUIRE(std::distance(std::filesystem::directory_iterator(temporary.path()),
                          std::filesystem::directory_iterator()) == 1);
}

TEST_CASE("format: hybrid checkpoint is terminal only for its writer instance",
          "[format][hybrid]") {
    test::TempDir temporary;
    const auto directory = temporary / "pair";
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto pair = co_await format::hybrid::FilePair::create(directory, 32768);
        auto writer = std::make_unique<format::hybrid::Writer>(std::move(pair));
        std::vector<uint8_t> payload(1024, 0x39), actual(1024);
        const auto written = co_await writer->pwrite(payload.data(), payload.size(), 512);
        REQUIRE(written == 1024);
        const int discarded = co_await writer->discard(1024, 512);
        REQUIRE(discarded == 0);
        std::fill(payload.begin() + 512, payload.end(), 0);
        const int checkpoint = co_await writer->checkpoint();
        REQUIRE(checkpoint == 0);
        const auto before_data = read_bytes(directory + "/data");
        const auto before_index = read_bytes(directory + "/index");
        const auto refused_write = co_await writer->pwrite(payload.data(), payload.size(), 512);
        REQUIRE(refused_write == -EROFS);
        const int refused_discard = co_await writer->discard(512, 1024);
        REQUIRE(refused_discard == -EROFS);
        auto* active = writer.get();
        const int refused_grow = co_await elio::spawn_blocking([active] { return active->grow(65536); });
        REQUIRE(refused_grow == -EROFS);
        const int refused_compact = co_await writer->compact();
        REQUIRE(refused_compact == -EROFS);
        const int refused_seal = co_await writer->seal();
        REQUIRE(refused_seal == -EROFS);
        const int refused_checkpoint = co_await writer->checkpoint();
        REQUIRE(refused_checkpoint == -EROFS);
        const int flushed = co_await writer->flush();
        REQUIRE(flushed == 0);
        const auto read = co_await writer->pread(actual.data(), actual.size(), 512);
        REQUIRE(read == 1024);
        REQUIRE(actual == payload);
        REQUIRE(read_bytes(directory + "/data") == before_data);
        REQUIRE(read_bytes(directory + "/index") == before_index);
        writer.reset();
        pair = co_await format::hybrid::FilePair::open_existing(
            directory + "/data", directory + "/index", true);
        writer = std::make_unique<format::hybrid::Writer>(std::move(pair));
        const auto recovered = co_await writer->pread(actual.data(), actual.size(), 512);
        REQUIRE(recovered == 1024);
        REQUIRE(actual == payload);
        const auto resumed = co_await writer->pwrite(payload.data(), payload.size(), 2048);
        REQUIRE(resumed == 1024);
        const int sealed = co_await writer->seal();
        REQUIRE(sealed == 0);
        const int sealed_checkpoint = co_await writer->checkpoint();
        REQUIRE(sealed_checkpoint == -EROFS);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

namespace {
struct ResetIoFault {
    ~ResetIoFault() { format::hybrid::writer_io_fault_for_test(-1); }
};
}

TEST_CASE("format: hybrid interrupted append never exposes an unpublished mapping",
          "[format][hybrid]") {
    for (const int stage : {1, 2, 3, 4, 5}) {
        DYNAMIC_SECTION("stage=" << stage) {
            ResetIoFault reset;
            test::TempDir temporary;
            const auto directory = temporary / "pair";
            const int rc = test::run_coro([&]() -> elio::coro::task<int> {
                auto pair = co_await format::hybrid::FilePair::create(directory, 32768);
                auto writer = std::make_unique<format::hybrid::Writer>(std::move(pair));
                std::vector<uint8_t> payload(1024, 0x31), actual(8192);
                const auto first = co_await writer->pwrite(payload.data(), payload.size(), 0);
                REQUIRE(first == 1024);
                const int flush = co_await writer->flush();
                REQUIRE(flush == 0);
                const auto committed_index = read_bytes(directory + "/index");
                format::hybrid::writer_io_fault_for_test(stage);
                const auto failed = co_await writer->pwrite(payload.data(), payload.size(), 4096);
                REQUIRE(failed == -EIO);
                format::hybrid::writer_io_fault_for_test(-1);
                const auto poisoned_read = co_await writer->pread(actual.data(), actual.size(), 0);
                REQUIRE(poisoned_read == -EIO);
                const int poisoned_flush = co_await writer->flush();
                REQUIRE(poisoned_flush == -EIO);
                writer.reset();
                if (stage == 4) {
                    const auto torn = read_bytes(directory + "/index");
                    REQUIRE(torn.size() == committed_index.size() + 8);
                    for (const bool writable : {false, true}) {
                        int error_code = 0;
                        try {
                            (void)co_await format::hybrid::FilePair::open_existing(
                                directory + "/data", directory + "/index", writable);
                        } catch (const error& e) { error_code = e.errno_value(); }
                        REQUIRE(error_code == EINVAL);
                        REQUIRE(read_bytes(directory + "/index") == torn);
                    }
                    co_return 0;
                }
                pair = co_await format::hybrid::FilePair::open_existing(
                    directory + "/data", directory + "/index", true);
                writer = std::make_unique<format::hybrid::Writer>(std::move(pair));
                const auto read = co_await writer->pread(actual.data(), actual.size(), 0);
                REQUIRE(read == static_cast<ssize_t>(actual.size()));
                std::vector<uint8_t> expected(actual.size(), 0);
                std::copy(payload.begin(), payload.end(), expected.begin());
                REQUIRE(actual == expected);
                REQUIRE(read_bytes(directory + "/index") == committed_index);
                const auto resumed = co_await writer->pwrite(payload.data(), payload.size(), 4096);
                REQUIRE(resumed == 1024);
                const int resumed_flush = co_await writer->flush();
                REQUIRE(resumed_flush == 0);
                co_return 0;
            });
            REQUIRE(rc == 0);
        }
    }
}

TEST_CASE("format: hybrid interrupted in place write does not promise rollback",
          "[format][hybrid]") {
    ResetIoFault reset;
    test::TempDir temporary;
    const auto directory = temporary / "pair";
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto pair = co_await format::hybrid::FilePair::create(directory, 32768);
        auto writer = std::make_unique<format::hybrid::Writer>(std::move(pair));
        std::vector<uint8_t> original(1024, 0x11), replacement(1024, 0x22), actual(1024);
        const auto first = co_await writer->pwrite(original.data(), original.size(), 0);
        REQUIRE(first == 1024);
        const int flushed = co_await writer->flush();
        REQUIRE(flushed == 0);
        const auto index = read_bytes(directory + "/index");
        format::hybrid::writer_io_fault_for_test(1);
        const auto failed = co_await writer->pwrite(replacement.data(), replacement.size(), 0);
        REQUIRE(failed == -EIO);
        format::hybrid::writer_io_fault_for_test(-1);
        writer.reset();
        pair = co_await format::hybrid::FilePair::open_existing(
            directory + "/data", directory + "/index", true);
        writer = std::make_unique<format::hybrid::Writer>(std::move(pair));
        const auto read = co_await writer->pread(actual.data(), actual.size(), 0);
        REQUIRE(read == 1024);
        std::fill_n(original.begin(), 512, 0x22);
        REQUIRE(actual == original);
        REQUIRE(read_bytes(directory + "/index") == index);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: hybrid checkpoint retries each failed sync boundary",
          "[format][hybrid]") {
    for (const int stage : {6, 7, 8}) {
        DYNAMIC_SECTION("stage=" << stage) {
            ResetIoFault reset;
            test::TempDir temporary;
            const auto directory = temporary / "pair";
            const int rc = test::run_coro([&]() -> elio::coro::task<int> {
                auto pair = co_await format::hybrid::FilePair::create(directory, 32768);
                auto writer = std::make_unique<format::hybrid::Writer>(std::move(pair));
                std::vector<uint8_t> payload(512, 0x68), actual(1024);
                const auto first = co_await writer->pwrite(payload.data(), payload.size(), 0);
                REQUIRE(first == 512);
                std::array<uint8_t, 128> anchor_before{}, anchor_after{};
                REQUIRE(::getxattr((directory + "/data").c_str(), format::hybrid::kLogStateAttribute,
                    anchor_before.data(), anchor_before.size()) == static_cast<ssize_t>(anchor_before.size()));
                format::hybrid::writer_io_fault_for_test(stage);
                const int failed = co_await writer->checkpoint();
                REQUIRE(failed == -EIO);
                REQUIRE(::getxattr((directory + "/data").c_str(), format::hybrid::kLogStateAttribute,
                    anchor_after.data(), anchor_after.size()) == static_cast<ssize_t>(anchor_after.size()));
                REQUIRE(anchor_after == anchor_before);
                format::hybrid::writer_io_fault_for_test(-1);
                const auto next = co_await writer->pwrite(payload.data(), payload.size(), 512);
                REQUIRE(next == 512);
                const int retried = co_await writer->checkpoint();
                REQUIRE(retried == 0);
                writer.reset();
                pair = co_await format::hybrid::FilePair::open_existing(
                    directory + "/data", directory + "/index", true);
                writer = std::make_unique<format::hybrid::Writer>(std::move(pair));
                const auto read = co_await writer->pread(actual.data(), actual.size(), 0);
                REQUIRE(read == 1024);
                REQUIRE(actual == std::vector<uint8_t>(1024, 0x68));
                co_return 0;
            });
            REQUIRE(rc == 0);
        }
    }
}
