// Ordinary lifecycle tests use real Device/bridge code and eventfds. Only
// /dev/ublkc registration, its command mapping and queue ring are bypassed.
#include "supervisor/device_control.hpp"
#include "ublk/device.hpp"
#include "ublk/elio_bridge.hpp"

#include "../support.hpp"
#include "../runtime_faults.hpp"

#include <elio/runtime/spawn.hpp>
#include <elio/runtime/spawn_blocking.hpp>
#include <elio/time/timer.hpp>
#include <elio/sync/event.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sys/eventfd.h>
#include <sys/socket.h>

#include <cstdlib>
#include <cstring>
#include <exception>
#include <system_error>
#include <thread>

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

    static elio::coro::task<void> cleanup_failed(
        std::unique_ptr<Device> dev, std::exception_ptr failure) {
        co_await Device::cleanup_failed(std::move(dev), failure);
    }

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

}  // namespace

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

TEST_CASE("ublk: failed setup drains partial state before rethrowing its error", "[ublk][shutdown]") {
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
        try {
            // The same cleanup routine is used by create and attach catches.
            co_await DeviceTestAccess::cleanup_failed(std::move(dev), original);
        } catch (...) {
            observed = std::current_exception();
        }
        CHECK(state.destroyed.load());
    }, one_worker());
    REQUIRE(observed == original);
    REQUIRE(state.destroyed_off_worker.load());
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
