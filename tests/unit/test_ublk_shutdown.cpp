// Ordinary lifecycle tests use real Device/bridge code and eventfds. Only
// /dev/ublkc registration, its command mapping and queue ring are bypassed.
#include "supervisor/device_control.hpp"
#include "supervisor/child.hpp"
#include "cmd/startup_socket.hpp"
#include "ublk/device.hpp"
#include "ublk/elio_bridge.hpp"
#include "ublk/tag_engine.hpp"

#include "../support.hpp"
#include "../runtime_faults.hpp"

#include <elio/runtime/spawn.hpp>
#include <elio/runtime/affinity.hpp>
#include <elio/runtime/spawn_blocking.hpp>
#include <elio/time/timer.hpp>
#include <elio/sync/event.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sys/eventfd.h>
#include <sys/socket.h>
#include <poll.h>

#include <cstdlib>
#include <array>
#include <cerrno>
#include <cstring>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <functional>
#include <system_error>
#include <thread>
#include <type_traits>

namespace obd::ublk {

struct DeviceTestAccess {
    static std::unique_ptr<Device> make(source::BlobSourcePtr source,
                                        bool queue = true, uint16_t depth = 1) {
        auto dev = std::unique_ptr<Device>(new Device());
        dev->src_ = std::move(source);
        if (queue) {
            auto q = std::make_unique<Queue>(0, 0, depth, 4096);
            q->elio_efd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            q->done_efd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            if (q->elio_efd_ < 0 || q->done_efd_ < 0) {
                throw std::runtime_error("test queue eventfd failed");
            }
            q->io_bufs_ = static_cast<uint8_t*>(std::calloc(depth, 4096));
            if (!q->io_bufs_) throw std::bad_alloc();
            dev->queues_.push_back(std::move(q));
        }
        return dev;
    }

    static void start(Device& dev, bool read = false, uint16_t reads = 1) {
        auto* q = dev.queues_.front().get();
        if (read) {
            for (uint16_t tag = 0; tag < reads; ++tag) {
                q->pending_.push_back(IoRequest{tag, UBLK_IO_OP_READ, 0, tag, 1});
            }
            q->notify_elio();
        }
        dev.bridge_tasks_.reserve(dev.bridge_tasks_.size() + 1);
        dev.bridge_tasks_.push_back(elio::spawn(
            run_bridge, q, dev.src_.get(), &dev.stop_));
    }

    static void start_on(Device& dev, size_t owner) {
        dev.bridge_tasks_.reserve(dev.bridge_tasks_.size() + 1);
        dev.bridge_tasks_.push_back(
            elio::runtime::scheduler::current()->go_joinable_to(
                owner, run_bridge, dev.queues_.front().get(),
                dev.src_.get(), &dev.stop_));
    }

    static void clear_completions(Device& dev) {
        auto& q = *dev.queues_.front();
        std::lock_guard lock(q.mu_);
        q.done_.clear();
    }

    static elio::coro::task<std::unique_ptr<Device>> fail_startup(
        Device& dev, StartupControl& startup, std::exception_ptr failure,
        bool recovery = false, Device::FailureCleanup cleanup = {}) {
        dev.startup_ = &startup;
        // Without a control transport this fixture has no device identity.
        if (!dev.ctrl_) dev.dev_id_ = UINT32_MAX;
        co_return co_await dev.fail_startup(recovery, failure, cleanup);
    }

    static void recovering(Device& dev, StartupControl& startup,
                           detail::CtrlTransport& transport) {
        dev.startup_ = &startup;
        dev.dev_id_ = 42;
        dev.publication_released_ = true;
        dev.ctrl_ = std::make_unique<Ctrl>(transport);
        dev.ctrl_->disable_destructor_cleanup();
        dev.ctrl_->set_observer({&dev, Device::cancel_control,
                                 Device::complete_control, Device::observe_control});
        if (!dev.queues_.empty()) {
            // This stand-in descriptor is closed by real release_once, not by
            // the scripted control transport. No kernel device is created.
            dev.queues_.front()->ublkc_fd_ = ::eventfd(0, EFD_CLOEXEC);
            if (dev.queues_.front()->ublkc_fd_ < 0)
                throw std::runtime_error("test cdev stand-in failed");
        }
    }
    static void start_rejecting_queues(Device& dev, std::function<void()> pause_second) {
        dev.queues_.reserve(2);
        dev.threads_.reserve(2);
        for (uint16_t index = 0; index < 2; ++index) {
            auto q = std::make_unique<Queue>(42, index, 2, 4096);
            // Real queue rings submit to an eventfd, which rejects uring_cmd.
            // This exercises publication/retirement without a kernel device.
            q->ublkc_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            q->elio_efd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            q->done_efd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            q->io_bufs_ = static_cast<uint8_t*>(std::calloc(2, 4096));
            if (q->ublkc_fd_ < 0 || q->elio_efd_ < 0 || q->done_efd_ < 0 || !q->io_bufs_)
                throw std::runtime_error("rejecting queue fixture setup failed");
            if (index == 1) q->initial_submit_hook_for_test_ = pause_second;
            dev.queues_.push_back(std::move(q));
        }
        for (auto& q : dev.queues_) {
            dev.threads_.emplace_back([&dev, queue = q.get()] { queue->run(dev.stop_); });
        }
        for (auto& q : dev.queues_) q->publish_initial_fetches();
    }
    static Queue& queue_at(Device& dev, size_t index) { return *dev.queues_[index]; }
    static bool all_tags_retired(const Queue& queue) {
        if (queue.raw_.size() != queue.depth_) return false;
        for (const auto& tag : queue.raw_)
            if (tag.pending || tag.delivered || tag.reply_ready) return false;
        return true;
    }

    static void end_recovery(Device& dev) { dev.ctrl_->end_user_recovery(dev.dev_id_); }
    static void start_device(Device& dev) { dev.ctrl_->start_dev(dev.dev_id_); }
    static bool publish_ready(Device& dev) { return dev.try_publish_ready(); }
    static bool owns_id(const Device& dev) { return dev.owns_id_; }

    static bool stopping(const Device& dev) { return dev.stop_.load(); }
    static int running(const Device& dev) {
        int live = 0;
        for (const auto& bridge : dev.bridge_tasks_) {
            if (!bridge.is_destroyed()) ++live;
        }
        return live;
    }
    static void cancel_bridge(Device& dev) { dev.bridge_tasks_.front().request_cancel(); }
    static void enqueue_read(Device& dev) {
        auto& q = *dev.queues_.front();
        {
            std::lock_guard lock(q.mu_);
            q.pending_.push_back(IoRequest{0, UBLK_IO_OP_READ, 0, 0, 1});
        }
        q.notify_elio();
    }
    static size_t completion_count(Device& dev) {
        auto& q = *dev.queues_.front();
        std::lock_guard lock(q.mu_);
        return q.done_.size();
    }
    static auto completions(Device& dev) {
        auto& q = *dev.queues_.front();
        std::lock_guard lock(q.mu_);
        return q.done_;
    }
    static bool completed_read(Device& dev) {
        auto& q = *dev.queues_.front();
        std::lock_guard<std::mutex> lock(q.mu_);
        return q.done_.size() == 1 && q.done_.front().second == 512;
    }
    static Queue& queue(Device& dev) { return *dev.queues_.front(); }
    static int done_efd(Device& dev) { return queue(dev).done_efd_; }
    static void set_completion_enqueue_hook(Device& dev, std::function<void()> hook) {
        queue(dev).completion_enqueue_hook_for_test_ = std::move(hook);
    }
    static void set_resume_hooks(
        Device& dev,
        std::function<elio::coro::task<void>(BridgeResumePoint)> resume,
        std::function<void(BridgeResumePoint)> observe) {
        queue(dev).bridge_resume_hook_for_test_ = std::move(resume);
        queue(dev).bridge_resume_observer_for_test_ = std::move(observe);
    }
    static void stop_after_admission(Device& dev, bool& admitted) {
        queue(dev).bridge_admission_hook_for_test_ = [&dev, &admitted] {
            admitted = true;
            dev.stop_.store(true);
            queue(dev).notify_elio();
        };
    }
    static void set_admission_hook(Device& dev, std::function<void()> hook) {
        queue(dev).bridge_admission_hook_for_test_ = std::move(hook);
    }
    static bool rescue_closing_tag(Device& dev) {
        // Called with no suspension on the sole bridge worker. TagEngine clears
        // this pointer before freeing its events, even before the outer
        // bridge join handle has reported final destruction.
        auto* ready = queue(dev).bridge_ready_for_test_;
        if (!ready) return false;
        ready->set();
        return true;
    }
};

}  // namespace obd::ublk

namespace {

struct ReadState {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> pool_ran{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> destroyed{false};
    std::atomic<bool> destroyed_off_worker{false};
};

struct OwnerSinkObservation {
    std::atomic<unsigned> published{0};
    std::atomic<unsigned> notified{0};
    std::atomic<unsigned> failed{0};
    std::atomic<int> failure{0};
    std::atomic<size_t> notify_worker{elio::coro::NO_AFFINITY};
    std::atomic<size_t> publish_worker{elio::coro::NO_AFFINITY};
    std::thread::id notify_thread;
    std::thread::id publish_thread;
    obd::ublk::OwnerResult result;
};

void record_owner_result(void* context, obd::ublk::OwnerResult result) {
    auto& observed = *static_cast<OwnerSinkObservation*>(context);
    observed.result = result;
    observed.publish_thread = std::this_thread::get_id();
    observed.publish_worker.store(elio::current_worker_id(),
                                  std::memory_order_release);
    observed.published.fetch_add(1, std::memory_order_release);
}

void record_sink_failure(void* context, int error) noexcept {
    auto& observed = *static_cast<OwnerSinkObservation*>(context);
    observed.failure.store(error, std::memory_order_release);
    observed.failed.fetch_add(1, std::memory_order_release);
}

void record_owner_notification(void* context) noexcept {
    auto& observed = *static_cast<OwnerSinkObservation*>(context);
    observed.notify_thread = std::this_thread::get_id();
    observed.notify_worker.store(elio::current_worker_id(),
                                 std::memory_order_release);
    observed.notified.fetch_add(1, std::memory_order_release);
}

// Scripted control CQEs exercise the real Ctrl/Device ownership callbacks.
// The optional delayed START/END waits for actual cancellation submission before
// delivering original success. Queue delivery uses the ordinary bridge fixture;
// this is not proof of raw FETCH retirement on a real ublk driver.
class LifecycleTransport final : public obd::ublk::detail::CtrlTransport {
public:
    ReadState* source = nullptr;
    obd::ublk::Queue* queue = nullptr;
    obd::ublk::Device* device = nullptr;
    std::atomic<bool> native_frames_gone_at_stop{false};
    std::atomic<unsigned> stops{0}, deletions{0};
    std::atomic<bool> source_finished_at_stop{false}, cdev_closed_at_delete{false};
    bool refuse_first_stop = false;
    int end_result = 0;
    bool delay_start_success = false;
    obd::ublk::StartupControl* startup = nullptr;
    unsigned cancellations = 0;
    bool timed_out_before_success = false;
    bool cancellation_target_matches = false;

    io_uring_sqe* get_sqe() noexcept override { prepared_ = true; return sqes_.data(); }
    unsigned sq_ready() noexcept override { return prepared_ ? 1 : 0; }
    int submit() noexcept override {
        if (!prepared_) return 0;
        prepared_ = false;
        cqe_ = {};
        cqe_.user_data = sqes_[0].user_data;
        if (sqes_[0].opcode == IORING_OP_ASYNC_CANCEL) {
            ++cancellations;
            cancellation_target_matches = sqes_[0].addr == delayed_cookie_;
            if (startup) {
                const auto facts = startup->snapshot();
                timed_out_before_success = facts.reason_errno == ETIMEDOUT &&
                    facts.control_pending && !facts.original_known;
            }
            cqe_.res = -ENOENT;
            available_ = true;
            original_after_cancel_ = true;
            return 1;
        }
        if (delay_start_success && (sqes_[0].cmd_op == UBLK_U_CMD_START_DEV ||
                                   sqes_[0].cmd_op == UBLK_U_CMD_END_USER_RECOVERY)) {
            delayed_cookie_ = cqe_.user_data;
            delayed_result_ = sqes_[0].cmd_op == UBLK_U_CMD_END_USER_RECOVERY
                ? end_result : 0;
            available_ = false;
            return 1;
        }
        switch (sqes_[0].cmd_op) {
        case UBLK_U_CMD_STOP_DEV:
            source_finished_at_stop.store(!source || source->finished.load());
            native_frames_gone_at_stop.store(!device ||
                obd::ublk::DeviceTestAccess::running(*device) == 0);
            if (stops.fetch_add(1) == 0 && refuse_first_stop) cqe_.res = -EIO;
            break;
        case UBLK_U_CMD_DEL_DEV:
            cdev_closed_at_delete.store(!queue || queue->cdev_fd() == -1);
            deletions.fetch_add(1);
            break;
        case UBLK_U_CMD_START_DEV: break;
        case UBLK_U_CMD_END_USER_RECOVERY: cqe_.res = end_result; break;
        default: cqe_.res = -EINVAL; break;
        }
        available_ = true;
        return 1;
    }
    int enter() noexcept override { return 0; }
    int peek_cqe(io_uring_cqe** cqe) noexcept override {
        *cqe = available_ ? &cqe_ : nullptr;
        return available_ ? 0 : -EAGAIN;
    }
    void cqe_seen(io_uring_cqe*) noexcept override {
        if (original_after_cancel_) {
            cqe_ = {};
            cqe_.user_data = delayed_cookie_;
            cqe_.res = delayed_result_;
            original_after_cancel_ = false;
            available_ = true;
        } else {
            available_ = false;
        }
    }
    void idle() noexcept override { std::this_thread::yield(); }
private:
    std::array<io_uring_sqe, 2> sqes_{};
    bool prepared_ = false, available_ = false;
    uint64_t delayed_cookie_ = 0;
    int delayed_result_ = 0;
    bool original_after_cancel_ = false;
    io_uring_cqe cqe_{};
};

struct CleanupWithoutCopies {
    unsigned* calls;
    unsigned* copies;
    CleanupWithoutCopies(unsigned& call_count, unsigned& copy_count)
        : calls(&call_count), copies(&copy_count) {}
    CleanupWithoutCopies(CleanupWithoutCopies&&) = default;
    CleanupWithoutCopies(const CleanupWithoutCopies& other)
        : calls(other.calls), copies(other.copies) {
        ++*copies;
        throw std::runtime_error("cleanup must not be copied after admission");
    }
    elio::coro::task<void> operator()() {
        ++*calls;
        co_return;
    }
};

class CompletingSource final : public obd::source::BlobSource {
public:
    explicit CompletingSource(ReadState& state) : state_(state) {}
    ~CompletingSource() override {
        state_.destroyed_off_worker.store(
            elio::runtime::worker_thread::current() == nullptr);
        state_.destroyed.store(true);
    }
    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                  uint64_t) override {
        state_.entered.store(true);
        while (!state_.release.load()) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        // This is intentionally submitted after stop starts. A drain that
        // monopolizes the sole pool thread would prevent normal completion.
        co_await elio::spawn_blocking([this] { state_.pool_ran.store(true); });
        std::memset(buf, 0x43, count);
        state_.finished.store(true);
        co_return static_cast<ssize_t>(count);
    }
    uint64_t size() const noexcept override { return 4096; }
    std::string_view label() const noexcept override { return "shutdown-read"; }
private:
    ReadState& state_;
};

struct TagReadState {
    elio::sync::event release;
    std::atomic<int> entered{0};
    std::atomic<int> finished{0};
    std::atomic<bool> destroyed{false};
};

class TagReadSource final : public obd::source::BlobSource {
public:
    TagReadSource(TagReadState& state, bool throw_second)
        : state_(state), throw_second_(throw_second) {}
    ~TagReadSource() override { state_.destroyed.store(true); }
    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                   uint64_t offset) override {
        state_.entered.fetch_add(1);
        co_await state_.release.wait();
        co_await elio::spawn_blocking([this] { state_.finished.fetch_add(1); });
        if (throw_second_ && offset == 512) throw std::runtime_error("tag source failure");
        std::memset(buf, 0x57, count);
        co_return static_cast<ssize_t>(count);
    }
    uint64_t size() const noexcept override { return 8192; }
    std::string_view label() const noexcept override { return "tag-drain"; }
private:
    TagReadState& state_;
    bool throw_second_;
};

elio::runtime::run_config one_worker() {
    elio::runtime::run_config config;
    config.num_threads = 1;
    config.blocking_threads = 1;
    return config;
}

struct LocalityState {
    std::atomic<unsigned> calls{0};
    std::array<size_t, 8> before{};
    std::array<size_t, 8> after{};
    LocalityState() {
        before.fill(elio::coro::NO_AFFINITY);
        after.fill(elio::coro::NO_AFFINITY);
    }
};

class LocalitySource final : public obd::source::BlobSource {
public:
    explicit LocalitySource(LocalityState& state, bool migrate = false)
        : state_(state), migrate_(migrate) {}
    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                   uint64_t) override {
        const auto index = state_.calls.fetch_add(1);
        if (index >= state_.before.size()) co_return -EIO;
        state_.before[index] = elio::current_worker_id();
        if (migrate_) co_await elio::set_affinity(1);
        co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        state_.after[index] = elio::current_worker_id();
        std::memset(buf, 0x62, count);
        co_return static_cast<ssize_t>(count);
    }
    uint64_t size() const noexcept override { return 4096; }
    std::string_view label() const noexcept override { return "locality-read"; }
private:
    LocalityState& state_;
    bool migrate_;
};

elio::coro::task<bool> await_test_completion(obd::ublk::Device& dev) {
    for (unsigned attempt = 0; attempt != 5000; ++attempt) {
        if (!obd::ublk::DeviceTestAccess::completions(dev).empty()) co_return true;
        co_await elio::time::sleep_for(std::chrono::milliseconds(1));
    }
    co_return false;
}

}  // namespace

TEST_CASE("ublk: repeated tag reads stay on the bridge worker", "[ublk][locality]") {
    using obd::ublk::DeviceTestAccess;
    LocalityState state;
    elio::runtime::run_config config;
    config.num_threads = 4;
    config.blocking_threads = 1;
    bool completed = true;
    elio::run([&]() -> elio::coro::task<void> {
        auto dev = DeviceTestAccess::make(std::make_unique<LocalitySource>(state));
        DeviceTestAccess::start_on(*dev, 0);
        for (unsigned read = 0; read != 4; ++read) {
            DeviceTestAccess::enqueue_read(*dev);
            if (!co_await await_test_completion(*dev)) {
                completed = false;
                break;
            }
            completed = completed && DeviceTestAccess::completed_read(*dev);
            DeviceTestAccess::clear_completions(*dev);
        }
        co_await dev->stop_async();
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, config);
    CHECK(completed);
    REQUIRE(state.calls.load() == 4);
    for (unsigned read = 0; read != 4; ++read) {
        CAPTURE(read);
        CHECK(state.before[read] == 0);
        CHECK(state.after[read] == 0);
    }
}

TEST_CASE("ublk: a reused tag does not readmit a scheduler child", "[ublk][locality]") {
    using obd::ublk::DeviceTestAccess;
    LocalityState state;
    elio::runtime::run_config config;
    config.num_threads = 4;
    config.blocking_threads = 1;
    bool primed = false;
    bool reused = false;
    bool rejection_unused = false;
    // Reset before cleanup and on any exceptional test departure so the
    // global native fault never escapes this case into another spawn.
    struct RejectionReset {
        ~RejectionReset() {
            elio::runtime::detail::reject_next_spawn_for_test.store(false);
        }
    } reset;
    elio::run([&]() -> elio::coro::task<void> {
        auto dev = DeviceTestAccess::make(std::make_unique<LocalitySource>(state));
        DeviceTestAccess::start_on(*dev, 0);
        DeviceTestAccess::enqueue_read(*dev);
        const bool first = co_await await_test_completion(*dev);
        primed = first && DeviceTestAccess::completed_read(*dev);
        DeviceTestAccess::clear_completions(*dev);
        if (primed) {
            elio::runtime::detail::reject_next_spawn_for_test.store(true);
            DeviceTestAccess::enqueue_read(*dev);
            const bool second = co_await await_test_completion(*dev);
            reused = second && DeviceTestAccess::completed_read(*dev);
            rejection_unused =
                elio::runtime::detail::reject_next_spawn_for_test.exchange(false);
        }
        elio::runtime::detail::reject_next_spawn_for_test.store(false);
        co_await dev->stop_async();
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, config);
    CHECK(primed);
    CHECK(reused);
    CHECK(rejection_unused);
    CHECK(state.calls.load() == 2);
}

TEST_CASE("ublk: a warm tag rejects new reads after parent cancellation", "[ublk][locality]") {
    using obd::ublk::DeviceTestAccess;
    LocalityState state;
    elio::runtime::run_config config;
    config.num_threads = 4;
    config.blocking_threads = 1;
    bool primed = false;
    std::deque<std::pair<uint16_t, int32_t>> completions;
    elio::run([&]() -> elio::coro::task<void> {
        auto dev = DeviceTestAccess::make(std::make_unique<LocalitySource>(state));
        DeviceTestAccess::start_on(*dev, 0);
        DeviceTestAccess::enqueue_read(*dev);
        const bool first = co_await await_test_completion(*dev);
        primed = first && DeviceTestAccess::completed_read(*dev);
        DeviceTestAccess::clear_completions(*dev);
        if (primed) {
            DeviceTestAccess::cancel_bridge(*dev);
            DeviceTestAccess::enqueue_read(*dev);
            (void)co_await await_test_completion(*dev);
        }
        co_await dev->stop_async();
        completions = DeviceTestAccess::completions(*dev);
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, config);
    CHECK(primed);
    CHECK(state.calls.load() == 1);
    REQUIRE(completions.size() == 1);
    CHECK(completions.front().first == 0);
    CHECK(completions.front().second == -EIO);
}

TEST_CASE("ublk: a tag returns to its bridge after source migration", "[ublk][locality]") {
    using obd::ublk::DeviceTestAccess;
    LocalityState state;
    elio::runtime::run_config config;
    config.num_threads = 4;
    config.blocking_threads = 1;
    bool completed = true;
    elio::run([&]() -> elio::coro::task<void> {
        auto dev = DeviceTestAccess::make(std::make_unique<LocalitySource>(state, true));
        DeviceTestAccess::start_on(*dev, 0);
        for (unsigned read = 0; read != 4; ++read) {
            DeviceTestAccess::enqueue_read(*dev);
            if (!co_await await_test_completion(*dev)) {
                completed = false;
                break;
            }
            completed = completed && DeviceTestAccess::completed_read(*dev);
            DeviceTestAccess::clear_completions(*dev);
        }
        co_await dev->stop_async();
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, config);
    CHECK(completed);
    REQUIRE(state.calls.load() == 4);
    for (unsigned read = 0; read != 4; ++read) {
        CAPTURE(read);
        CHECK(state.before[read] == 0);
        CHECK(state.after[read] == 1);
    }
}

TEST_CASE("ublk: foreign resumptions retain tag state until owner restoration",
          "[ublk][locality]") {
    using obd::ublk::BridgeResumePoint;
    using obd::ublk::DeviceTestAccess;
    auto point = BridgeResumePoint::tag_wait;
    SECTION("warm tag event wait") {}
    SECTION("source return") { point = BridgeResumePoint::source_return; }
    SECTION("request notification retry") { point = BridgeResumePoint::request_retry; }
    SECTION("stop notification retry") { point = BridgeResumePoint::close_retry; }
    LocalityState reads;
    obd::test::WorkerEnqueueFault pressure;
    struct Observation {
        std::mutex mutex;
        std::condition_variable changed;
        std::atomic<bool> armed{false};
        bool accepting_pressure = true;
        bool foreign = false;
        bool released = false;
        bool rejection_observed = false;
        unsigned rejections = 0;
        unsigned calls_during_pressure = 0;
        size_t completions_during_pressure = 0;
        size_t restored_worker = elio::coro::NO_AFFINITY;
        bool boundary_observed = false;
        bool accessed_before_release = false;
        std::atomic<bool> release_owner{false};
        std::atomic<bool> owner_held{false};
        std::atomic<size_t> held_worker{elio::coro::NO_AFFINITY};
        std::atomic<bool> controller_failed{false};
    } observed;
    std::optional<elio::coro::join_handle<void>> owner_hold;
    // All borrowed state and the Device owner remain outside the runtime.
    auto dev = DeviceTestAccess::make(std::make_unique<LocalitySource>(reads));
    DeviceTestAccess::set_resume_hooks(*dev,
        [&](BridgeResumePoint resumed) -> elio::coro::task<void> {
            if (resumed != point || !observed.armed.exchange(false)) co_return;
            // This is deterministic boundary injection using public migration,
            // not a claim that the runtime steal fallback itself was observed.
            co_await elio::set_affinity(1);
            owner_hold.emplace(elio::runtime::scheduler::current()->go_joinable_to(
                0, [&]() -> elio::coro::task<void> {
                    observed.held_worker.store(elio::current_worker_id());
                    observed.owner_held.store(true);
                    // Intentional fixture-only owner hold. The OS controller
                    // releases it without requiring scheduler admission.
                    while (!observed.release_owner.load()) {
                        observed.release_owner.wait(false);
                    }
                    co_return;
                }));
            while (!observed.owner_held.load()) co_await elio::time::yield();
            {
                std::lock_guard lock(observed.mutex);
                if (!observed.accepting_pressure) co_return;
                pressure.arm();
                observed.foreign = elio::current_worker_id() == 1;
            }
            observed.changed.notify_all();
        },
        [&](BridgeResumePoint resumed) {
            std::lock_guard lock(observed.mutex);
            if (resumed == BridgeResumePoint::rejected_return) {
                ++observed.rejections;
                observed.changed.notify_all();
            } else if (resumed == point && observed.foreign &&
                       !observed.boundary_observed) {
                observed.boundary_observed = true;
                observed.restored_worker = elio::current_worker_id();
                observed.accessed_before_release = !observed.released;
            }
        });
    // This controller never needs an Elio task, timer or queue admission to
    // release the faults or owner hold, including a guarded red control.
    std::jthread controller([&] {
        try {
            std::unique_lock lock(observed.mutex);
            if (observed.changed.wait_for(lock, std::chrono::seconds(5),
                                         [&] { return observed.foreign; })) {
                observed.rejection_observed = observed.changed.wait_for(
                    lock, std::chrono::seconds(2),
                    [&] { return observed.rejections >= 1; });
                observed.calls_during_pressure = reads.calls.load();
                observed.completions_during_pressure =
                    DeviceTestAccess::completion_count(*dev);
            }
        } catch (...) {
            observed.controller_failed.store(true);
        }
        // Serialize final release with hook arming, even after an exception.
        std::lock_guard lock(observed.mutex);
        observed.accepting_pressure = false;
        observed.released = true;
        pressure.release();
        observed.release_owner.store(true);
        observed.release_owner.notify_all();
    });
    bool primed = false;
    bool completed = false;
    bool stopped = false;
    std::exception_ptr failure;
    elio::runtime::run_config config;
    config.num_threads = 4;
    config.blocking_threads = 1;
    config.shutdown_timeout = std::chrono::milliseconds::max();
    try {
        elio::run([&]() -> elio::coro::task<void> {
            co_await elio::set_affinity(2);
            DeviceTestAccess::start_on(*dev, 0);
            DeviceTestAccess::enqueue_read(*dev);
            primed = co_await await_test_completion(*dev);
            primed = primed && DeviceTestAccess::completed_read(*dev);
            DeviceTestAccess::clear_completions(*dev);
            if (primed) {
                obd::test::WorkerEventFault dispatch(0);
                if (point == BridgeResumePoint::request_retry ||
                    point == BridgeResumePoint::close_retry) dispatch.arm();
                observed.armed.store(true);
                if (point == BridgeResumePoint::close_retry) {
                    co_await dev->stop_async();
                    completed = true;
                } else {
                    DeviceTestAccess::enqueue_read(*dev);
                    completed = co_await await_test_completion(*dev);
                    completed = completed && DeviceTestAccess::completed_read(*dev);
                    co_await dev->stop_async();
                }
            } else {
                co_await dev->stop_async();
            }
            stopped = DeviceTestAccess::running(*dev) == 0;
            if (owner_hold) co_await owner_hold->wait_destroyed_async();
        }, config);
    } catch (...) {
        failure = std::current_exception();
    }
    controller.join();
    pressure.release();
    dev.reset();
    if (failure) std::rethrow_exception(failure);
    CAPTURE(static_cast<int>(point), observed.rejections);
    CHECK(primed);
    CHECK(completed);
    CHECK(stopped);
    CHECK(observed.foreign);
    CHECK_FALSE(observed.controller_failed.load());
    CHECK(observed.held_worker.load() == 0);
    CHECK(observed.rejection_observed);
    CHECK(observed.boundary_observed);
    CHECK(observed.restored_worker == 0);
    CHECK_FALSE(observed.accessed_before_release);
    CHECK(observed.completions_during_pressure == 0);
    CHECK(observed.calls_during_pressure ==
          (point == BridgeResumePoint::source_return ? 2 : 1));
    const unsigned expected_reads = point == BridgeResumePoint::close_retry ? 1 : 2;
    REQUIRE(reads.calls.load() == expected_reads);
    for (unsigned read = 0; read != expected_reads; ++read) {
        CAPTURE(read);
        CHECK(reads.before[read] == 0);
    }
}

TEST_CASE("ublk: a tag retries after its initial child admission is rejected", "[ublk][locality]") {
    using obd::ublk::DeviceTestAccess;
    LocalityState state;
    bool first_failed = false;
    bool retried = false;
    struct RejectionReset {
        ~RejectionReset() {
            elio::runtime::detail::reject_next_spawn_for_test.store(false);
        }
    } reset;
    elio::run([&]() -> elio::coro::task<void> {
        auto dev = DeviceTestAccess::make(std::make_unique<LocalitySource>(state));
        DeviceTestAccess::start_on(*dev, 0);
        while (elio::runtime::worker_thread::current()->io_context()
                   .pending_count() == 0) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        elio::runtime::detail::reject_next_spawn_for_test.store(true);
        DeviceTestAccess::enqueue_read(*dev);
        const bool rejected = co_await await_test_completion(*dev);
        const auto initial = DeviceTestAccess::completions(*dev);
        first_failed = rejected && initial.size() == 1 &&
                       initial.front() == std::pair<uint16_t, int32_t>{0, -EIO};
        elio::runtime::detail::reject_next_spawn_for_test.store(false);
        DeviceTestAccess::clear_completions(*dev);
        DeviceTestAccess::enqueue_read(*dev);
        const bool next = co_await await_test_completion(*dev);
        retried = next && DeviceTestAccess::completed_read(*dev);
        co_await dev->stop_async();
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, one_worker());
    CHECK(first_failed);
    CHECK(retried);
    CHECK(state.calls.load() == 1);
}

TEST_CASE("ublk: tag engine rejects admission after terminal close",
          "[ublk][tag-engine][shutdown]") {
    using obd::ublk::DeviceTestAccess;
    using obd::ublk::IoRequest;
    using obd::ublk::OwnerResultSink;
    using obd::ublk::TagEngine;

    static_assert(std::is_same_v<
                  decltype(std::declval<TagEngine&>().admit(IoRequest{})),
                  bool>);
    static_assert(std::is_same_v<
                  decltype(std::declval<TagEngine&>().begin_close()), void>);
    static_assert(std::is_same_v<
                  decltype(std::declval<TagEngine&>().join_handlers()),
                  elio::coro::task_group::join_awaitable>);

    bool warm = false;
    SECTION("cold tag") {}
    SECTION("warm reusable tag") { warm = true; }

    OwnerSinkObservation observed;
    obd::test::VectorSource source(std::vector<uint8_t>(512, 0x5a));
    auto dev = DeviceTestAccess::make(nullptr, true, 1);
    bool rejected = false;
    unsigned published_before_rejection = 0;

    elio::run([&]() -> elio::coro::task<void> {
        OwnerResultSink sink(&observed, record_owner_result,
                             record_sink_failure, record_owner_notification);
        TagEngine engine(&DeviceTestAccess::queue(*dev), &source, sink,
                         elio::current_worker_id());
        if (warm) {
            bool signaled = engine.admit(
                IoRequest{0, UBLK_IO_OP_READ, 0, 0, 1});
            while (!signaled) {
                co_await elio::time::yield();
                signaled = engine.retry_signal(0);
            }
            for (unsigned attempt = 0;
                 attempt != 5000 &&
                 observed.published.load(std::memory_order_acquire) == 0;
                 ++attempt) {
                co_await elio::time::sleep_for(std::chrono::milliseconds(1));
            }
        }
        engine.begin_close();
        while (!engine.retry_close_signal(0))
            co_await elio::time::yield();
        try {
            co_await engine.join_handlers();
        } catch (...) {
        }
        engine.collect_foreign_results();
        published_before_rejection =
            observed.published.load(std::memory_order_acquire);
        try {
            (void)engine.admit(IoRequest{0, UBLK_IO_OP_READ, 0, 0, 1});
        } catch (const std::logic_error&) {
            rejected = true;
        }
    }, one_worker());

    dev.reset();
    CHECK(rejected);
    CHECK(observed.failed.load(std::memory_order_acquire) == 1);
    CHECK(observed.failure.load(std::memory_order_acquire) == ESHUTDOWN);
    CHECK(observed.published.load(std::memory_order_acquire) ==
          published_before_rejection);
    CHECK(observed.notified.load(std::memory_order_acquire) == 0);
    CHECK(source.reads() == (warm ? 1 : 0));
    CHECK(published_before_rejection == (warm ? 1 : 0));
}

TEST_CASE("ublk: foreign handler destruction publishes only through owner sink",
          "[ublk][tag-engine][locality]") {
    using obd::ublk::BridgeResumePoint;
    using obd::ublk::DeviceTestAccess;
    using obd::ublk::IoRequest;
    using obd::ublk::OwnerResultSink;
    using obd::ublk::TagEngine;

    OwnerSinkObservation observed;
    obd::test::VectorSource source(std::vector<uint8_t>(512, 0x5a));
    auto dev = DeviceTestAccess::make(nullptr, true, 1);
    std::thread::id owner_thread;
    bool notification_preceded_publish = false;
    bool joined_failure = false;

    DeviceTestAccess::set_resume_hooks(
        *dev,
        [&](BridgeResumePoint point) -> elio::coro::task<void> {
            if (point != BridgeResumePoint::tag_wait) co_return;
            co_await elio::set_affinity(1);
            throw std::runtime_error("forced foreign handler teardown");
        },
        {});

    elio::runtime::run_config config;
    config.num_threads = 2;
    config.blocking_threads = 1;
    config.shutdown_timeout = std::chrono::milliseconds::max();
    elio::run([&]() -> elio::coro::task<void> {
        co_await elio::set_affinity(0);
        owner_thread = std::this_thread::get_id();
        OwnerResultSink sink(&observed, record_owner_result,
                             record_sink_failure, record_owner_notification);
        TagEngine engine(&DeviceTestAccess::queue(*dev), &source, sink, 0);

        bool signaled = engine.admit(
            IoRequest{0, UBLK_IO_OP_READ, 0, 0, 1});
        while (!signaled) {
            co_await elio::time::yield();
            signaled = engine.retry_signal(0);
        }
        for (unsigned attempt = 0;
             attempt != 5000 &&
             observed.notified.load(std::memory_order_acquire) == 0;
             ++attempt) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        notification_preceded_publish =
            observed.notified.load(std::memory_order_acquire) == 1 &&
            observed.published.load(std::memory_order_acquire) == 0;
        engine.collect_foreign_results();
        engine.collect_foreign_results();
        engine.begin_close();
        while (!engine.retry_close_signal(0))
            co_await elio::time::yield();
        try {
            co_await engine.join_handlers();
        } catch (const elio::coro::task_group_error&) {
            joined_failure = true;
        }
        engine.collect_foreign_results();
    }, config);

    dev.reset();
    CHECK(notification_preceded_publish);
    CHECK(joined_failure);
    CHECK(observed.notified.load(std::memory_order_acquire) == 1);
    CHECK(observed.notify_worker.load(std::memory_order_acquire) == 1);
    CHECK(observed.notify_thread != owner_thread);
    CHECK(observed.published.load(std::memory_order_acquire) == 1);
    CHECK(observed.publish_worker.load(std::memory_order_acquire) == 0);
    CHECK(observed.publish_thread == owner_thread);
    CHECK(observed.failed.load(std::memory_order_acquire) == 0);
    CHECK((observed.result == obd::ublk::OwnerResult{0, 1, -EIO}));
    CHECK(source.reads() == 0);
}

TEST_CASE("ublk: admission failure still joins the accepted handler",
          "[ublk][tag-engine][shutdown]") {
    using obd::ublk::DeviceTestAccess;
    LocalityState source;
    bool injected = false;
    bool completed = false;
    bool joined_after_completion = false;
    auto dev = DeviceTestAccess::make(std::make_unique<LocalitySource>(source));
    DeviceTestAccess::set_admission_hook(*dev, [&] {
        injected = true;
        throw std::bad_alloc();
    });

    elio::run([&]() -> elio::coro::task<void> {
        DeviceTestAccess::start_on(*dev, 0);
        DeviceTestAccess::enqueue_read(*dev);
        completed = co_await await_test_completion(*dev);
        for (unsigned attempt = 0;
             attempt != 5000 && DeviceTestAccess::running(*dev) != 0;
             ++attempt) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        joined_after_completion =
            DeviceTestAccess::running(*dev) == 0 &&
            DeviceTestAccess::completed_read(*dev);
        co_await dev->stop_async();
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, one_worker());

    CHECK(injected);
    CHECK(completed);
    CHECK(joined_after_completion);
    CHECK(source.calls.load() == 1);
}

TEST_CASE("ublk: tag notification allocation failure retains reads and stop", "[ublk][locality]") {
    using obd::ublk::DeviceTestAccess;
    LocalityState state;
    bool primed = false;
    bool reused = false;
    bool stopped = false;
    unsigned read_failures = 0;
    unsigned stop_failures = 0;
    elio::run([&]() -> elio::coro::task<void> {
        auto dev = DeviceTestAccess::make(std::make_unique<LocalitySource>(state));
        DeviceTestAccess::start_on(*dev, 0);
        DeviceTestAccess::enqueue_read(*dev);
        const bool first = co_await await_test_completion(*dev);
        primed = first && DeviceTestAccess::completed_read(*dev);
        DeviceTestAccess::clear_completions(*dev);
        // On the sole worker, observing a published completion after our timer
        // resumes means the tag has already reached its next unset event wait.
        obd::test::WorkerEventFault fault;
        DeviceTestAccess::enqueue_read(*dev);
        const bool second = co_await await_test_completion(*dev);
        reused = second && DeviceTestAccess::completed_read(*dev);
        read_failures = fault.failures();
        fault.arm();
        co_await dev->stop_async();
        stop_failures = fault.failures() - read_failures;
        stopped = DeviceTestAccess::running(*dev) == 0;
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, one_worker());
    CHECK(primed);
    CHECK(reused);
    CHECK(stopped);
    CHECK(read_failures == 1);
    CHECK(stop_failures == 1);
    CHECK(state.calls.load() == 2);
}

TEST_CASE("ublk: stop drains a request before its tag consumes the wake", "[ublk][shutdown][locality]") {
    using obd::ublk::DeviceTestAccess;
    bool warm = false;
    SECTION("cold tag") {}
    SECTION("reused tag") { warm = true; }
    LocalityState state;
    bool admitted = false;
    bool completed = false;
    bool primed = true;
    bool drained_without_rescue = false;
    bool rescued = false;
    elio::run([&]() -> elio::coro::task<void> {
        auto dev = DeviceTestAccess::make(std::make_unique<LocalitySource>(state));
        DeviceTestAccess::start_on(*dev, 0);
        if (warm) {
            DeviceTestAccess::enqueue_read(*dev);
            primed = co_await await_test_completion(*dev);
            primed = primed && DeviceTestAccess::completed_read(*dev);
            DeviceTestAccess::clear_completions(*dev);
        }
        DeviceTestAccess::stop_after_admission(*dev, admitted);
        // One worker keeps the bridge's admission and close consecutive,
        // before either a new or a reused tag consumes its request event.
        DeviceTestAccess::enqueue_read(*dev);
        for (unsigned attempt = 0; !admitted && attempt != 1000; ++attempt) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        auto stopped = elio::spawn(dev->stop_async());
        for (unsigned attempt = 0; !stopped.is_destroyed() && attempt != 1000;
             ++attempt) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        drained_without_rescue = stopped.is_destroyed();
        if (!drained_without_rescue) {
            rescued = DeviceTestAccess::rescue_closing_tag(*dev);
        }
        co_await stopped.wait_destroyed_async();
        completed = DeviceTestAccess::completed_read(*dev);
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, one_worker());
    CHECK(primed);
    CHECK(admitted);
    CHECK(completed);
    CHECK(drained_without_rescue);
    CHECK_FALSE(rescued);
    CHECK(state.calls.load() == (warm ? 2 : 1));
}

TEST_CASE("ublk: async stop drains an idle bridge before source destruction", "[ublk][shutdown]") {
    using obd::ublk::DeviceTestAccess;
    bool start_before_stop = true;
    SECTION("bridge is parked in eventfd read") { start_before_stop = true; }
    SECTION("bridge has only been scheduled") { start_before_stop = false; }
    ReadState state;
    elio::run([&]() -> elio::coro::task<void> {
        auto dev = DeviceTestAccess::make(std::make_unique<CompletingSource>(state));
        DeviceTestAccess::start(*dev);
        if (start_before_stop) {
            // With one worker the only outstanding IO is the real bridge read.
            while (elio::runtime::worker_thread::current()->io_context()
                       .pending_count() == 0) {
                co_await elio::time::sleep_for(std::chrono::milliseconds(1));
            }
        }
        co_await dev->stop_async();
        CHECK(DeviceTestAccess::running(*dev) == 0);
        CHECK_FALSE(state.destroyed.load());
        co_await dev->stop_async();  // sequential idempotence
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, one_worker());
    REQUIRE(state.destroyed.load());
    REQUIRE(state.destroyed_off_worker.load());
}

TEST_CASE("ublk: async stop lets a source finish on the sole blocking thread", "[ublk][shutdown]") {
    using obd::ublk::DeviceTestAccess;
    bool inject_failure = false;
    SECTION("normal observer setup") { inject_failure = false; }
    SECTION("native observer allocation fails") { inject_failure = true; }
    std::optional<obd::test::DestroyedWaitFault> fault;
    if (inject_failure) fault.emplace();
    ReadState state;
    elio::run([&]() -> elio::coro::task<void> {
        auto dev = DeviceTestAccess::make(std::make_unique<CompletingSource>(state));
        DeviceTestAccess::start(*dev, true);
        while (!state.entered.load()) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        auto stop = elio::spawn(dev->stop_async());
        while (!DeviceTestAccess::stopping(*dev)) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK_FALSE(stop.is_ready());
        CHECK_FALSE(state.destroyed.load());
        CHECK_FALSE(state.finished.load());
        state.release.store(true);
        co_await stop;
        CHECK(state.finished.load());
        CHECK(state.pool_ran.load());
        CHECK(DeviceTestAccess::running(*dev) == 0);
        CHECK(DeviceTestAccess::completed_read(*dev));
        CHECK_FALSE(state.destroyed.load());
        co_await stop.wait_destroyed_async();
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, one_worker());
    REQUIRE(state.destroyed.load());
    REQUIRE(state.destroyed_off_worker.load());
    if (fault) CHECK(fault->failures() == 1);
}

TEST_CASE("ublk: async stop handles a partially initialized device", "[ublk][shutdown]") {
    using obd::ublk::DeviceTestAccess;
    ReadState state;
    elio::run([&]() -> elio::coro::task<void> {
        auto dev = DeviceTestAccess::make(std::make_unique<CompletingSource>(state), false);
        co_await dev->stop_async();
        CHECK(DeviceTestAccess::running(*dev) == 0);
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, one_worker());
    REQUIRE(state.destroyed.load());
    REQUIRE(state.destroyed_off_worker.load());
}

TEST_CASE("ublk: task group drains every admitted tag including source failures", "[ublk][shutdown]") {
    using obd::ublk::DeviceTestAccess;
    bool throw_second = false;
    SECTION("both reads complete normally") { throw_second = false; }
    SECTION("one source read throws") { throw_second = true; }
    TagReadState state;
    std::deque<std::pair<uint16_t, int32_t>> completions;
    bool stop_was_pending = false;
    bool source_was_retained = false;
    elio::run([&]() -> elio::coro::task<void> {
        auto dev = DeviceTestAccess::make(
            std::make_unique<TagReadSource>(state, throw_second), true, 2);
        DeviceTestAccess::start(*dev, true, 2);
        for (int i = 0; i < 5000 && state.entered.load() < 2; ++i) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        auto stop = elio::spawn(dev->stop_async());
        while (!DeviceTestAccess::stopping(*dev)) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        stop_was_pending = !stop.is_ready();
        source_was_retained = !state.destroyed.load();
        state.release.set();
        co_await stop;
        co_await stop.wait_destroyed_async();
        completions = DeviceTestAccess::completions(*dev);
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, one_worker());
    CHECK(state.entered.load() == 2);
    CHECK(state.finished.load() == 2);
    CHECK(stop_was_pending);
    CHECK(source_was_retained);
    REQUIRE(completions.size() == 2);
    for (const auto& [tag, result] : completions) {
        CHECK(result == (tag == 1 && throw_second ? -EIO : 512));
    }
    CHECK(state.destroyed.load());
}

TEST_CASE("ublk: reused tag preserves short reads and source error conversion",
          "[ublk][shutdown][dispatch]") {
    class SequenceSource final : public obd::source::BlobSource {
    public:
        elio::coro::task<ssize_t> pread(void* buffer, size_t length,
                                      uint64_t) override {
            const unsigned current = calls++;
            // Exercise a real suspension inside the operation being dispatched.
            co_await elio::time::sleep_for(std::chrono::nanoseconds(1));
            if (current == 1) co_return -ENODATA;
            if (current == 2) throw std::runtime_error("dispatch source error");
            const size_t produced = current == 0 ? 17 : length;
            std::memset(buffer, current == 0 ? 0x31 : 0x72, produced);
            co_return static_cast<ssize_t>(produced);
        }
        uint64_t size() const noexcept override { return 4096; }
        std::string_view label() const noexcept override { return "dispatch-sequence"; }
        unsigned calls = 0;
    };
    using obd::ublk::DeviceTestAccess;
    std::array<int32_t, 4> results{};
    std::array<std::array<uint8_t, 512>, 4> bytes{};
    unsigned calls = 0;
    unsigned completed = 0;
    elio::run([&]() -> elio::coro::task<void> {
        auto source = std::make_unique<SequenceSource>();
        auto* observed = source.get();
        auto dev = DeviceTestAccess::make(std::move(source));
        DeviceTestAccess::start(*dev);
        for (unsigned iteration = 0; iteration < results.size(); ++iteration) {
            std::memset(DeviceTestAccess::queue(*dev).io_buf(0), 0xa5, 512);
            DeviceTestAccess::enqueue_read(*dev);
            for (unsigned wait = 0; wait < 5000 &&
                 DeviceTestAccess::completion_count(*dev) == 0; ++wait)
                co_await elio::time::sleep_for(std::chrono::milliseconds(1));
            const auto completions = DeviceTestAccess::completions(*dev);
            if (completions.size() != 1) break;
            results[iteration] = completions.front().second;
            std::memcpy(bytes[iteration].data(),
                        DeviceTestAccess::queue(*dev).io_buf(0), 512);
            DeviceTestAccess::clear_completions(*dev);
            ++completed;
        }
        co_await dev->stop_async();
        calls = observed->calls;
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, one_worker());
    REQUIRE(completed == 4);
    CHECK(calls == 4);
    CHECK(results == std::array<int32_t, 4>{512, -ENODATA, -EIO, 512});
    for (size_t index = 0; index < 512; ++index) {
        CHECK(bytes[0][index] == (index < 17 ? 0x31 : 0));
        CHECK(bytes[3][index] == 0x72);
    }
}

TEST_CASE("ublk: a cancelled task group still completes a skipped tag", "[ublk][shutdown]") {
    using obd::ublk::DeviceTestAccess;
    ReadState state;
    std::deque<std::pair<uint16_t, int32_t>> completions;
    elio::run([&]() -> elio::coro::task<void> {
        auto dev = DeviceTestAccess::make(std::make_unique<CompletingSource>(state));
        DeviceTestAccess::start(*dev);
        while (elio::runtime::worker_thread::current()->io_context()
                   .pending_count() == 0) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        DeviceTestAccess::cancel_bridge(*dev);
        DeviceTestAccess::enqueue_read(*dev);
        for (int i = 0; i < 5000 && DeviceTestAccess::completions(*dev).empty(); ++i) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        // Failure cleanup also permits a mistakenly-started source to depart.
        state.release.store(true);
        co_await dev->stop_async();
        completions = DeviceTestAccess::completions(*dev);
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, one_worker());
    CHECK_FALSE(state.entered.load());
    REQUIRE(completions.size() == 1);
    CHECK(completions.front().first == 0);
    CHECK(completions.front().second == -EIO);
    CHECK(state.destroyed.load());
}

TEST_CASE("ublk: failed completion publication wakes the queue before stop", "[ublk][shutdown]") {
    using obd::ublk::DeviceTestAccess;
    enum class Failure { SkippedChild, Completion, External };
    Failure failure = Failure::SkippedChild;
    SECTION("skipped child fallback cannot enqueue") { failure = Failure::SkippedChild; }
    SECTION("normal completion and fallback cannot enqueue") { failure = Failure::Completion; }
    SECTION("external failure publication") { failure = Failure::External; }
    ReadState state;
    std::atomic<unsigned> enqueue_attempts{0};
    std::atomic<bool> waiter_entered{false};
    std::atomic<bool> waiter_finished{false};
    std::atomic<bool> waiter_woken{false};
    bool waited_before_failure = false;
    bool bridge_waiting_before_failure = false;
    bool woke_before_stop = false;
    bool failed_before_stop = false;
    int queue_error = 0;
    std::deque<std::pair<uint16_t, int32_t>> completions;
    elio::run([&]() -> elio::coro::task<void> {
        auto dev = DeviceTestAccess::make(std::make_unique<CompletingSource>(state));
        if (failure != Failure::External) {
            DeviceTestAccess::set_completion_enqueue_hook(*dev, [&] {
                enqueue_attempts.fetch_add(1);
                throw std::bad_alloc();
            });
        }
        const int done_efd = DeviceTestAccess::done_efd(*dev);
        // Observe the production wake descriptor from a plain queue-side
        // thread. Kernel registration/ring ownership remains bypassed here.
        std::jthread waiter([&] {
            pollfd descriptor{done_efd, POLLIN, 0};
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(5);
            waiter_entered.store(true);
            for (;;) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                if (remaining.count() <= 0) break;
                const int result = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
                if (result < 0 && errno == EINTR) continue;
                waiter_woken.store(result > 0 && (descriptor.revents & POLLIN) != 0);
                break;
            }
            waiter_finished.store(true);
        });
        DeviceTestAccess::start(*dev);
        for (int i = 0; i < 5000 &&
             (!waiter_entered.load() ||
              elio::runtime::worker_thread::current()->io_context().pending_count() == 0); ++i) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        waited_before_failure = waiter_entered.load() && !waiter_finished.load();
        bridge_waiting_before_failure =
            elio::runtime::worker_thread::current()->io_context().pending_count() != 0;
        state.release.store(true);  // also releases a mistakenly-started read
        if (failure == Failure::External) {
            // Covers the shared publication used by deferred bridge-loop
            // failure without injecting a synthetic exception into that loop.
            DeviceTestAccess::queue(*dev).fail(EIO);
        } else {
            if (failure == Failure::SkippedChild) DeviceTestAccess::cancel_bridge(*dev);
            DeviceTestAccess::enqueue_read(*dev);
        }
        for (int i = 0; i < 5000 && !waiter_finished.load(); ++i) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        // Snapshot before stop's own wakeup, which would hide the regression.
        woke_before_stop = waiter_finished.load() && waiter_woken.load();
        failed_before_stop = DeviceTestAccess::queue(*dev).failed();
        queue_error = DeviceTestAccess::queue(*dev).failure();
        co_await dev->stop_async();
        completions = DeviceTestAccess::completions(*dev);
        co_await elio::spawn_blocking([&] {
            waiter.join();
            dev.reset();
        });
    }, one_worker());
    CAPTURE(enqueue_attempts.load(), waiter_entered.load(), waiter_finished.load(), queue_error);
    CHECK(waited_before_failure);
    CHECK(bridge_waiting_before_failure);
    CHECK(woke_before_stop);
    CHECK(failed_before_stop);
    CHECK(queue_error == EIO);
    CHECK(completions.empty());
    CHECK(enqueue_attempts.load() == (failure == Failure::External ? 0u :
                                    failure == Failure::SkippedChild ? 1u : 2u));
    CHECK(state.entered.load() == (failure == Failure::Completion));
    CHECK(state.finished.load() == (failure == Failure::Completion));
    CHECK(state.destroyed.load());
    CHECK(state.destroyed_off_worker.load());
}

TEST_CASE("ublk: control task releases its device owner before final destruction", "[ublk][shutdown]") {
    using obd::ublk::DeviceTestAccess;
    ReadState state;
    int sockets[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    elio::run([&]() -> elio::coro::task<void> {
        std::shared_ptr<obd::ublk::Device> dev =
            DeviceTestAccess::make(std::make_unique<CompletingSource>(state));
        DeviceTestAccess::start(*dev);
        obd::supervisor::DeviceControlHooks hooks;
        hooks.resize.current_size = [dev] { return dev->size_bytes(); };
        hooks.resize.apply_resize = [dev](uint64_t) { return dev->size_bytes(); };
        auto channel = std::make_shared<obd::supervisor::ControlChannelWriter>(sockets[0]);
        auto control = elio::spawn(obd::supervisor::run_device_control,
                                  channel, nullptr, std::move(hooks));
        co_await dev->stop_async();
        ::shutdown(sockets[1], SHUT_RDWR);
        co_await control;
        co_await control.wait_destroyed_async();
        CHECK(dev.use_count() == 1);
        CHECK_FALSE(state.destroyed.load());
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, one_worker());
    ::close(sockets[0]);
    ::close(sockets[1]);
    REQUIRE(state.destroyed.load());
    REQUIRE(state.destroyed_off_worker.load());
}

TEST_CASE("ublk: failed startup drains sources before propagating its original error", "[ublk][shutdown]") {
    using obd::ublk::DeviceTestAccess;
    bool bridge = false;
    SECTION("before queue setup") { bridge = false; }
    SECTION("after bridge registration") { bridge = true; }
    ReadState state;
    const auto original = std::make_exception_ptr(
        std::system_error(EIO, std::generic_category(), "setup failed"));
    std::exception_ptr observed;
    elio::run([&]() -> elio::coro::task<void> {
        auto dev = DeviceTestAccess::make(
            std::make_unique<CompletingSource>(state), bridge);
        if (bridge) DeviceTestAccess::start(*dev);
        obd::ublk::StartupControl startup(1);
        try {
            (void)co_await DeviceTestAccess::fail_startup(*dev, startup, original);
        } catch (...) {
            observed = std::current_exception();
        }
        const auto status = startup.snapshot();
        CHECK(status.phase == obd::ublk::StartupPhase::released);
        CHECK(DeviceTestAccess::running(*dev) == 0);
        CHECK_FALSE(state.destroyed.load());
        co_await elio::spawn_blocking([&] { dev.reset(); });
        CHECK(state.destroyed.load());
    }, one_worker());
    REQUIRE(observed == original);
    REQUIRE(state.destroyed_off_worker.load());
}

TEST_CASE("ublk: startup failure keeps the admitted cleanup callback without copying",
          "[ublk][shutdown]") {
    using namespace obd::ublk;
    ReadState state;
    StartupControl startup(23);
    // A release accepted before create prevents any kernel control open.
    // The public API still runs start_impl and its actual failure coordinator.
    REQUIRE(startup.request_release(23, 1) == ReleaseRequestResult::accepted);
    unsigned calls = 0, copies = 0;
    Device::FailureCleanup cleanup{CleanupWithoutCopies(calls, copies)};
    bool released = false;
    elio::run([&]() -> elio::coro::task<void> {
        DeviceParams params;
        params.dev_sectors = 8;
        try {
            (void)co_await Device::create(params,
                std::make_unique<CompletingSource>(state), startup, std::move(cleanup));
        } catch (const StartupReleased&) { released = true; }
    }, one_worker());
    CHECK(released);
    CHECK(calls == 1);
    CHECK(copies == 0);
    CHECK(startup.snapshot().phase == StartupPhase::released);
    CHECK(state.destroyed.load());
}

TEST_CASE("ublk: retained recovery waits for source drain and explicit release",
          "[ublk][shutdown]") {
    using namespace obd::ublk;
    using namespace std::chrono_literals;
    bool early_release = false;
    bool refuse = false;
    SECTION("release admitted while source still borrows the buffer") { early_release = true; }
    SECTION("drained source does not authorize release") {}
    SECTION("refused STOP preserves the owner until a new release request") { refuse = true; }
    ReadState state;
    LifecycleTransport transport;
    transport.source = &state;
    transport.refuse_first_stop = refuse;
    StartupControl startup(17);
    bool released = false;
    unsigned cleanup_attempts = 0;
    elio::run([&]() -> elio::coro::task<void> {
        auto dev = DeviceTestAccess::make(std::make_unique<CompletingSource>(state));
        DeviceTestAccess::recovering(*dev, startup, transport);
        transport.queue = &DeviceTestAccess::queue(*dev);
        transport.device = dev.get();
        DeviceTestAccess::start(*dev, true);
        while (!state.entered.load()) co_await elio::time::sleep_for(1ms);
        auto failure = std::make_exception_ptr(
            std::system_error(EIO, std::generic_category(), "failed replacement"));
        auto cleanup = [&]() -> elio::coro::task<void> {
            // Exercise failure of ancillary fill parking without losing the
            // retained Device/source or returning to the caller prematurely.
            if (++cleanup_attempts < 3) throw std::bad_alloc();
            co_return;
        };
        auto failed = elio::spawn(DeviceTestAccess::fail_startup(
            *dev, startup, failure, true, cleanup));
        while (!DeviceTestAccess::stopping(*dev)) co_await elio::time::sleep_for(1ms);
        CHECK(startup.snapshot().phase == StartupPhase::retained);
        CHECK_FALSE(failed.is_ready());
        CHECK_FALSE(state.finished.load());
        CHECK(transport.stops.load() == 0);
        if (early_release) {
            CHECK(startup.request_release(17, 1) == ReleaseRequestResult::accepted);
        }
        state.release.store(true);
        while (cleanup_attempts < 3) co_await elio::time::sleep_for(1ms);
        if (!early_release) {
            CHECK(transport.stops.load() == 0);
            CHECK_FALSE(failed.is_ready());
            CHECK(startup.request_release(17, 1) == ReleaseRequestResult::accepted);
        }
        if (refuse) {
            while (startup.snapshot().release != StartupRelease::refused)
                co_await elio::time::sleep_for(1ms);
            CHECK(transport.stops.load() == 1);
            CHECK(transport.deletions.load() == 0);
            CHECK(transport.queue->cdev_fd() >= 0);
            CHECK_FALSE(state.destroyed.load());
            CHECK_FALSE(failed.is_ready());
            CHECK(startup.request_release(17, 2) == ReleaseRequestResult::accepted);
        }
        try { (void)co_await failed; }
        catch (const StartupReleased&) { released = true; }
        co_await failed.wait_destroyed_async();
        const auto final = startup.snapshot();
        CHECK(final.phase == StartupPhase::released);
        CHECK(final.release == StartupRelease::released);
        CHECK(final.release_sequence == (refuse ? 2 : 1));
        CHECK(DeviceTestAccess::running(*dev) == 0);
        CHECK_FALSE(state.destroyed.load());
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, one_worker());
    CHECK(released);
    CHECK(transport.stops.load() == (refuse ? 2 : 1));
    CHECK(transport.deletions.load() == 1);
    CHECK(transport.source_finished_at_stop.load());
    CHECK(transport.native_frames_gone_at_stop.load());
    CHECK(transport.cdev_closed_at_delete.load());
    CHECK(state.destroyed.load());
    CHECK(state.destroyed_off_worker.load());
}

TEST_CASE("ublk: retained release waits for every queue to issue and retire originals",
          "[ublk][shutdown]") {
    using namespace obd::ublk;
    using namespace std::chrono_literals;
    ReadState state;
    LifecycleTransport transport;
    StartupControl startup(29);
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> paused{false};
    bool resume = false, resumed_by_test = false;
    elio::run([&]() -> elio::coro::task<void> {
        auto dev = DeviceTestAccess::make(std::make_unique<CompletingSource>(state), false);
        DeviceTestAccess::recovering(*dev, startup, transport);
        DeviceTestAccess::start_rejecting_queues(*dev, [&] {
            std::unique_lock lock(mutex);
            paused.store(true);
            resumed_by_test = cv.wait_for(lock, 5s, [&] { return resume; });
        });
        transport.queue = &DeviceTestAccess::queue_at(*dev, 0);
        transport.device = dev.get();
        auto& second = DeviceTestAccess::queue_at(*dev, 1);
        while (!paused.load() || !transport.queue->initial_fetches_issued())
            co_await elio::time::sleep_for(1ms);
        auto failure = std::make_exception_ptr(
            std::system_error(EIO, std::generic_category(), "partial queue publication"));
        auto failed = elio::spawn(DeviceTestAccess::fail_startup(*dev, startup, failure, true));
        while (startup.snapshot().phase != StartupPhase::retained)
            co_await elio::time::sleep_for(1ms);
        CHECK(startup.request_release(29, 1) == ReleaseRequestResult::accepted);
        while (startup.snapshot().phase != StartupPhase::releasing)
            co_await elio::time::sleep_for(1ms);
        CHECK_FALSE(second.initial_fetches_issued());
        CHECK_FALSE(failed.is_ready());
        CHECK(transport.stops.load() == 0);
        CHECK(transport.deletions.load() == 0);
        {
            std::lock_guard lock(mutex);
            resume = true;
            cv.notify_one();
        }
        bool released = false;
        try { (void)co_await failed; }
        catch (const StartupReleased&) { released = true; }
        co_await failed.wait_destroyed_async();
        CHECK(released);
        for (size_t i = 0; i != 2; ++i) {
            const auto& queue = DeviceTestAccess::queue_at(*dev, i);
            const auto ledger = queue.retirement();
            CHECK(queue.initial_fetches_issued());
            CHECK(queue.owner_exited());
            CHECK(queue.cdev_fd() == -1);
            CHECK(ledger.known);
            CHECK(ledger.complete);
            // The raw ledger also counts the completion-eventfd poll if this
            // owner armed it before observing failure. Verify both tag slots
            // and every published cookie rather than assuming only FETCH exists.
            CHECK(DeviceTestAccess::all_tags_retired(queue));
            CHECK(ledger.published >= 2);
            CHECK(ledger.retired == ledger.published);
        }
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, one_worker());
    CHECK(resumed_by_test);
    CHECK(transport.stops.load() == 1);
    CHECK(transport.deletions.load() == 1);
    CHECK(transport.cdev_closed_at_delete.load());
}

TEST_CASE("ublk: original END completion decides recovery adoption",
          "[ublk][shutdown]") {
    using namespace obd::ublk;
    bool success = true;
    SECTION("original success adopts before returning from Ctrl") {}
    SECTION("original failure leaves the old device unadopted") { success = false; }
    ReadState state;
    LifecycleTransport transport;
    transport.end_result = success ? 0 : -EIO;
    StartupControl startup(19);
    auto dev = DeviceTestAccess::make(std::make_unique<CompletingSource>(state), false);
    DeviceTestAccess::recovering(*dev, startup, transport);
    bool threw = false;
    try { DeviceTestAccess::end_recovery(*dev); }
    catch (const std::system_error&) { threw = true; }
    CHECK(threw == !success);
    CHECK(DeviceTestAccess::owns_id(*dev) == success);
    CHECK(dev->started() == success);
    CHECK(startup.snapshot().original_known);
    CHECK(startup.snapshot().original_result == transport.end_result);
    // Even the failed-original branch needs explicit release: the fixture
    // represents a recovery that has already published its queue owners.
    CHECK(startup.request_release(19, 1) == ReleaseRequestResult::accepted);
    elio::run([&]() -> elio::coro::task<void> {
        try {
            (void)co_await DeviceTestAccess::fail_startup(*dev, startup, {}, true);
        } catch (const StartupReleased&) {}
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, one_worker());
    CHECK(transport.stops.load() == 1);
    CHECK(transport.deletions.load() == 1);
}

TEST_CASE("ublk: late successful startup produces an accepted ready handoff",
          "[ublk][shutdown][startup-handoff]") {
    using namespace obd::ublk;
    using namespace obd::supervisor;
    using namespace std::chrono_literals;
    bool recovery = false;
    bool operator_release = false;
    SECTION("original START succeeds after deadline cancellation") {}
    SECTION("original END succeeds after deadline cancellation") { recovery = true; }
    SECTION("operator release wins the START handoff") { operator_release = true; }
    SECTION("operator release wins the END handoff") {
        recovery = true;
        operator_release = true;
    }

    ChildSpec spec;
    spec.id = "late-startup-success";
    // This process exits by itself even when a regression keeps protection
    // installed. The test owns and collects it through Child, never raw waitpid.
    spec.device_bin = "/bin/true";
    spec.startup_generation = 19;
    auto child = Child::spawn(spec);
    REQUIRE(child->protect_startup(19));

    ReadState state;
    LifecycleTransport transport;
    StartupControl startup(19);
    transport.delay_start_success = true;
    transport.startup = &startup;
    bool handed_off = false;
    bool protected_after_handoff = false;
    bool decoded_ready = false;
    bool accepted_ready = false;
    bool released_on_request = false;
    std::exception_ptr failure;
    elio::run([&]() -> elio::coro::task<void> {
        auto dev = DeviceTestAccess::make(std::make_unique<CompletingSource>(state));
        DeviceTestAccess::recovering(*dev, startup, transport);
        transport.queue = &DeviceTestAccess::queue(*dev);
        transport.device = dev.get();
        DeviceTestAccess::start(*dev);
        try {
            const auto bridge_deadline = std::chrono::steady_clock::now() + 2s;
            while (!DeviceTestAccess::queue(*dev).bridge_ready() &&
                   std::chrono::steady_clock::now() < bridge_deadline)
                co_await elio::time::sleep_for(1ms);
            CHECK(DeviceTestAccess::queue(*dev).bridge_ready());
            // The fixture's default deadline is already expired. Delaying the
            // original CQE forces Ctrl through Device::cancel_control and its
            // progress observer; no timeout/reason fields are fabricated here.
            co_await elio::spawn_blocking([&] {
                if (recovery) DeviceTestAccess::end_recovery(*dev);
                else DeviceTestAccess::start_device(*dev);
            });
            const auto before_handoff = startup.snapshot();
            CHECK(before_handoff.reason_errno == ETIMEDOUT);
            CHECK(before_handoff.phase == StartupPhase::retained);
            CHECK(before_handoff.original_known);
            CHECK(before_handoff.original_result == 0);
            CHECK(before_handoff.cancel_known);
            CHECK(before_handoff.cancel_result == -ENOENT);
            CHECK_FALSE(before_handoff.control_pending);
            CHECK(DeviceTestAccess::owns_id(*dev));
            CHECK(dev->started());
            CHECK(DeviceTestAccess::running(*dev) == 1);
            const auto pending = parse_device_status(make_device_status(DeviceStatus{
                "failed", "", "deadline cancellation intent",
                obd::cmd::startup_status(before_handoff)}));
            CHECK(pending.has_value());
            if (pending && pending->startup_owner)
                CHECK(child->observe_startup(*pending->startup_owner, false));
            CHECK(child->startup_protected());

            if (operator_release) {
                const auto sequence = child->admit_startup_release();
                CHECK(sequence == 1);
                CHECK(startup.request_release(19, 1) == ReleaseRequestResult::accepted);
            }
            handed_off = DeviceTestAccess::publish_ready(*dev);
            const auto after_handoff = startup.snapshot();
            if (handed_off) {
                CHECK(after_handoff.reason_errno == 0);
                CHECK(after_handoff.phase == StartupPhase::ready);
                CHECK_FALSE(after_handoff.control_pending);
                CHECK(after_handoff.original_known);
                CHECK(after_handoff.original_result == 0);
                CHECK(after_handoff.cancel_known);
                CHECK(after_handoff.cancel_result == -ENOENT);
                CHECK(startup.request_release(19, 1) == ReleaseRequestResult::not_startup);
                // Exercise the exact codec consumed by the supervisor, then
                // actual Child admission. Parsing alone would miss protection.
                const auto ready = parse_device_status(make_device_status(DeviceStatus{
                    "ready", dev->bdev_path(), "", obd::cmd::startup_status(after_handoff)}));
                decoded_ready = ready && ready->startup_owner.has_value();
                if (decoded_ready) {
                    accepted_ready = child->observe_startup(*ready->startup_owner, true);
                    if (accepted_ready)
                        child->update_status({ready->state, ready->device, ready->error, -1});
                }
            } else {
                CHECK(after_handoff.phase != StartupPhase::ready);
            }
            protected_after_handoff = child->startup_protected();
            if (operator_release) {
                try {
                    (void)co_await DeviceTestAccess::fail_startup(
                        *dev, startup, {}, recovery);
                } catch (const StartupReleased&) { released_on_request = true; }
                const auto released = parse_device_status(make_device_status(DeviceStatus{
                    "stopped", "", "", obd::cmd::startup_status(startup.snapshot())}));
                CHECK(released.has_value());
                if (released && released->startup_owner)
                    CHECK(child->observe_startup(*released->startup_owner, false));
            }
        } catch (...) { failure = std::current_exception(); }
        // Successful original START/END already committed ownership. Both the
        // ordinary ready path and an assertion/error path can safely drain it.
        co_await dev->stop_async();
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, one_worker());

    CHECK(transport.cancellations == 1);
    CHECK(transport.cancellation_target_matches);
    CHECK(transport.timed_out_before_success);
    CHECK(handed_off == !operator_release);
    CHECK(protected_after_handoff == operator_release);
    CHECK(decoded_ready == !operator_release);
    CHECK(accepted_ready == !operator_release);
    CHECK(released_on_request == operator_release);
    CHECK_FALSE(child->startup_protected());
    if (!operator_release) CHECK(child->status().state == "ready");
    CHECK(transport.stops.load() == 1);
    CHECK(transport.deletions.load() == 1);
    CHECK(transport.native_frames_gone_at_stop.load());
    CHECK(transport.cdev_closed_at_delete.load());
    CHECK(state.destroyed.load());

    // An accepted ready record permits the ordinary terminate/reap path. The
    // independent /bin/true fixture guarantees cleanup even under the old bug.
    child->terminate();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!child->reaped() && std::chrono::steady_clock::now() < deadline) {
        (void)child->try_reap();
        if (!child->reaped()) std::this_thread::sleep_for(1ms);
    }
    CHECK(child->reaped());
    if (failure) std::rethrow_exception(failure);
}

TEST_CASE("ublk: blocking stop waits for a slow source to finish", "[ublk][shutdown]") {
    using obd::ublk::DeviceTestAccess;
    ReadState state;
    std::atomic<bool> stop_returned{false};
    std::atomic<bool> finished_at_return{false};
    elio::run([&]() -> elio::coro::task<void> {
        auto dev = DeviceTestAccess::make(std::make_unique<CompletingSource>(state));
        DeviceTestAccess::start(*dev, true);
        while (!state.entered.load()) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        // A dedicated caller thread leaves both source executors available.
        std::thread stopper([&] {
            dev->stop();
            finished_at_return.store(state.finished.load());
            stop_returned.store(true);
        });
        // Exercise a normal read slower than the former five-second drain
        // allowance. Keep all owners until the read actually completes, even
        // if an incorrect stop returns early; assert the recorded ordering.
        co_await elio::time::sleep_for(std::chrono::seconds(8));
        state.release.store(true);
        while (!stop_returned.load()) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        co_await elio::spawn_blocking([&] { stopper.join(); });
        co_await dev->stop_async();
        CHECK(stop_returned.load());
        CHECK(finished_at_return.load());
        CHECK(DeviceTestAccess::completed_read(*dev));
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }, one_worker());
    REQUIRE(state.destroyed.load());
    REQUIRE(state.destroyed_off_worker.load());
}
