#pragma once

#include <elio/coro/task.hpp>
#include <elio/runtime/spawn_blocking.hpp>
#include <elio/time/timer.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <thread>
#include <utility>

namespace obd::ublk::detail {

/// Cold control commands may wait for source IO that itself uses the runtime's
/// only blocking worker. Give the command independent execution capacity.
/// After launch, observation failures cannot release this frame's captures:
/// the command must return and its thread must be joined first.
template <class Action>
elio::coro::task<void> run_control(Action action) {
    std::atomic<bool> done{false};
    std::exception_ptr failure;
    std::thread worker([&]() noexcept {
        try {
            action();
        } catch (...) {
            failure = std::current_exception();
        }
        done.store(true, std::memory_order_release);
    });
    while (!done.load(std::memory_order_acquire)) {
        try {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        } catch (...) {
            // Keep the command/payload owner even if an observer allocation
            // fails or the calling context has requested cancellation.
        }
    }
    // No source operation depends on this command anymore. Join off-worker,
    // retaining the frame if allocation of that small blocking task fails.
    while (worker.joinable()) {
        try {
            co_await elio::spawn_blocking([&] { worker.join(); });
        } catch (...) {
        }
    }
    if (failure) std::rethrow_exception(failure);
}

} // namespace obd::ublk::detail
