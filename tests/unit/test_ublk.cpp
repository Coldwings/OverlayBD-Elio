// Unit tests: ublk ABI geometry and recovery retry/open ordering. The
// kernel-dependent E2E lives in tests/integration.
#include "ublk/ctrl.hpp"
#include "ublk/queue.hpp"
#include "ublk/recovery.hpp"
#include "ublk/uapi_compat.hpp"

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <system_error>
#include <vector>

using namespace obd;

TEST_CASE("ublk: command buffer geometry matches the driver layout",
          "[ublk]") {
    // ublk_drv.c: stride = round_up(4096 * desc_size, PAGE); size =
    // round_up(depth * desc_size, PAGE). desc is 24B → stride 98304.
    REQUIRE(ublk::cmd_buf_stride(sizeof(ublksrv_io_desc)) == 98304);
    REQUIRE(ublk::cmd_buf_size(128, sizeof(ublksrv_io_desc)) == 4096);
    REQUIRE(ublk::cmd_buf_size(4096, sizeof(ublksrv_io_desc)) == 98304);
}

TEST_CASE("ublk: IoRequest byte math is sector based", "[ublk]") {
    ublk::IoRequest req;
    req.tag = 3;
    req.start_sector = 100;
    req.nr_sectors = 8;
    REQUIRE(req.byte_offset() == 51200);
    REQUIRE(req.byte_len() == 4096);
}

TEST_CASE("ublk: recovery feature flags follow device params", "[ublk]") {
    using obd::ublk::DeviceParams;
    DeviceParams p;
    p.enable_recovery = true;
    const uint64_t f = obd::ublk::dev_info_flags(p);
    REQUIRE((f & UBLK_F_USER_RECOVERY) != 0);
    REQUIRE((f & UBLK_F_USER_RECOVERY_REISSUE) != 0);
    p.enable_recovery = false;
    const uint64_t f2 = obd::ublk::dev_info_flags(p);
    REQUIRE((f2 & UBLK_F_USER_RECOVERY) == 0);
    REQUIRE((f2 & UBLK_F_USER_RECOVERY_REISSUE) == 0);
}

TEST_CASE("ublk: recovery waits for quiescence before opening replacement queues",
          "[ublk][recovery]") {
    using namespace std::chrono_literals;
    auto now = std::chrono::steady_clock::time_point{};
    std::vector<std::chrono::steady_clock::duration> waits;
    bool queues_open = false;
    int attempts = 0;
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        co_await ublk::detail::start_recovery_and_open_queues(
            [&]() -> elio::coro::task<void> {
                REQUIRE_FALSE(queues_open);
                if (++attempts < 4) throw error(EBUSY, "old server still open");
                co_return;
            },
            [&] { return now; },
            [&](std::chrono::steady_clock::duration delay)
                -> elio::coro::task<void> {
                waits.push_back(delay);
                now += delay;
                co_return;
            },
            [&] { queues_open = true; });
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(attempts == 4);
    REQUIRE(queues_open);
    REQUIRE(waits == std::vector<std::chrono::steady_clock::duration>{
                         50ms, 50ms, 50ms});
}

TEST_CASE("ublk: recovery does not retry nonbusy failures",
          "[ublk][recovery]") {
    for (const int failure : {EINVAL, EACCES, ECANCELED}) {
        INFO("errno=" << failure);
        auto now = std::chrono::steady_clock::time_point{};
        int attempts = 0;
        int sleeps = 0;
        bool queues_open = false;
        int observed = 0;
        const int rc = test::run_coro([&]() -> elio::coro::task<int> {
            try {
                co_await ublk::detail::start_recovery_and_open_queues(
                    [&]() -> elio::coro::task<void> {
                        ++attempts;
                        throw error(failure, "START_USER_RECOVERY");
                        co_return;
                    },
                    [&] { return now; },
                    [&](std::chrono::steady_clock::duration delay)
                        -> elio::coro::task<void> {
                        ++sleeps;
                        now += delay;
                        co_return;
                    },
                    [&] { queues_open = true; });
            } catch (const std::system_error& e) {
                observed = e.code().value();
            }
            co_return 0;
        });
        REQUIRE(rc == 0);
        REQUIRE(attempts == 1);
        REQUIRE(sleeps == 0);
        REQUIRE(observed == failure);
        REQUIRE_FALSE(queues_open);
        REQUIRE(now == std::chrono::steady_clock::time_point{});
    }
}

TEST_CASE("ublk: recovery busy retries share one monotonic deadline",
          "[ublk][recovery]") {
    using namespace std::chrono_literals;
    auto now = std::chrono::steady_clock::time_point{};
    std::vector<std::chrono::steady_clock::duration> waits;
    int attempts = 0;
    bool queues_open = false;
    int observed = 0;
    std::string message;
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        try {
            co_await ublk::detail::start_recovery_and_open_queues(
                [&]() -> elio::coro::task<void> {
                    ++attempts;
                    // Command time consumes the same budget as retry sleeps.
                    now += 9970ms;
                    throw error(EBUSY, "not quiesced");
                    co_return;
                },
                [&] { return now; },
                [&](std::chrono::steady_clock::duration delay)
                    -> elio::coro::task<void> {
                    waits.push_back(delay);
                    now += delay;
                    co_return;
                },
                [&] { queues_open = true; });
        } catch (const std::system_error& e) {
            observed = e.code().value();
            message = e.what();
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(observed == EBUSY);
    REQUIRE(message.find("START_USER_RECOVERY not ready after 30s") !=
            std::string::npos);
    REQUIRE(attempts == 3);
    REQUIRE_FALSE(queues_open);
    // The third command ends at 30010ms, so no extra sleep is allowed.
    REQUIRE(waits == std::vector<std::chrono::steady_clock::duration>{
                         50ms, 50ms});
    REQUIRE(now == std::chrono::steady_clock::time_point{} + 30010ms);
}

TEST_CASE("ublk: recovery caps its final wait and never retries after expiry",
          "[ublk][recovery]") {
    using namespace std::chrono_literals;
    auto now = std::chrono::steady_clock::time_point{};
    std::vector<std::chrono::steady_clock::duration> waits;
    int attempts = 0;
    int observed = 0;
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        try {
            co_await ublk::detail::start_recovery_and_open_queues(
                [&]() -> elio::coro::task<void> {
                    ++attempts;
                    now += 29980ms;
                    throw error(EBUSY, "not quiesced");
                    co_return;
                },
                [&] { return now; },
                [&](std::chrono::steady_clock::duration delay)
                    -> elio::coro::task<void> {
                    waits.push_back(delay);
                    now += delay;
                    co_return;
                },
                [] {});
        } catch (const std::system_error& e) {
            observed = e.code().value();
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(observed == EBUSY);
    REQUIRE(attempts == 1);
    REQUIRE(waits == std::vector<std::chrono::steady_clock::duration>{20ms});
    REQUIRE(now == std::chrono::steady_clock::time_point{} + 30s);
}

TEST_CASE("ublk: replacement queue open failure never repeats accepted recovery",
          "[ublk][recovery]") {
    using namespace std::chrono_literals;
    auto now = std::chrono::steady_clock::time_point{};
    int attempts = 0;
    int sleeps = 0;
    int opens = 0;
    int observed = 0;
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        try {
            co_await ublk::detail::start_recovery_and_open_queues(
                [&]() -> elio::coro::task<void> {
                    ++attempts;
                    now += 10s;
                    co_return;
                },
                [&] { return now; },
                [&](std::chrono::steady_clock::duration delay)
                    -> elio::coro::task<void> {
                    ++sleeps;
                    now += delay;
                    co_return;
                },
                [&] {
                    ++opens;
                    throw error(EBUSY, "replacement cdev open failed");
                });
        } catch (const std::system_error& e) {
            observed = e.code().value();
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(observed == EBUSY);
    REQUIRE(attempts == 1);
    REQUIRE(opens == 1);
    REQUIRE(sleeps == 0);
}
