// Internal integration-test seam; not a control-plane API. Normal
// run_daemon installs no gate. This concrete bounded gate runs no caller
// callbacks, including at publication while lifecycle op_mu is held.
#pragma once

#include "supervisor/daemon.hpp"
#include "supervisor/child.hpp"

#include <elio/time/timer.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <map>
#include <string_view>
#include <thread>

namespace obd::supervisor::detail {

// A bounded pause at a real daemon boundary. The gate has only a weak
// child reference and runs outside mu_; it cannot repair product ownership.
class DaemonTestGate {
public:
    void mark(std::string point) {
        std::lock_guard lock(mu);
        ++observations[std::move(point)];
    }
    size_t count(const std::string& point) {
        std::lock_guard lock(mu);
        return observations[point];
    }
    bool is_armed(std::string_view point) {
        std::lock_guard lock(mu);
        return armed == point;
    }
    void arm(std::string point) {
        std::lock_guard lock(mu);
        armed = std::move(point);
    }
    elio::coro::task<void> observe(std::string_view point,
                                     std::weak_ptr<Child> child) {
        {
            std::lock_guard lock(mu);
            if (point != armed || hit) {
                if (hit && !released && std::this_thread::get_id() != worker) {
                    distinct_worker = true;
                }
                co_return;
            }
            owner = std::move(child);
            worker = std::this_thread::get_id();
            hit = true;
            cv.notify_all();
        }
        // Only the synchronous fixture thread waits on cv. Blocking an
        // Elio worker here would also stall its unrelated I/O completions.
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(10);
        for (;;) {
            {
                std::lock_guard lock(mu);
                if (released) co_return;
                if (std::chrono::steady_clock::now() >= deadline) {
                    timed_out = true;
                    co_return;
                }
            }
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
    }
    bool wait() {
        std::unique_lock lock(mu);
        return cv.wait_for(lock, std::chrono::seconds(10), [&] { return hit; });
    }
    void release() {
        std::lock_guard lock(mu);
        released = true;
        cv.notify_all();
    }
    std::weak_ptr<Child> child() {
        std::lock_guard lock(mu);
        return owner;
    }
    bool concurrent_workers() {
        std::lock_guard lock(mu);
        return distinct_worker;
    }
    bool timeout() {
        std::lock_guard lock(mu);
        return timed_out;
    }
private:
    std::map<std::string, size_t> observations;
    std::mutex mu;
    std::condition_variable cv;
    std::string armed;
    bool hit = false;
    bool released = false;
    bool timed_out = false;
    std::weak_ptr<Child> owner;
    std::thread::id worker;
    bool distinct_worker = false;
};

elio::coro::task<int> run_daemon_with_test_gate(
    const DaemonConfig& cfg, std::shared_ptr<DaemonTestGate> gate);

}  // namespace obd::supervisor::detail
