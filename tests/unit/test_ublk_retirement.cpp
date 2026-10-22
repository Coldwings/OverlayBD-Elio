#include "ublk/queue.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
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
