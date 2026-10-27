#include "format/hybrid_writer.hpp"
#include "format/hybrid_log_state.hpp"
#include "common/crc32c.hpp"
#include "common/errors.hpp"
#include "../support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <fcntl.h>
#include <sys/xattr.h>
#include <unistd.h>

using namespace obd;
namespace {
std::vector<uint8_t> read_bytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void write_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    REQUIRE(file.good());
}
std::array<uint8_t, 128> attribute(const std::string& path) {
    std::array<uint8_t, 128> value{};
    REQUIRE(::getxattr(path.c_str(), format::hybrid::kLogStateAttribute,
                       value.data(), value.size()) == static_cast<ssize_t>(value.size()));
    return value;
}
int try_open(const std::string& directory, bool writable = true) {
    return test::run_coro([&]() -> elio::coro::task<int> {
        try { (void)co_await format::hybrid::FilePair::open_existing(
            directory + "/data", directory + "/index", writable); }
        catch (const error& e) { co_return e.errno_value(); }
        co_return 0;
    });
}
}

TEST_CASE("format: hybrid committed log rejects stale or altered same UUID index",
          "[format][hybrid]") {
    for (const bool old_copy : {true, false}) {
        DYNAMIC_SECTION("old_copy=" << old_copy) {
            test::TempDir temporary;
            const auto directory = temporary / "pair";
            std::vector<uint8_t> stale;
            const int rc = test::run_coro([&]() -> elio::coro::task<int> {
                auto pair = co_await format::hybrid::FilePair::create(directory, 32768);
                format::hybrid::Writer writer(std::move(pair));
                std::vector<uint8_t> bytes(512, 0x41);
                const auto first = co_await writer.pwrite(bytes.data(), bytes.size(), 512);
                REQUIRE(first == 512);
                const int flush1 = co_await writer.flush();
                REQUIRE(flush1 == 0);
                stale = read_bytes(directory + "/index");
                const int zero = co_await writer.discard(512, 512);
                REQUIRE(zero == 0);
                const auto second = co_await writer.pwrite(bytes.data(), bytes.size(), 1024);
                REQUIRE(second == 512);
                const int flush2 = co_await writer.flush();
                REQUIRE(flush2 == 0);
                co_return 0;
            });
            REQUIRE(rc == 0);
            const auto current = read_bytes(directory + "/index");
            const auto data = read_bytes(directory + "/data");
            auto damaged = old_copy ? stale : current;
            if (!old_copy) damaged.back() ^= 1; // A structurally valid tag change.
            write_bytes(directory + "/index", damaged);
            REQUIRE(try_open(directory, false) == EINVAL);
            REQUIRE(try_open(directory) == EINVAL);
            REQUIRE(read_bytes(directory + "/index") == damaged);
            REQUIRE(read_bytes(directory + "/data") == data);
            write_bytes(directory + "/index", current);
            REQUIRE(try_open(directory) == 0);
        }
    }
}

TEST_CASE("format: hybrid compacted log rejects an earlier index generation",
          "[format][hybrid]") {
    test::TempDir temporary;
    const auto directory = temporary / "pair";
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto pair = co_await format::hybrid::FilePair::create(directory, 32768);
        format::hybrid::Writer writer(std::move(pair));
        std::vector<uint8_t> payload(512, 0x31);
        const auto first = co_await writer.pwrite(payload.data(), payload.size(), 512);
        REQUIRE(first == 512);
        const int zero = co_await writer.discard(512, 512);
        REQUIRE(zero == 0);
        const int flushed = co_await writer.flush();
        REQUIRE(flushed == 0);
        std::filesystem::copy_file(directory + "/index", directory + "/old-index");
        const auto old_progress = attribute(directory + "/index");
        REQUIRE(::setxattr((directory + "/old-index").c_str(), format::hybrid::kLogStateAttribute,
                          old_progress.data(), old_progress.size(), 0) == 0);
        const auto rewrite = co_await writer.pwrite(payload.data(), payload.size(), 512);
        REQUIRE(rewrite == 512);
        const int compacted = co_await writer.compact();
        REQUIRE(compacted == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
    std::filesystem::rename(directory + "/index", directory + "/current-index");
    std::filesystem::rename(directory + "/old-index", directory + "/index");
    REQUIRE(try_open(directory) == EINVAL);
    std::filesystem::rename(directory + "/index", directory + "/rejected-index");
    std::filesystem::rename(directory + "/current-index", directory + "/index");
    REQUIRE(try_open(directory) == 0);
}

TEST_CASE("format: hybrid progress recovers completed appends and discards unconfirmed tails",
          "[format][hybrid]") {
    for (const bool progress_ahead : {false, true}) {
        DYNAMIC_SECTION("progress_ahead=" << progress_ahead) {
            test::TempDir temporary;
            const auto directory = temporary / "pair";
            const int rc = test::run_coro([&]() -> elio::coro::task<int> {
                auto pair = co_await format::hybrid::FilePair::create(directory, 32768);
                format::hybrid::Writer writer(std::move(pair));
                std::vector<uint8_t> payload(512, 0x71);
                const auto first = co_await writer.pwrite(payload.data(), payload.size(), 512);
                REQUIRE(first == 512);
                const int flush = co_await writer.flush();
                REQUIRE(flush == 0);
                if (!progress_ahead) {
                    const auto second = co_await writer.pwrite(payload.data(), payload.size(), 1024);
                    REQUIRE(second == 512); // Completed append; deliberately no FLUSH.
                }
                co_return 0;
            });
            REQUIRE(rc == 0);
            const auto index_before = read_bytes(directory + "/index");
            std::array<uint8_t, 16> next{};
            bytes::store_segment_le(next.data(), bytes::segment_mapping{4, 1, 8, true, 0});
            if (progress_ahead) {
                const int data = ::open((directory + "/data").c_str(), O_RDONLY | O_CLOEXEC);
                const int index = ::open((directory + "/index").c_str(), O_RDWR | O_CLOEXEC);
                REQUIRE(data >= 0);
                REQUIRE(index >= 0);
                auto stamp = format::hybrid::inspect_log_state(data, index).value();
                stamp.bytes += next.size();
                stamp.crc = crc32::crc32c_extend(next.data(), next.size(), stamp.crc);
                format::hybrid::publish_log_progress(index, stamp);
                ::close(index);
                ::close(data);
            } else {
                auto tail = index_before;
                tail.insert(tail.end(), next.begin(), next.end());
                write_bytes(directory + "/index", tail); // Record arrived, marker did not.
            }
            const auto size_before_read = std::filesystem::file_size(directory + "/index");
            REQUIRE(try_open(directory, false) == 0);
            REQUIRE(std::filesystem::file_size(directory + "/index") == size_before_read);
            const int reopened = test::run_coro([&]() -> elio::coro::task<int> {
                auto pair = co_await format::hybrid::FilePair::open_existing(
                    directory + "/data", directory + "/index", true);
                format::hybrid::Writer writer(std::move(pair));
                std::vector<uint8_t> actual(5 * 512), expected(5 * 512, 0);
                std::fill_n(expected.begin() + 512, progress_ahead ? 512 : 1024, 0x71);
                const auto read = co_await writer.pread(actual.data(), actual.size(), 0);
                REQUIRE(read == static_cast<ssize_t>(actual.size()));
                REQUIRE(actual == expected);
                co_return 0;
            });
            REQUIRE(reopened == 0);
            REQUIRE(read_bytes(directory + "/index") == index_before);
        }
    }
}

TEST_CASE("format: hybrid integrity metadata cannot silently disappear or corrupt",
          "[format][hybrid]") {
    test::TempDir temporary;
    const auto directory = temporary / "pair";
    const int created = test::run_coro([&]() -> elio::coro::task<int> {
        auto pair = co_await format::hybrid::FilePair::create(directory, 32768);
        co_return 0;
    });
    REQUIRE(created == 0);
    const auto anchor = attribute(directory + "/data");
    const auto progress = attribute(directory + "/index");
    REQUIRE(::removexattr((directory + "/index").c_str(), format::hybrid::kLogStateAttribute) == 0);
    REQUIRE(try_open(directory) == EINVAL);
    REQUIRE(::setxattr((directory + "/index").c_str(), format::hybrid::kLogStateAttribute,
                      progress.data(), progress.size(), 0) == 0);
    auto corrupt = anchor;
    corrupt[48] ^= 1;
    REQUIRE(::setxattr((directory + "/data").c_str(), format::hybrid::kLogStateAttribute,
                      corrupt.data(), corrupt.size(), 0) == 0);
    REQUIRE(try_open(directory) == EINVAL);
    REQUIRE(::setxattr((directory + "/data").c_str(), format::hybrid::kLogStateAttribute,
                      anchor.data(), anchor.size(), 0) == 0);
    REQUIRE(try_open(directory) == 0);
}

namespace {
struct ResetLogFault {
    ~ResetLogFault() { format::hybrid::log_state_fault_for_test(-1); }
};
}

TEST_CASE("format: hybrid interrupted integrity adoption preserves upstream bytes",
          "[format][hybrid]") {
    for (const auto producer : {"overlaybd", "agentenv"}) {
        for (const int stage : {1, 2, 3}) {
            DYNAMIC_SECTION(producer << " stage=" << stage) {
                ResetLogFault reset;
                test::TempDir temporary;
                const auto directory = temporary / "pair";
                std::filesystem::create_directory(directory);
                const auto fixtures = std::filesystem::path(__FILE__).parent_path().parent_path() /
                                      "fixtures/hybrid" / producer;
                for (const auto part : {"data", "index"})
                    std::filesystem::copy_file(fixtures / (std::string("writable.") + part),
                                               directory + "/" + part);
                const auto data = read_bytes(directory + "/data");
                const auto index = read_bytes(directory + "/index");
                format::hybrid::log_state_fault_for_test(stage);
                REQUIRE(try_open(directory) == EIO);
                format::hybrid::log_state_fault_for_test(-1);
                REQUIRE(try_open(directory, false) == 0);
                REQUIRE(read_bytes(directory + "/data") == data);
                REQUIRE(read_bytes(directory + "/index") == index);
                REQUIRE(try_open(directory) == 0);
                REQUIRE(attribute(directory + "/data") == attribute(directory + "/index"));
                REQUIRE(read_bytes(directory + "/data") == data);
                REQUIRE(read_bytes(directory + "/index") == index);
            }
        }
    }
}

TEST_CASE("format: hybrid failed progress publication requires reopen",
          "[format][hybrid]") {
    ResetLogFault reset;
    test::TempDir temporary;
    const auto directory = temporary / "pair";
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto pair = co_await format::hybrid::FilePair::create(directory, 32768);
        auto writer = std::make_unique<format::hybrid::Writer>(std::move(pair));
        std::vector<uint8_t> payload(512, 0x71), actual(512);
        format::hybrid::log_state_fault_for_test(2);
        const auto failed = co_await writer->pwrite(payload.data(), payload.size(), 512);
        REQUIRE(failed == -EIO);
        format::hybrid::log_state_fault_for_test(-1);
        const auto refused = co_await writer->pwrite(payload.data(), payload.size(), 1024);
        REQUIRE(refused == -EIO);
        writer.reset();
        pair = co_await format::hybrid::FilePair::open_existing(
            directory + "/data", directory + "/index", true);
        writer = std::make_unique<format::hybrid::Writer>(std::move(pair));
        // The failed operation reached complete record+progress publication;
        // process recovery may expose it, but must not expose a partial mapping.
        const auto read = co_await writer->pread(actual.data(), actual.size(), 512);
        REQUIRE(read == 512);
        REQUIRE(actual == payload);
        const auto resumed = co_await writer->pwrite(payload.data(), payload.size(), 1024);
        REQUIRE(resumed == 512);
        const int flushed = co_await writer->flush();
        REQUIRE(flushed == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: hybrid checkpoint anchor failure remains retryable",
          "[format][hybrid]") {
    ResetLogFault reset;
    test::TempDir temporary;
    const auto directory = temporary / "pair";
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto pair = co_await format::hybrid::FilePair::create(directory, 32768);
        auto writer = std::make_unique<format::hybrid::Writer>(std::move(pair));
        std::vector<uint8_t> payload(512, 0x42), actual(1024);
        const auto first = co_await writer->pwrite(payload.data(), payload.size(), 512);
        REQUIRE(first == 512);
        format::hybrid::log_state_fault_for_test(3);
        const int failed = co_await writer->checkpoint();
        REQUIRE(failed == -EIO);
        format::hybrid::log_state_fault_for_test(-1);
        const auto next = co_await writer->pwrite(payload.data(), payload.size(), 1024);
        REQUIRE(next == 512);
        const int retried = co_await writer->checkpoint();
        REQUIRE(retried == 0);
        const auto terminal = co_await writer->pwrite(payload.data(), payload.size(), 1536);
        REQUIRE(terminal == -EROFS);
        writer.reset();
        pair = co_await format::hybrid::FilePair::open_existing(
            directory + "/data", directory + "/index", true);
        writer = std::make_unique<format::hybrid::Writer>(std::move(pair));
        const auto read = co_await writer->pread(actual.data(), actual.size(), 512);
        REQUIRE(read == 1024);
        REQUIRE(actual == std::vector<uint8_t>(1024, 0x42));
        co_return 0;
    });
    REQUIRE(rc == 0);
}
