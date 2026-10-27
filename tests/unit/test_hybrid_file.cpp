#include "format/hybrid_file.hpp"
#include "common/errors.hpp"
#include "../support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fcntl.h>
#include <sys/file.h>
#if defined(ELIO_HAS_IO_URING) && defined(ELIO_RUNTIME_TEST_HOOKS)
#include <elio/io/io_uring_backend.hpp>
#endif

using namespace obd;

#if defined(ELIO_HAS_IO_URING) && defined(ELIO_RUNTIME_TEST_HOOKS)
TEST_CASE("format: hybrid destruction releases both locks before deferred close",
          "[format][hybrid]") {
    const auto fixtures = std::filesystem::path(__FILE__).parent_path().parent_path() /
                          "fixtures/hybrid/agentenv";
    test::TempDir temporary;
    const auto data = temporary / "upper.data";
    const auto index = temporary / "upper.index";
    std::filesystem::copy_file(fixtures / "writable.data", data);
    std::filesystem::copy_file(fixtures / "writable.index", index);
    struct Probe {
        int fd;
        ~Probe() { if (fd >= 0) ::close(fd); }
    } data_probe{::open(data.c_str(), O_RDWR | O_CLOEXEC)},
      index_probe{::open(index.c_str(), O_RDWR | O_CLOEXEC)};
    REQUIRE(data_probe.fd >= 0);
    REQUIRE(index_probe.fd >= 0);
    const int rc = elio::run([&]() -> elio::coro::task<int> {
        if (!elio::runtime::worker_thread::current()->io_context().is_io_uring())
            co_return ENOTSUP;
        auto pair = co_await format::hybrid::FilePair::open_existing(data, index, true);
        REQUIRE(::flock(data_probe.fd, LOCK_EX | LOCK_NB) == -1);
        REQUIRE(errno == EWOULDBLOCK);
        REQUIRE(::flock(index_probe.fd, LOCK_EX | LOCK_NB) == -1);
        REQUIRE(errno == EWOULDBLOCK);
        const int old_data = pair->data_fd();
        const int old_index = pair->index_fd();
        struct DeferClose {
            bool previous = elio::io::detail::defer_destructor_close_submission_for_test.exchange(true);
            ~DeferClose() {
                elio::io::detail::defer_destructor_close_submission_for_test.store(previous);
            }
        } defer;
        // No suspension between destruction and the ownership probes: the
        // single worker cannot poll its deferred CLOSE operations in between.
        pair.reset();
        REQUIRE(::fcntl(old_data, F_GETFD) >= 0);
        REQUIRE(::fcntl(old_index, F_GETFD) >= 0);
        CHECK(::flock(data_probe.fd, LOCK_EX | LOCK_NB) == 0);
        CHECK(::flock(index_probe.fd, LOCK_EX | LOCK_NB) == 0);
        co_return 0;
    }, elio::runtime::run_config{.num_threads = 1});
    if (rc == ENOTSUP) SKIP("Deferred-close regression requires an active io_uring backend");
    REQUIRE(rc == 0);
}
#endif

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
