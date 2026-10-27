#include "format/lsmt_rw_file.hpp"
#include "format/lsmt_rw.hpp"
#include "common/errors.hpp"
#include "../support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <sys/wait.h>
#include <elio/runtime/spawn_blocking.hpp>

using namespace obd;
namespace {
template<class F>
auto run_pair(F&& body) {
    elio::run_config runtime;
    runtime.num_threads = 2;
    runtime.blocking_threads = 1;
    return elio::run(std::forward<F>(body), runtime);
}
void copy_pair(const test::TempDir& directory) {
    const auto fixtures = std::filesystem::path(__FILE__).parent_path().parent_path() /
                          "fixtures/lsmt/overlaybd";
    std::filesystem::copy_file(fixtures / "writable.data", directory / "data");
    std::filesystem::copy_file(fixtures / "writable.index", directory / "index");
}

TEST_CASE("format: LSMT imported pair resumes writes after unflushed process death",
          "[format][lsmt-rw-file]") {
    test::TempDir directory;
    copy_pair(directory);
    const std::string data = directory / "data", index = directory / "index";
    const auto fixtures = std::filesystem::path(__FILE__).parent_path().parent_path() /
                          "fixtures/lsmt/overlaybd";
    std::ifstream input(fixtures / "expected.raw", std::ios::binary);
    std::vector<uint8_t> expected{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    REQUIRE(expected.size() == 32768);
    std::fill_n(expected.begin() + 512, 512, 0xa1);
    std::fill_n(expected.begin() + 30 * 512, 512, 0xb2);
    std::fill_n(expected.begin() + 2 * 512, 512, 0);
    std::fill_n(expected.begin() + 10 * 512, 512, 0xc3);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        ::execl(OBD_TEST_LSMT_CRASH_WRITER_BIN, OBD_TEST_LSMT_CRASH_WRITER_BIN,
                data.c_str(), index.c_str(), nullptr);
        ::_exit(127);
    }
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    REQUIRE(waited == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 73);
    const int rc = run_pair([&]() -> elio::coro::task<int> {
        auto upper = co_await format::LsmtRwLayer::open_existing(data, index);
        std::vector<uint8_t> actual(expected.size());
        const auto read = co_await upper->pread(actual.data(), actual.size(), 0);
        REQUIRE(read == static_cast<ssize_t>(actual.size()));
        REQUIRE(actual == expected);
        std::array<uint8_t, 512> next{}; next.fill(0xd4);
        const auto written = co_await upper->pwrite(next.data(), next.size(), 31 * 512);
        REQUIRE(written == 512);
        const int flushed = co_await upper->flush();
        REQUIRE(flushed == 0);
        upper.reset();
        upper = co_await format::LsmtRwLayer::open_existing(data, index);
        std::fill_n(expected.begin() + 31 * 512, 512, 0xd4);
        const auto reread = co_await upper->pread(actual.data(), actual.size(), 0);
        REQUIRE(reread == static_cast<ssize_t>(actual.size()));
        REQUIRE(actual == expected);
        co_return 0;
    });
    REQUIRE(rc == 0);
}
}

TEST_CASE("format: LSMT pair replays completed appends without FLUSH and compacts under stable ownership",
          "[format][lsmt-rw-file]") {
    test::TempDir directory;
    copy_pair(directory);
    const int rc = run_pair([&]() -> elio::coro::task<int> {
        auto pair = co_await format::lsmt_rw::FilePair::open_existing(directory / "data", directory / "index");
        const auto original = pair->mappings();
        REQUIRE_FALSE(original.empty());
        bytes::segment_mapping zero;
        zero.offset = 30; zero.length = 2; zero.moffset = 8; zero.zeroed = true;
        const int appended = co_await pair->append(zero);
        REQUIRE(appended == 0);
        pair.reset();
        pair = co_await format::lsmt_rw::FilePair::open_existing(directory / "data", directory / "index");
        bool found = false;
        for (const auto& m : pair->mappings())
            if (m.offset == 30 && m.length == 2 && m.zeroed) found = true;
        REQUIRE(found);
        const auto count = pair->mappings().size();
        const int compacted = co_await pair->compact(pair->mappings());
        REQUIRE(compacted == 0);
        REQUIRE(pair->index_bytes() == 4096 + 16 * count);
        int second_error = 0;
        try { (void)co_await format::lsmt_rw::FilePair::open_existing(directory / "data", directory / "index"); }
        catch (const error& e) { second_error = e.errno_value(); }
        REQUIRE(second_error == EWOULDBLOCK);
        const int flushed = co_await pair->flush();
        REQUIRE(flushed == 0);
        pair.reset();
        pair = co_await format::lsmt_rw::FilePair::open_existing(directory / "data", directory / "index");
        REQUIRE(pair->mappings().size() == count);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: LSMT pair repairs only incomplete suffix after validating completed records",
          "[format][lsmt-rw-file]") {
    test::TempDir directory;
    copy_pair(directory);
    const auto index = directory / "index";
    const auto original_bytes = std::filesystem::file_size(index);
    {
        std::ofstream stream(index, std::ios::binary | std::ios::app);
        stream.write("partial", 7);
    }
    SECTION("valid prefix is retained") {
        const int rc = run_pair([&]() -> elio::coro::task<int> {
            auto pair = co_await format::lsmt_rw::FilePair::open_existing(directory / "data", index);
            REQUIRE(pair->index_bytes() == original_bytes);
            co_return 0;
        });
        REQUIRE(rc == 0);
        REQUIRE(std::filesystem::file_size(index) == original_bytes);
    }
    SECTION("invalid full record prevents all repairs") {
        std::fstream stream(index, std::ios::binary | std::ios::in | std::ios::out);
        std::array<char, 16> invalid{};
        stream.seekp(4096); stream.write(invalid.data(), invalid.size()); stream.close();
        const int failure = run_pair([&]() -> elio::coro::task<int> {
            try { (void)co_await format::lsmt_rw::FilePair::open_existing(directory / "data", index); }
            catch (const error& e) { co_return e.errno_value(); }
            co_return 0;
        });
        REQUIRE(failure == EINVAL);
        REQUIRE(std::filesystem::file_size(index) == original_bytes + 7);
    }
}

TEST_CASE("format: LSMT pair refuses complete sealed data before opening its stale index",
          "[format][lsmt-rw-file]") {
    test::TempDir directory;
    const auto fixture = std::filesystem::path(__FILE__).parent_path().parent_path() /
                         "fixtures/lsmt/overlaybd/sealed.data";
    std::filesystem::copy_file(fixture, directory / "data");
    const int failure = run_pair([&]() -> elio::coro::task<int> {
        try { (void)co_await format::lsmt_rw::FilePair::open_existing(directory / "data", directory / "missing-index"); }
        catch (const error& e) { co_return e.errno_value(); }
        co_return 0;
    });
    REQUIRE(failure == EALREADY);
    REQUIRE_FALSE(std::filesystem::exists(directory / "missing-index"));
    REQUIRE(std::filesystem::file_size(directory / "data") == std::filesystem::file_size(fixture));
}

#ifdef OBD_TEST_HOOKS
TEST_CASE("format: LSMT legacy checkpoint migration survives publication faults without retiring old metadata",
          "[format][lsmt-rw-file]") {
    for (bool crash : {false, true}) {
        for (int stage = 1; stage <= 4; ++stage) {
            DYNAMIC_SECTION("crash=" << crash << " stage=" << stage) {
                test::TempDir directory;
                const std::string data = directory / "data", index = directory / "index";
                const int setup = run_pair([&]() -> elio::coro::task<int> {
                    auto legacy = co_await format::LsmtRwLayer::create_legacy_for_test(data, 32768);
                    std::array<uint8_t, 2048> payload{}; payload.fill(0x31);
                    const auto written = co_await legacy->pwrite(payload.data(), payload.size(), 0);
                    REQUIRE(written == 2048);
                    const int discarded = co_await legacy->discard(512, 512);
                    REQUIRE(discarded == 0);
                    co_return co_await legacy->checkpoint();
                });
                REQUIRE(setup == 0);
                std::ifstream old_file(data, std::ios::binary);
                const std::vector<char> old_bytes{std::istreambuf_iterator<char>(old_file), {}};
                struct Reset { ~Reset() { format::lsmt_rw::migration_fault_for_test(-1); } } reset;
                if (crash) {
                    const auto selected = std::to_string(stage);
                    const pid_t child = ::fork(); REQUIRE(child >= 0);
                    if (child == 0) {
                        ::execl(OBD_TEST_LSMT_CRASH_WRITER_BIN, OBD_TEST_LSMT_CRASH_WRITER_BIN,
                                data.c_str(), index.c_str(), "migrate-crash", selected.c_str(), nullptr);
                        ::_exit(127);
                    }
                    int status = 0; pid_t waited;
                    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
                    REQUIRE(waited == child); REQUIRE(WIFEXITED(status));
                    REQUIRE(WEXITSTATUS(status) == 110 + stage);
                } else {
                    format::lsmt_rw::migration_fault_for_test(stage);
                    const int failure = run_pair([&]() -> elio::coro::task<int> {
                        try { (void)co_await format::LsmtRwLayer::migrate_checkpoint(data, index); }
                        catch (const error& e) { co_return e.errno_value(); }
                        co_return 0;
                    });
                    REQUIRE(failure == EIO);
                    format::lsmt_rw::migration_fault_for_test(-1);
                }
                REQUIRE(std::filesystem::exists(index) == (stage >= 3));
                const int recovered = run_pair([&]() -> elio::coro::task<int> {
                    auto upper = co_await format::LsmtRwLayer::migrate_checkpoint(data, index);
                    std::array<uint8_t, 2048> actual{};
                    const auto read = co_await upper->pread(actual.data(), actual.size(), 0);
                    REQUIRE(read == 2048);
                    std::array<uint8_t, 2048> expected{}; expected.fill(0x31);
                    std::fill_n(expected.begin() + 512, 512, 0);
                    REQUIRE(actual == expected);
                    std::array<uint8_t, 512> payload{}; payload.fill(0x72);
                    const auto written = co_await upper->pwrite(payload.data(), payload.size(), 8192);
                    REQUIRE(written == 512);
                    REQUIRE(std::filesystem::file_size(data) == old_bytes.size() + 512);
                    std::ifstream current(data, std::ios::binary);
                    std::vector<char> prefix(old_bytes.size()); current.read(prefix.data(), prefix.size());
                    REQUIRE(prefix == old_bytes);
                    upper.reset();
                    upper = co_await format::LsmtRwLayer::open_existing(data, index);
                    std::array<uint8_t, 512> tail{};
                    const auto tail_read = co_await upper->pread(tail.data(), tail.size(), 8192);
                    REQUIRE(tail_read == 512); REQUIRE(tail == payload);
                    co_return 0;
                });
                REQUIRE(recovered == 0);
            }
        }
    }
}

TEST_CASE("format: LSMT pair creation resumes only its complete staged artifacts",
          "[format][lsmt-rw-file]") {
    for (bool crash : {false, true}) {
        for (int stage = 1; stage <= 6; ++stage) {
            DYNAMIC_SECTION("crash=" << crash << " stage=" << stage) {
                test::TempDir directory;
                const std::string data = directory / "data", index = directory / "index";
                struct Reset { ~Reset() { format::lsmt_rw::create_fault_for_test(-1); } } reset;
                if (crash) {
                    const auto selected = std::to_string(stage);
                    const pid_t child = ::fork(); REQUIRE(child >= 0);
                    if (child == 0) {
                        ::execl(OBD_TEST_LSMT_CRASH_WRITER_BIN, OBD_TEST_LSMT_CRASH_WRITER_BIN,
                                data.c_str(), index.c_str(), "create-crash", selected.c_str(), nullptr);
                        ::_exit(127);
                    }
                    int status = 0; pid_t waited;
                    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
                    REQUIRE(waited == child); REQUIRE(WIFEXITED(status));
                    REQUIRE(WEXITSTATUS(status) == 100 + stage);
                } else {
                    format::lsmt_rw::create_fault_for_test(stage);
                    const int failure = run_pair([&]() -> elio::coro::task<int> {
                        try { (void)co_await format::LsmtRwLayer::create_persistent(data, index, 32768); }
                        catch (const error& e) { co_return e.errno_value(); }
                        co_return 0;
                    });
                    REQUIRE(failure == EIO);
                    format::lsmt_rw::create_fault_for_test(-1);
                }
                REQUIRE(std::filesystem::exists(data) == (stage >= 5));
                REQUIRE(std::filesystem::exists(index) == (stage >= 4));
                const int recovered = run_pair([&]() -> elio::coro::task<int> {
                    std::unique_ptr<format::LsmtRwLayer> upper;
                    if (stage >= 3)
                        upper = co_await format::LsmtRwLayer::open_existing(data, index);
                    else
                        upper = co_await format::LsmtRwLayer::create_persistent(data, index, 32768);
                    REQUIRE(upper->segments().empty());
                    std::array<uint8_t, 512> payload{}; payload.fill(0x77);
                    const auto written = co_await upper->pwrite(payload.data(), payload.size(), 1024);
                    REQUIRE(written == 512);
                    const int discarded = co_await upper->discard(4096, 512);
                    REQUIRE(discarded == 0);
                    upper.reset();
                    upper = co_await format::LsmtRwLayer::open_existing(data, index);
                    std::array<uint8_t, 512> actual{};
                    const auto read = co_await upper->pread(actual.data(), actual.size(), 1024);
                    REQUIRE(read == 512); REQUIRE(actual == payload);
                    REQUIRE(upper->segments().size() == 2);
                    REQUIRE(upper->segments().back().zeroed);
                    REQUIRE_FALSE(std::filesystem::exists(index + ".init"));
                    co_return 0;
                });
                REQUIRE(recovered == 0);
            }
        }
    }
}

TEST_CASE("format: LSMT creation never replaces an existing header-only upper or foreign destination",
          "[format][lsmt-rw-file]") {
    test::TempDir directory;
    const std::string data = directory / "data", index = directory / "index";
    SECTION("missing index does not imply empty ownership") {
        const int setup = run_pair([&]() -> elio::coro::task<int> {
            auto upper = co_await format::LsmtRwLayer::create_persistent(data, index, 32768);
            co_return co_await upper->discard(4096, 512);
        });
        REQUIRE(setup == 0);
        REQUIRE(std::filesystem::file_size(data) == 4096);
        std::filesystem::remove(index);
        const int failed = run_pair([&]() -> elio::coro::task<int> {
            try { (void)co_await format::LsmtRwLayer::open_existing(data, index); }
            catch (const error& e) { co_return e.errno_value(); }
            co_return 0;
        });
        REQUIRE(failed == ENOENT);
        const int create_failed = run_pair([&]() -> elio::coro::task<int> {
            try { (void)co_await format::LsmtRwLayer::create_persistent(data, index, 32768); }
            catch (const error& e) { co_return e.errno_value(); }
            co_return 0;
        });
        REQUIRE(create_failed == EEXIST);
        REQUIRE_FALSE(std::filesystem::exists(index));
        REQUIRE(std::filesystem::file_size(data) == 4096);
    }
    SECTION("publication validates both destinations before linking either") {
        format::lsmt_rw::create_fault_for_test(3);
        const int setup = run_pair([&]() -> elio::coro::task<int> {
            try { (void)co_await format::LsmtRwLayer::create_persistent(data, index, 32768); }
            catch (const error& e) { co_return e.errno_value(); }
            co_return 0;
        });
        format::lsmt_rw::create_fault_for_test(-1);
        REQUIRE(setup == EIO);
        { std::ofstream file(data); file << "foreign"; }
        const int failure = run_pair([&]() -> elio::coro::task<int> {
            try { (void)co_await format::LsmtRwLayer::open_existing(data, index); }
            catch (const error& e) { co_return e.errno_value(); }
            co_return 0;
        });
        REQUIRE(failure == EINVAL);
        REQUIRE(std::filesystem::file_size(data) == 7);
        REQUIRE_FALSE(std::filesystem::exists(index));
    }
}

TEST_CASE("format: LSMT grow intent repairs torn capacity but rejects changed identity",
          "[format][lsmt-rw-file]") {
    test::TempDir directory;
    copy_pair(directory);
    const std::string data = directory / "data", index = directory / "index";
    const int setup = run_pair([&]() -> elio::coro::task<int> {
        auto upper = co_await format::LsmtRwLayer::open_existing(data, index);
        format::lsmt_rw::grow_fault_for_test(1);
        auto grow = [layer = upper.get()] { return layer->grow(65536); };
        const int rc = co_await elio::spawn_blocking(std::move(grow));
        format::lsmt_rw::grow_fault_for_test(-1);
        co_return rc;
    });
    REQUIRE(setup == -EIO);
    SECTION("capacity bytes can be interrupted independently in both headers") {
        for (const auto& path : {data, index}) {
            std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
            // 32768 -> 65536, interrupted after the first two capacity bytes.
            std::array<char, 8> torn{};
            file.seekp(48); file.write(torn.data(), torn.size());
        }
        const int rc = run_pair([&]() -> elio::coro::task<int> {
            auto upper = co_await format::LsmtRwLayer::open_existing(data, index);
            REQUIRE(upper->virtual_size() == 65536);
            co_return 0;
        });
        REQUIRE(rc == 0);
        REQUIRE_FALSE(std::filesystem::exists(index + ".grow"));
    }
    SECTION("an obsolete intent cannot shrink a newer capacity") {
        {
            std::fstream file(data, std::ios::binary | std::ios::in | std::ios::out);
            std::array<uint8_t, 8> newer{}; bytes::store_u64_le(newer.data(), 131072);
            file.seekp(48); file.write(reinterpret_cast<const char*>(newer.data()), newer.size());
        }
        const int failed = run_pair([&]() -> elio::coro::task<int> {
            try { (void)co_await format::LsmtRwLayer::open_existing(data, index); }
            catch (const error& e) { co_return e.errno_value(); }
            co_return 0;
        });
        REQUIRE(failed == EINVAL);
        std::ifstream file(data, std::ios::binary); file.seekg(48);
        std::array<uint8_t, 8> capacity{};
        file.read(reinterpret_cast<char*>(capacity.data()), capacity.size());
        REQUIRE(bytes::load_u64_le(capacity.data()) == 131072);
    }
    SECTION("an intent cannot repair an unrelated identity change") {
        {
            std::fstream file(data, std::ios::binary | std::ios::in | std::ios::out);
            file.seekp(56); file.put('a');
        }
        std::ifstream before_stream(data, std::ios::binary);
        const std::vector<char> before{std::istreambuf_iterator<char>(before_stream), {}};
        const int failed = run_pair([&]() -> elio::coro::task<int> {
            try { (void)co_await format::LsmtRwLayer::open_existing(data, index); }
            catch (const error& e) { co_return e.errno_value(); }
            co_return 0;
        });
        REQUIRE(failed == EINVAL);
        std::ifstream after_stream(data, std::ios::binary);
        const std::vector<char> after{std::istreambuf_iterator<char>(after_stream), {}};
        REQUIRE(after == before);
        REQUIRE(std::filesystem::exists(index + ".grow"));
    }
}

TEST_CASE("format: LSMT paired grow recovers every publication boundary and permits retry",
          "[format][lsmt-rw-file]") {
    for (bool crash : {false, true}) {
        for (int stage = 1; stage <= 5; ++stage) {
            DYNAMIC_SECTION("crash=" << crash << " stage=" << stage) {
                test::TempDir directory;
                copy_pair(directory);
                const std::string data = directory / "data", index = directory / "index";
                struct Reset { ~Reset() { format::lsmt_rw::grow_fault_for_test(-1); } } reset;
                if (crash) {
                    const auto selected = std::to_string(stage);
                    const pid_t child = ::fork();
                    REQUIRE(child >= 0);
                    if (child == 0) {
                        ::execl(OBD_TEST_LSMT_CRASH_WRITER_BIN, OBD_TEST_LSMT_CRASH_WRITER_BIN,
                                data.c_str(), index.c_str(), "grow-crash", selected.c_str(), nullptr);
                        ::_exit(127);
                    }
                    int status = 0; pid_t waited;
                    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
                    REQUIRE(waited == child);
                    REQUIRE(WIFEXITED(status));
                    REQUIRE(WEXITSTATUS(status) == 80 + stage);
                }
                const int rc = run_pair([&]() -> elio::coro::task<int> {
                    auto upper = co_await format::LsmtRwLayer::open_existing(data, index);
                    if (!crash) {
                        format::lsmt_rw::grow_fault_for_test(stage);
                        auto failing = [layer = upper.get()] { return layer->grow(65536); };
                        const int failed = co_await elio::spawn_blocking(std::move(failing));
                        REQUIRE(failed == -EIO);
                        std::array<uint8_t, 512> bytes{};
                        const auto blocked = co_await upper->pwrite(bytes.data(), bytes.size(), 512);
                        REQUIRE(blocked == -EIO);
                        format::lsmt_rw::grow_fault_for_test(-1);
                        auto shrinking_retry = [layer = upper.get()] { return layer->grow(32768); };
                        const int rejected = co_await elio::spawn_blocking(std::move(shrinking_retry));
                        REQUIRE(rejected == -EINVAL);
                        REQUIRE(upper->virtual_size() == 65536);
                        auto retry = [layer = upper.get()] { return layer->grow(65536); };
                        const int retried = co_await elio::spawn_blocking(std::move(retry));
                        REQUIRE(retried == 0);
                    }
                    REQUIRE(upper->virtual_size() == 65536);
                    std::array<uint8_t, 512> payload{}; payload.fill(0xab);
                    const auto written = co_await upper->pwrite(payload.data(), payload.size(), 65024);
                    REQUIRE(written == 512);
                    const int flushed = co_await upper->flush();
                    REQUIRE(flushed == 0);
                    upper.reset();
                    upper = co_await format::LsmtRwLayer::open_existing(data, index);
                    REQUIRE(upper->virtual_size() == 65536);
                    std::array<uint8_t, 512> actual{};
                    const auto read = co_await upper->pread(actual.data(), actual.size(), 65024);
                    REQUIRE(read == 512);
                    REQUIRE(actual == payload);
                    REQUIRE_FALSE(std::filesystem::exists(index + ".grow"));
                    co_return 0;
                });
                REQUIRE(rc == 0);
            }
        }
    }
}
#endif

#ifdef OBD_TEST_HOOKS
TEST_CASE("format: LSMT partial mapping append poisons writer until validated recovery",
          "[format][lsmt-rw-file]") {
    test::TempDir directory;
    copy_pair(directory);
    struct Reset { ~Reset() { format::lsmt_rw::io_fault_for_test(-1); } } reset;
    const auto index = directory / "index";
    const auto before = std::filesystem::file_size(index);
    const int rc = run_pair([&]() -> elio::coro::task<int> {
        auto upper = co_await format::LsmtRwLayer::open_existing(directory / "data", index);
        format::lsmt_rw::io_fault_for_test(1);
        const int failed = co_await upper->discard(30 * 512, 512);
        REQUIRE(failed == -EIO);
        REQUIRE(std::filesystem::file_size(index) == before + 8);
        format::lsmt_rw::io_fault_for_test(-1);
        const int retry = co_await upper->discard(31 * 512, 512);
        REQUIRE(retry == -EIO);
        const int flush = co_await upper->flush();
        REQUIRE(flush == -EIO);
        upper.reset();
        upper = co_await format::LsmtRwLayer::open_existing(directory / "data", index);
        REQUIRE(std::filesystem::file_size(index) == before);
        for (const auto& m : upper->segments()) REQUIRE_FALSE((m.offset <= 30 && m.end() > 30));
        const int resumed = co_await upper->discard(31 * 512, 512);
        REQUIRE(resumed == 0);
        upper.reset();
        upper = co_await format::LsmtRwLayer::open_existing(directory / "data", index);
        bool found = false;
        for (const auto& m : upper->segments())
            if (m.offset <= 31 && m.end() > 31 && m.zeroed) found = true;
        REQUIRE(found);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: LSMT checkpoint retries each synchronization failure before becoming terminal",
          "[format][lsmt-rw-file]") {
    struct Reset { ~Reset() { format::lsmt_rw::io_fault_for_test(-1); } } reset;
    for (int stage = 2; stage <= 4; ++stage) {
        CAPTURE(stage);
        test::TempDir directory;
        copy_pair(directory);
        const int rc = run_pair([&]() -> elio::coro::task<int> {
            auto upper = co_await format::LsmtRwLayer::open_existing(directory / "data", directory / "index");
            format::lsmt_rw::io_fault_for_test(stage);
            const int failed = co_await upper->checkpoint();
            REQUIRE(failed == -EIO);
            format::lsmt_rw::io_fault_for_test(-1);
            const int written = co_await upper->discard(30 * 512, 512);
            REQUIRE(written == 0);
            const int retried = co_await upper->checkpoint();
            REQUIRE(retried == 0);
            const int terminal = co_await upper->discard(31 * 512, 512);
            REQUIRE(terminal == -EROFS);
            upper.reset();
            upper = co_await format::LsmtRwLayer::open_existing(directory / "data", directory / "index");
            bool found = false;
            for (const auto& m : upper->segments())
                if (m.offset <= 30 && m.end() > 30 && m.zeroed) found = true;
            REQUIRE(found);
            const int resumed = co_await upper->discard(31 * 512, 512);
            REQUIRE(resumed == 0);
            co_return 0;
        });
        REQUIRE(rc == 0);
    }
}

TEST_CASE("format: LSMT compaction retains the authoritative index after publication failures",
          "[format][lsmt-rw-file]") {
    struct Reset { ~Reset() { format::lsmt_rw::io_fault_for_test(-1); } } reset;
    for (int stage = 5; stage <= 7; ++stage) {
        CAPTURE(stage);
        test::TempDir directory;
        copy_pair(directory);
        const int rc = run_pair([&]() -> elio::coro::task<int> {
            auto pair = co_await format::lsmt_rw::FilePair::open_existing(directory / "data", directory / "index");
            const auto live = pair->mappings();
            const auto before = pair->index_bytes();
            format::lsmt_rw::io_fault_for_test(stage);
            const int failed = co_await pair->compact(live);
            REQUIRE(failed == -EIO);
            REQUIRE(pair->healthy());
            REQUIRE(pair->index_bytes() == (stage == 7 ? 4096 + live.size() * 16 : before));
            REQUIRE(std::filesystem::file_size(directory / "index") == pair->index_bytes());
            int owner_error = 0;
            try { (void)co_await format::lsmt_rw::FilePair::open_existing(directory / "data", directory / "index"); }
            catch (const error& e) { owner_error = e.errno_value(); }
            REQUIRE(owner_error == EWOULDBLOCK);
            format::lsmt_rw::io_fault_for_test(-1);
            bytes::segment_mapping zero;
            zero.offset = 30; zero.length = 1; zero.moffset = 8; zero.zeroed = true;
            const int appended = co_await pair->append(zero);
            REQUIRE(appended == 0);
            pair.reset();
            pair = co_await format::lsmt_rw::FilePair::open_existing(directory / "data", directory / "index");
            bool found = false;
            for (const auto& m : pair->mappings())
                if (m.offset == 30 && m.length == 1 && m.zeroed) found = true;
            REQUIRE(found);
            const int retried = co_await pair->compact(pair->mappings());
            REQUIRE(retried == 0);
            pair.reset();
            auto upper = co_await format::LsmtRwLayer::open_existing(directory / "data", directory / "index");
            std::array<uint8_t, 512> actual{};
            const auto read = co_await upper->pread(actual.data(), actual.size(), 63 * 512);
            REQUIRE(read == 512);
            for (const auto byte : actual) REQUIRE(byte == 0x77);
            co_return 0;
        });
        REQUIRE(rc == 0);
    }
}
#endif

#ifdef OBD_TEST_HOOKS
TEST_CASE("format: LSMT admission counts replay splits and uncoalesced records before publication",
          "[format][lsmt-rw-file]") {
    struct Reset { ~Reset() { format::lsmt_rw::live_limit_for_test(0); } } reset;
    for (const bool split : {false, true}) {
        CAPTURE(split);
        test::TempDir directory;
        const auto data = directory / "data", index = data + ".index";
        const int rc = run_pair([&]() -> elio::coro::task<int> {
            auto upper = co_await format::LsmtRwLayer::create(data, 4096);
            std::array<uint8_t, 512> bytes{}; bytes.fill(0x42);
            format::lsmt_rw::live_limit_for_test(split ? 2 : 3);
            if (split) {
                const int zero = co_await upper->discard(0, 3 * 512);
                REQUIRE(zero == 0);
            } else {
                for (uint64_t sector = 0; sector < 3; ++sector) {
                    const auto wrote = co_await upper->pwrite(bytes.data(), bytes.size(), sector * 512);
                    REQUIRE(wrote == 512);
                }
                REQUIRE(upper->segments().size() == 1); // Replay still has three records.
            }
            const auto before = std::filesystem::file_size(index);
            const auto rejected = co_await upper->pwrite(bytes.data(), bytes.size(), split ? 512 : 1536);
            REQUIRE(rejected == -EFBIG);
            REQUIRE(std::filesystem::file_size(index) == before);
            const int flushed = co_await upper->flush();
            REQUIRE(flushed == 0);
            const int replaces = co_await upper->discard(0, 3 * 512);
            REQUIRE(replaces == 0); // Rejection does not poison a usable writer.
            const auto resumed = co_await upper->pwrite(bytes.data(), bytes.size(), 1536);
            REQUIRE(resumed == 512);
            upper.reset();
            upper = co_await format::LsmtRwLayer::open_existing(data, index);
            std::array<uint8_t, 2048> actual{};
            const auto read = co_await upper->pread(actual.data(), actual.size(), 0);
            REQUIRE(read == 2048);
            for (size_t i = 0; i < actual.size(); ++i) REQUIRE(actual[i] == (i < 1536 ? 0 : 0x42));
            co_return 0;
        });
        REQUIRE(rc == 0);
    }
}
#endif

#ifdef OBD_TEST_HOOKS
TEST_CASE("format: LSMT partial admission requires reopen before further mutation", "[format][lsmt-rw-file]") {
    test::TempDir directory;
    struct Reset { ~Reset() { format::lsmt_rw::live_limit_for_test(0); } } reset;
    const auto data = directory / "data";
    const int rc = run_pair([&]() -> elio::coro::task<int> {
        auto upper = co_await format::LsmtRwLayer::create(data, 16384 * 512);
        format::lsmt_rw::live_limit_for_test(1);
        const int failed = co_await upper->discard(0, 16384 * 512);
        REQUIRE(failed == -EFBIG); // First full-length record already published.
        format::lsmt_rw::live_limit_for_test(0);
        const int poisoned = co_await upper->discard(0, 512);
        REQUIRE(poisoned == -EIO);
        upper.reset();
        upper = co_await format::LsmtRwLayer::open_existing(data, data + ".index");
        REQUIRE(upper->segments().size() == 1);
        REQUIRE(upper->segments()[0].length == 16383);
        REQUIRE(upper->segments()[0].zeroed);
        const int resumed = co_await upper->discard(16383 * 512, 512);
        REQUIRE(resumed == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}
#endif
