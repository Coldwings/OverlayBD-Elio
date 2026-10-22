// Replacement servers must wait for the old server's asynchronous kernel
// teardown before opening /dev/ublkcN. The injected clock and awaitables keep
// the retry policy testable without a privileged device or real-time waits.
#pragma once

#include "common/errors.hpp"

#include <elio/coro/task.hpp>

#include <algorithm>
#include <chrono>
#include <system_error>

namespace obd::ublk::detail {

template <typename Command, typename Now, typename Sleep, typename OpenQueues>
elio::coro::task<void> start_recovery_and_open_queues(
    Command command, Now now, Sleep sleep, OpenQueues open_queues) {
    using Clock = std::chrono::steady_clock;
    constexpr auto timeout = std::chrono::seconds(30);
    constexpr Clock::duration interval = std::chrono::milliseconds(50);
    const auto deadline = now() + timeout;

    for (;;) {
        try {
            co_await command();
            break;
        } catch (const std::system_error& e) {
            if (e.code() != std::errc::device_or_resource_busy) throw;
        }

        const auto current = now();
        if (current >= deadline) {
            throw error(EBUSY, "ublk START_USER_RECOVERY not ready after 30s");
        }
        co_await sleep(std::min(interval, deadline - current));
        // Timer/scheduler delay and control-command time both consume the
        // original budget. Never begin another command after it expires.
        if (now() >= deadline) {
            throw error(EBUSY, "ublk START_USER_RECOVERY not ready after 30s");
        }
    }
    // Opening before START succeeds would itself keep the driver busy.
    // Keep this outside the catch: an open failure must not retry START,
    // which has already reset the old queue state successfully.
    open_queues();
}

}  // namespace obd::ublk::detail
