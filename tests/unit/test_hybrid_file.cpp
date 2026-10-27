#include "format/hybrid_file.hpp"
#include "common/errors.hpp"
#include "../support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

using namespace obd;

TEST_CASE("format: hybrid file recovery reads original pairs and releases ownership",
          "[format][hybrid]") {
    const auto fixtures = std::filesystem::path(__FILE__).parent_path().parent_path() /
                          "fixtures/hybrid";
    for (const auto* producer : {"overlaybd", "agentenv"}) {
        DYNAMIC_SECTION(producer) {
            test::TempDir temporary;
            const auto data = temporary / "upper.data";
            const auto index = temporary / "upper.index";
            std::filesystem::copy_file(fixtures / producer / "writable.data", data);
            std::filesystem::copy_file(fixtures / producer / "writable.index", index);
            const auto data_size = std::filesystem::file_size(data);
            const auto index_size = std::filesystem::file_size(index);
            const int rc = test::run_coro([&]() -> elio::coro::task<int> {
                auto pair = co_await format::hybrid::FilePair::open_existing(data, index, true);
                REQUIRE(pair->header().virtual_size == 32768);
                REQUIRE(pair->data_bytes() == data_size);
                REQUIRE(pair->index_bytes() == index_size);
                REQUIRE(pair->mappings().size() == 6);
                int conflict = 0;
                try {
                    auto other = co_await format::hybrid::FilePair::open_existing(data, index, true);
                    (void)other;
                } catch (const error& e) { conflict = e.errno_value(); }
                REQUIRE((conflict == EWOULDBLOCK || conflict == EAGAIN));
                pair.reset();
                auto reopened = co_await format::hybrid::FilePair::open_existing(data, index, true);
                REQUIRE(reopened->mappings().size() == 6);
                co_return 0;
            });
            REQUIRE(rc == 0);
            REQUIRE(std::filesystem::file_size(data) == data_size);
            REQUIRE(std::filesystem::file_size(index) == index_size);
        }
    }
}

TEST_CASE("format: hybrid file recovery rejects missing and sealed pairs without truncation",
          "[format][hybrid]") {
    const auto fixtures = std::filesystem::path(__FILE__).parent_path().parent_path() /
                          "fixtures/hybrid/agentenv";
    test::TempDir temporary;
    const auto data = temporary / "upper.data";
    const auto index = temporary / "upper.index";
    std::filesystem::copy_file(fixtures / "sealed.data", data);
    std::filesystem::copy_file(fixtures / "writable.index", index);
    const auto data_size = std::filesystem::file_size(data);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        int failure = 0;
        try {
            auto pair = co_await format::hybrid::FilePair::open_existing(data, temporary / "missing", true);
            (void)pair;
        } catch (const error& e) { failure = e.errno_value(); }
        REQUIRE(failure == ENOENT);
        failure = 0;
        try {
            auto pair = co_await format::hybrid::FilePair::open_existing(data, index, true);
            (void)pair;
        } catch (const error& e) { failure = e.errno_value(); }
        REQUIRE(failure == EALREADY);
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(std::filesystem::file_size(data) == data_size);
    REQUIRE_FALSE(std::filesystem::exists(temporary / "missing"));
}
