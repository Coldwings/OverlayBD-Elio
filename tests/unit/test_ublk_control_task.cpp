#include "ublk/control_task.hpp"

#include <elio/runtime/async_main.hpp>
#include <elio/runtime/spawn.hpp>
#include <catch2/catch_test_macros.hpp>

#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace {
elio::runtime::run_config one_worker() {
    elio::runtime::run_config config;
    config.num_threads = 1;
    config.blocking_threads = 1;
    return config;
}
}

TEST_CASE("ublk: cold controller leaves the sole blocking worker available",
          "[ublk][control-task]") {
    std::atomic<bool> entered{false};
    std::atomic<bool> completed{false};
    std::mutex mutex;
    std::condition_variable cv;
    bool source_completed = false;
    bool controller_saw_source = false;
    bool off_worker = false;
    elio::run([&]() -> elio::coro::task<void> {
        auto control = elio::spawn(obd::ublk::detail::run_control([&] {
            off_worker = elio::runtime::worker_thread::current() == nullptr;
            entered.store(true);
            std::unique_lock lock(mutex);
            // A broken pool-based controller reaches this bounded deadline
            // before the source can acquire that pool's only worker.
            controller_saw_source = cv.wait_for(
                lock, std::chrono::seconds(2), [&] { return source_completed; });
            completed.store(true);
        }));
        while (!entered.load()) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK_FALSE(control.is_ready());
        co_await elio::spawn_blocking([&] {
            std::lock_guard lock(mutex);
            source_completed = true;
            cv.notify_one();
        });
        co_await control;
        co_await control.wait_destroyed_async();
    }, one_worker());
    REQUIRE(controller_saw_source);
    REQUIRE(completed.load());
    REQUIRE(off_worker);
}

TEST_CASE("ublk: controller failure propagates after action scope retirement",
          "[ublk][control-task]") {
    std::atomic<bool> scope_destroyed{false};
    bool observed_failure = false;
    elio::run([&]() -> elio::coro::task<void> {
        try {
            co_await obd::ublk::detail::run_control([&] {
                struct Scope {
                    std::atomic<bool>& destroyed;
                    ~Scope() { destroyed.store(true); }
                } scope{scope_destroyed};
                throw std::runtime_error("controller failed");
            });
        } catch (const std::runtime_error& e) {
            observed_failure = std::string(e.what()) == "controller failed";
            CHECK(scope_destroyed.load());
        }
    }, one_worker());
    REQUIRE(observed_failure);
    REQUIRE(scope_destroyed.load());
}

TEST_CASE("ublk: controller cancellation intent retains the running action",
          "[ublk][control-task]") {
    std::atomic<bool> entered{false};
    std::atomic<bool> action_done{false};
    std::mutex mutex;
    std::condition_variable cv;
    bool release = false;
    bool released_by_caller = false;
    elio::run([&]() -> elio::coro::task<void> {
        auto control = elio::spawn(obd::ublk::detail::run_control([&] {
            entered.store(true);
            std::unique_lock lock(mutex);
            released_by_caller = cv.wait_for(
                lock, std::chrono::seconds(2), [&] { return release; });
            action_done.store(true);
        }));
        while (!entered.load()) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        control.request_cancel();
        co_await elio::time::sleep_for(std::chrono::milliseconds(10));
        CHECK_FALSE(control.is_ready());
        CHECK_FALSE(action_done.load());
        {
            std::lock_guard lock(mutex);
            release = true;
            cv.notify_one();
        }
        co_await control;
        co_await control.wait_destroyed_async();
    }, one_worker());
    REQUIRE(released_by_caller);
    REQUIRE(action_done.load());
}
