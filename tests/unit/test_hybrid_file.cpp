#include "format/hybrid_file.hpp"
#include "format/hybrid_rw.hpp"
#include "common/errors.hpp"
#include "../support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <filesystem>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#if defined(ELIO_HAS_IO_URING) && defined(ELIO_RUNTIME_TEST_HOOKS)
#include <elio/io/io_uring_backend.hpp>
#endif

using namespace obd;

#ifdef OBD_TEST_HOOKS
TEST_CASE("format: hybrid creation boundaries preserve only complete published pairs",
          "[format][hybrid]") {
    for (bool process_exit : {false, true}) {
        for (int stage = 1; stage <= 6; ++stage) {
            DYNAMIC_SECTION("exit=" << process_exit << " stage=" << stage) {
                test::TempDir temporary;
                const auto directory = temporary / "pair";
                struct ResetFault {
                    ~ResetFault() { format::hybrid::creation_fault_for_test(-1); }
                } reset;
                if (process_exit) {
                    const std::string selected = std::to_string(stage);
                    const pid_t child = ::fork();
                    REQUIRE(child >= 0);
                    if (child == 0) {
                        ::execl(OBD_TEST_HYBRID_WRITER_BIN, OBD_TEST_HYBRID_WRITER_BIN,
                                "crash-create", directory.c_str(), selected.c_str(), nullptr);
                        ::_exit(127);
                    }
                    int status = 0;
                    pid_t waited;
                    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
                    REQUIRE(waited == child);
                    REQUIRE(WIFEXITED(status));
                    REQUIRE(WEXITSTATUS(status) == 90 + stage);
                } else {
                    format::hybrid::creation_fault_for_test(stage);
                    const int failure = test::run_coro([&]() -> elio::coro::task<int> {
                        try { (void)co_await format::hybrid::FilePair::create(directory, 32768); }
                        catch (const error& e) { co_return e.errno_value(); }
                        co_return 0;
                    });
                    REQUIRE(failure == EIO);
                    format::hybrid::creation_fault_for_test(-1);
                }
                REQUIRE(std::filesystem::exists(directory) == (stage >= 5));
                const auto abandoned_count = [&] {
                    size_t count = 0;
                    for (const auto& entry : std::filesystem::directory_iterator(temporary.path()))
                        if (entry.path().filename().string().starts_with(".hybrid-init-")) ++count;
                    return count;
                };
                // A process exit skips RAII cleanup; private unpublished
                // staging names are retained and never adopted by retry.
                const size_t abandoned = process_exit && stage < 5 ? 1 : 0;
                REQUIRE(abandoned_count() == abandoned);
                const int rc = test::run_coro([&]() -> elio::coro::task<int> {
                    auto upper = co_await format::HybridRwLayer::open_or_create(directory, 32768);
                    std::vector<uint8_t> actual(32768, 0xff), payload(512, 0x61);
                    const auto initial = co_await upper->pread(actual.data(), actual.size(), 0);
                    REQUIRE(initial == 32768);
                    REQUIRE(std::all_of(actual.begin(), actual.end(), [](uint8_t byte) { return byte == 0; }));
                    const auto written = co_await upper->pwrite(payload.data(), payload.size(), 512);
                    REQUIRE(written == 512);
                    const int flushed = co_await upper->flush();
                    REQUIRE(flushed == 0);
                    upper.reset();
                    upper = co_await format::HybridRwLayer::open_existing(directory + "/data", directory + "/index");
                    const auto read = co_await upper->pread(actual.data(), payload.size(), 512);
                    REQUIRE(read == 512);
                    REQUIRE(std::equal(payload.begin(), payload.end(), actual.begin()));
                    co_return 0;
                });
                REQUIRE(rc == 0);
                REQUIRE(abandoned_count() == abandoned);
            }
        }
    }
}
#endif

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
