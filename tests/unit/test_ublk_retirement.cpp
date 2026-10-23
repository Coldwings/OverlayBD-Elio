#include "ublk/queue.hpp"
#include "ublk/elio_bridge.hpp"
#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <elio/runtime/async_main.hpp>
#include <elio/runtime/affinity.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>

#include <cerrno>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <sys/eventfd.h>
#include <thread>

namespace obd::ublk {

// No kernel commands are published by these ledger tests.
struct QueueRetirementTestAccess {
    static void delivered(Queue& queue, uint16_t tag) {
        queue.raw_.resize(queue.depth_);
        queue.raw_[tag].delivered = true;
    }
    static void pending(Queue& queue, uint16_t tag) {
        queue.raw_[tag].pending = true;
    }
    static void reply(Queue& queue, uint16_t tag, int32_t result) {
        queue.raw_[tag].reply_ready = true;
        queue.raw_[tag].reply = result;
    }
    static bool has_reply(const Queue& queue, uint16_t tag) {
        return queue.raw_[tag].reply_ready;
    }
    static int32_t reply(const Queue& queue, uint16_t tag) {
        return queue.raw_[tag].reply;
    }
    static void recover(Queue& queue) { queue.recover_lost_replies(); }
    static void rejecting_transport(Queue& queue, std::function<void()> pause) {
        // Real queue owner and io_uring; eventfd has no uring_cmd callback.
        // Unlike /dev/null on newer kernels it cannot accept a no-op command.
        // No ublk device or command mapping is created.
        queue.ublkc_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        queue.elio_efd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        queue.done_efd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        queue.io_bufs_ = static_cast<uint8_t*>(std::calloc(queue.depth_, 4096));
        if (queue.ublkc_fd_ < 0 || queue.elio_efd_ < 0 ||
            queue.done_efd_ < 0 || !queue.io_bufs_)
            throw std::runtime_error("cannot prepare rejecting queue transport");
        queue.initial_submit_hook_for_test_ = std::move(pause);
    }
    static size_t owner_worker(const Queue& queue) { return queue.owner_worker_; }
    static pid_t owner_tid(const Queue& queue) { return queue.owner_tid_; }
    static unsigned owner_tid_syscalls(const Queue& queue) {
        return queue.owner_tid_syscalls_for_test_;
    }
    static unsigned owner_ring_flags(const Queue& queue) {
        return queue.ring_.flags;
    }
    static bool prepare_post_stop_commit(Queue& queue) {
        queue.raw_[0].delivered = true;
        queue.raw_[0].reply_ready = true;
        queue.raw_[0].reply = -ECANCELED;
        queue.kernel_stopped();
        queue.prepare_replies();
        const bool prepared = queue.raw_[0].pending &&
                              queue.raw_[0].completing &&
                              !queue.raw_[0].delivered &&
                              queue.raw_published_ == 1;
        // This white-box transition test deliberately does not publish the
        // prepared SQE. Restore an empty ledger before Queue destruction;
        // queue_exit then discards the userspace-only SQ entry.
        queue.raw_[0] = {};
        queue.raw_published_ = 0;
        queue.publish_retirement();
        return prepared;
    }
};

} // namespace obd::ublk

TEST_CASE("ublk: missing replies wait for native source retirement",
          "[ublk][retirement]") {
    using obd::ublk::QueueRetirementTestAccess;
    obd::ublk::Queue queue(0, 0, 1, 4096);
    QueueRetirementTestAccess::delivered(queue, 0);
    // Absence from done_ alone cannot distinguish an allocation failure from
    // a source that still borrows this tag's buffer.
    QueueRetirementTestAccess::recover(queue);
    REQUIRE_FALSE(QueueRetirementTestAccess::has_reply(queue, 0));
    queue.sources_drained();
    QueueRetirementTestAccess::recover(queue);
    REQUIRE(QueueRetirementTestAccess::has_reply(queue, 0));
    REQUIRE(QueueRetirementTestAccess::reply(queue, 0) == -EIO);
}

TEST_CASE("ublk: owner submission drain retries flushed EINTR and partial progress",
          "[ublk][retirement][owner-pump]") {
    unsigned submit_calls = 0;
    std::vector<unsigned> enter_counts;
    std::array<int, 3> enter_results{-EINTR, 1, 1};
    size_t next = 0;

    const auto drained = obd::ublk::detail::drain_submissions(
        2, true,
        [&] {
            ++submit_calls;
            return -EINTR; // liburing has already flushed both local SQEs.
        },
        [&](unsigned remaining) {
            enter_counts.push_back(remaining);
            return enter_results.at(next++);
        });

    CHECK(submit_calls == 1);
    CHECK(enter_counts == std::vector<unsigned>{2, 2, 1});
    CHECK(drained.remaining == 0);
    CHECK(drained.error == 0);
    CHECK(drained.last == 1);

    unsigned partial_submit_calls = 0;
    unsigned direct_enters = 0;
    const auto partial = obd::ublk::detail::drain_submissions(
        3, true,
        [&] { ++partial_submit_calls; return 1; },
        [&](unsigned remaining) {
            ++direct_enters;
            return static_cast<int>(remaining);
        });
    CHECK(partial_submit_calls == 1);
    CHECK(direct_enters == 1);
    CHECK(partial.remaining == 0);
    CHECK(partial.error == 0);
}

TEST_CASE("ublk: owner CQ turn coalesces trailing hints without losing new work",
          "[ublk][retirement][owner-pump]") {
    using obd::ublk::detail::CqDrain;
    using obd::ublk::detail::drain_owner_cq_turn;

    SECTION("trailing CQ notification is consumed before park") {
        unsigned cqes = 1;
        std::array<int, 2> hints{1, 0};
        size_t next_hint = 0;
        const auto turn = drain_owner_cq_turn(
            4, [] { return 0; },
            [&](unsigned remaining) {
                const unsigned consumed = std::min(cqes, remaining);
                cqes -= consumed;
                return CqDrain{consumed, cqes != 0};
            },
            [&] { return hints.at(next_hint++); });
        CHECK(turn.consumed == 1);
        CHECK(turn.enters == 2);
        CHECK(turn.hints == 1);
        CHECK_FALSE(turn.immediate);
        CHECK(turn.error == 0);
    }

    SECTION("pre-CQE work arriving before hint drain is entered immediately") {
        unsigned enters = 0;
        unsigned cqes = 0;
        std::array<int, 3> hints{1, 1, 0};
        size_t next_hint = 0;
        const auto turn = drain_owner_cq_turn(
            4,
            [&] {
                if (++enters == 2) cqes = 1;
                return 0;
            },
            [&](unsigned remaining) {
                const unsigned consumed = std::min(cqes, remaining);
                cqes -= consumed;
                return CqDrain{consumed, cqes != 0};
            },
            [&] { return hints.at(next_hint++); });
        CHECK(turn.consumed == 1);
        CHECK(turn.enters == 3);
        CHECK(turn.hints == 2);
        CHECK(cqes == 0);
        CHECK_FALSE(turn.immediate);
        CHECK(turn.error == 0);
    }

    SECTION("work after the final empty read remains for the next armed turn") {
        unsigned cqes = 0;
        const auto empty = drain_owner_cq_turn(
            4, [] { return 0; },
            [&](unsigned) { return CqDrain{0, cqes != 0}; },
            [] { return 0; });
        CHECK(empty.enters == 1);
        CHECK_FALSE(empty.immediate);

        // A level eventfd wake arrives after the synchronous EAGAIN and is
        // consumed by the outer async read. The next turn must still enter
        // the raw ring before inspecting CQ, then clear its trailing signal.
        cqes = 1;
        bool trailing = true;
        const auto next = drain_owner_cq_turn(
            4, [] { return 0; },
            [&](unsigned remaining) {
                const unsigned consumed = std::min(cqes, remaining);
                cqes -= consumed;
                return CqDrain{consumed, cqes != 0};
            },
            [&] {
                if (trailing) {
                    trailing = false;
                    return 1;
                }
                return 0;
            });
        CHECK(next.consumed == 1);
        CHECK(next.enters == 2);
        CHECK(next.hints == 1);
        CHECK(cqes == 0);
        CHECK_FALSE(next.immediate);
        CHECK(next.error == 0);
    }
}

TEST_CASE("ublk: missing-reply repair preserves pending and completed slots",
          "[ublk][retirement]") {
    using obd::ublk::QueueRetirementTestAccess;
    obd::ublk::Queue queue(0, 0, 4, 4096);
    QueueRetirementTestAccess::delivered(queue, 0);
    QueueRetirementTestAccess::delivered(queue, 1);
    QueueRetirementTestAccess::pending(queue, 1);
    QueueRetirementTestAccess::delivered(queue, 2);
    QueueRetirementTestAccess::reply(queue, 2, 4096);
    // Slot 3 has never delivered a request.
    queue.sources_drained();
    QueueRetirementTestAccess::recover(queue);
    REQUIRE(QueueRetirementTestAccess::reply(queue, 0) == -EIO);
    REQUIRE_FALSE(QueueRetirementTestAccess::has_reply(queue, 1));
    REQUIRE(QueueRetirementTestAccess::reply(queue, 2) == 4096);
    REQUIRE_FALSE(QueueRetirementTestAccess::has_reply(queue, 3));
    QueueRetirementTestAccess::recover(queue);
    REQUIRE(QueueRetirementTestAccess::reply(queue, 2) == 4096);
}

TEST_CASE("ublk: initial issuance fence stays closed while FETCH is unsubmitted",
          "[ublk][retirement]") {
    using obd::ublk::QueueRetirementTestAccess;
    using namespace std::chrono_literals;
    obd::ublk::Queue queue(0, 0, 2, 4096);
    std::mutex mutex;
    std::condition_variable cv;
    bool paused = false;
    bool resume = false;
    bool resumed_by_test = false;
    QueueRetirementTestAccess::rejecting_transport(queue, [&] {
        std::unique_lock lock(mutex);
        paused = true;
        cv.notify_one();
        resumed_by_test = cv.wait_for(lock, 2s, [&] { return resume; });
    });
    std::atomic<bool> stop{false};
    std::thread owner([&] { queue.run(stop); });
    queue.publish_initial_fetches();
    bool observed_pause;
    {
        std::unique_lock lock(mutex);
        observed_pause = cv.wait_for(lock, 2s, [&] { return paused; });
    }
    const bool acknowledged_before_submit = queue.initial_fetches_issued();
    queue.request_release();
    queue.request_owner_exit();
    {
        std::lock_guard lock(mutex);
        resume = true;
        cv.notify_one();
    }
    owner.join();
    const auto ledger = queue.retirement();
    REQUIRE(observed_pause);
    REQUIRE(resumed_by_test);
    REQUIRE_FALSE(acknowledged_before_submit);
    REQUIRE(queue.initial_fetches_issued());
    // Rejections count as issued but not accepted. Each original still has
    // to retire independently before this owner may exit.
    REQUIRE(queue.failed());
    REQUIRE(queue.owner_exited());
    REQUIRE(ledger.known);
    REQUIRE(ledger.complete);
    REQUIRE(ledger.published == 2);
    REQUIRE(ledger.retired == ledger.published);
}

TEST_CASE("ublk: fixed worker pump retires rejected commands through raw eventfd",
          "[ublk][retirement][owner-pump]") {
    using namespace std::chrono_literals;
    using obd::ublk::QueueRetirementTestAccess;
    obd::ublk::Queue queue(0, 0, 2, 4096);
    QueueRetirementTestAccess::rejecting_transport(queue, [] {});
    obd::test::VectorSource source(std::vector<uint8_t>(4096));
    std::atomic<bool> stop{false};
    bool setup = false;
    bool issued = false;
    bool drained = false;
    bool destroyed = false;
    unsigned ring_flags = 0;

    elio::runtime::run_config config;
    config.num_threads = 2;
    config.blocking_threads = 1;
    elio::run([&]() -> elio::coro::task<void> {
        auto owner = elio::runtime::scheduler::current()->go_joinable_to(
            1, obd::ublk::run_owner_pump, &queue, &source, &stop);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!(queue.setup_ready() && queue.bridge_ready()) &&
               std::chrono::steady_clock::now() < deadline)
            co_await elio::time::sleep_for(1ms);
        setup = queue.setup_ready() && queue.bridge_ready();
        if (setup)
            ring_flags = QueueRetirementTestAccess::owner_ring_flags(queue);
        if (setup) queue.publish_initial_fetches();
        while (!queue.initial_fetches_issued() && !queue.failed() &&
               std::chrono::steady_clock::now() < deadline)
            co_await elio::time::sleep_for(1ms);
        issued = queue.initial_fetches_issued();

        // The rejecting fd returns terminal CQEs for the real raw commands.
        // Stop and retirement use the same state transitions as a real queue,
        // without loading ublk or creating a block device.
        while (!queue.failed() && std::chrono::steady_clock::now() < deadline)
            co_await elio::time::sleep_for(1ms);
        stop.store(true, std::memory_order_release);
        queue.request_release();
        while (!queue.source_handlers_drained() &&
               std::chrono::steady_clock::now() < deadline)
            co_await elio::time::sleep_for(1ms);
        drained = queue.source_handlers_drained();
        queue.kernel_stopped();
        queue.request_owner_exit();
        while (!owner.is_destroyed() &&
               std::chrono::steady_clock::now() < deadline)
            co_await elio::time::sleep_for(1ms);
        destroyed = owner.is_destroyed();
        if (destroyed) owner.await_resume();
    }, config);

    const auto ledger = queue.retirement();
    CHECK(setup);
    CHECK(issued);
    CHECK(drained);
    CHECK(destroyed);
    CHECK(queue.failed());
    CHECK(queue.owner_exited());
    CHECK(QueueRetirementTestAccess::owner_worker(queue) == 1);
    CHECK(QueueRetirementTestAccess::owner_tid(queue) > 0);
    CHECK(QueueRetirementTestAccess::owner_tid_syscalls(queue) == 1);
    CHECK((ring_flags & IORING_SETUP_SINGLE_ISSUER) != 0);
    // Linux signals the registered eventfd when deferred local work changes
    // from empty to nonempty, before its CQE is published. The owner then
    // enters this same ring with GETEVENTS to execute that work.
    CHECK((ring_flags & IORING_SETUP_DEFER_TASKRUN) != 0);
    CHECK(ledger.known);
    CHECK(ledger.complete);
    CHECK(ledger.published == 2);
    CHECK(ledger.retired == ledger.published);
}

TEST_CASE("ublk: STOP preserves a delivered request's COMMIT obligation",
          "[ublk][retirement][owner-pump]") {
    using obd::ublk::QueueRetirementTestAccess;
    obd::ublk::Queue queue(0, 0, 1, 4096);
    QueueRetirementTestAccess::rejecting_transport(queue, [] {});
    bool prepared = false;

    elio::runtime::run_config config;
    config.num_threads = 1;
    config.blocking_threads = 1;
    elio::run([&]() -> elio::coro::task<void> {
        const auto owner = elio::current_worker_id();
        queue.init_owner_pump(owner);
        prepared = QueueRetirementTestAccess::prepare_post_stop_commit(queue);
        queue.finish_owner();
        co_return;
    }, config);

    CHECK(prepared);
    CHECK(queue.retirement().complete);
}
