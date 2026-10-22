// Public startup coordinator coverage. Control CQEs are scripted; queue rings
// are real and submit URING_CMD to an eventfd that rejects it. No ublk device,
// fake delivered request, or manually assigned lifecycle state is involved.
#include "ublk/device.hpp"
#include "../support.hpp"

#include <elio/runtime/spawn.hpp>
#include <elio/runtime/spawn_blocking.hpp>
#include <catch2/catch_test_macros.hpp>

#include <sys/eventfd.h>
#include <fcntl.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <map>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace obd::ublk {
struct StartupCoordinatorTestAccess {
    static void open(Queue& q, int shared) {
        q.ublkc_fd_ = shared < 0 ? ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)
                                : ::fcntl(shared, F_DUPFD_CLOEXEC, 0);
        q.elio_efd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        q.done_efd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        q.io_bufs_ = static_cast<uint8_t*>(std::calloc(q.depth_, q.max_io_buf_bytes_));
        if (q.ublkc_fd_ < 0 || q.elio_efd_ < 0 || q.done_efd_ < 0 || !q.io_bufs_)
            throw std::runtime_error("startup queue stand-in allocation failed");
    }
};
}

namespace {
using namespace std::chrono_literals;
using namespace obd::ublk;
using Kind = detail::StartupTestKind;

// Only native queue/control threads park here. Timeout opens this synthetic
// checkpoint and invalidates the test; it never asserts production retirement.
struct Gate {
    std::atomic<bool> entered{false}, escaped{false};
    std::mutex mutex;
    std::condition_variable cv;
    bool open = false;
    void wait() {
        std::unique_lock lock(mutex);
        entered.store(true);
        if (!cv.wait_for(lock, 5s, [&] { return open; })) escaped.store(true);
    }
    void release() {
        std::lock_guard lock(mutex);
        open = true;
        cv.notify_all();
    }
};

template<class Predicate>
elio::coro::task<bool> until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) co_return false;
        co_await elio::time::sleep_for(1ms);
    }
    co_return true;
}

struct SourceState {
    std::atomic<bool> entered{false}, pool_ran{false}, returned{false}, destroyed{false};
    std::atomic<unsigned> owners{0}, bridges{0}, closed{0}, controls{0};
    unsigned expected_queues = 1;
    std::atomic<bool> destroyed_after_retirement{false};
};
class PoolSource final : public obd::source::BlobSource {
public:
    explicit PoolSource(SourceState& state) : state_(state) {}
    ~PoolSource() override {
        state_.destroyed_after_retirement.store(
            state_.owners.load() == state_.expected_queues &&
            state_.bridges.load() == state_.expected_queues &&
            state_.closed.load() == state_.expected_queues && state_.controls.load() == 1);
        state_.destroyed.store(true);
    }
    elio::coro::task<ssize_t> pread(void* buffer, size_t count, uint64_t) override {
        state_.entered.store(true);
        auto work = [&] {
            std::memset(buffer, 0x5a, count);
            state_.pool_ran.store(true);
        };
        co_await elio::spawn_blocking(std::move(work));
        state_.returned.store(true);
        co_return static_cast<ssize_t>(count);
    }
    uint64_t size() const noexcept override { return 65536; }
    std::string_view label() const noexcept override { return "startup-pool-dependency"; }
private:
    SourceState& state_;
};

elio::coro::task<void> read_device_source(PoolSource* source) {
    std::array<unsigned char, 512> buffer{};
    const auto bytes = co_await source->pread(buffer.data(), buffer.size(), 0);
    CHECK(bytes == 512);
    CHECK(buffer.front() == 0x5a);
    CHECK(buffer.back() == 0x5a);
}

// One cancel-first/original-held script serves all admitted START/END cases.
// Initial ADD/SET_PARAMS and START_RECOVERY/GET_PARAMS really traverse Ctrl.
// Only the transport is substituted; Device installs its production observer.
class Script final : public detail::CtrlTransport {
public:
    using Command = std::pair<uint32_t, uint64_t>;
    std::atomic<bool> admitted{false}, release_original{false}, escaped{false};
    std::atomic<unsigned> starts{0}, stops{0}, deletes{0}, cancels{0};
    std::atomic<uint64_t> original_cookie{0}, cancel_cookie{0}, cancel_target{0};
    std::atomic<bool> source_required{false};
    SourceState* source = nullptr;
    std::atomic<bool> source_done_at_original{false};

    io_uring_sqe* get_sqe() noexcept override { prepared_ = true; return sqe_.data(); }
    unsigned sq_ready() noexcept override { return prepared_ ? 1 : 0; }
    int submit() noexcept override {
        if (!prepared_) return 0;
        prepared_ = false;
        if (sqe_[0].opcode == IORING_OP_ASYNC_CANCEL) {
            cancels.fetch_add(1);
            cancel_cookie.store(sqe_[0].user_data);
            cancel_target.store(sqe_[0].addr);
            available_ = true;
            auto* completion = cqe();
            std::memset(completion, 0, sizeof(*completion));
            completion->user_data = sqe_[0].user_data;
            completion->res = -EALREADY;
            return 1;
        }
        const auto opcode = sqe_[0].cmd_op;
        record_command(opcode, sqe_[0].user_data);
        ublksrv_ctrl_cmd cmd{};
        std::memcpy(&cmd, sqe_[0].cmd, sizeof(cmd));
        if (opcode == UBLK_U_CMD_ADD_DEV)
            reinterpret_cast<ublksrv_ctrl_dev_info*>(cmd.addr)->dev_id = 42;
        if (opcode == UBLK_U_CMD_GET_PARAMS) {
            auto* params = reinterpret_cast<ublk_params*>(cmd.addr);
            params->types = UBLK_PARAM_TYPE_BASIC;
            params->basic.dev_sectors = 128;
        }
        if (opcode == UBLK_U_CMD_START_DEV || opcode == UBLK_U_CMD_END_USER_RECOVERY) {
            starts.fetch_add(1);
            original_cookie.store(sqe_[0].user_data);
            held_ = true;
            admitted_at_ = std::chrono::steady_clock::now();
            admitted.store(true);
            return 1;
        }
        if (opcode == UBLK_U_CMD_STOP_DEV) stops.fetch_add(1);
        if (opcode == UBLK_U_CMD_DEL_DEV) deletes.fetch_add(1);
        auto* completion = cqe();
        std::memset(completion, 0, sizeof(*completion));
        completion->user_data = sqe_[0].user_data;
        available_ = true;
        return 1;
    }
    int enter() noexcept override { return 0; }
    int peek_cqe(io_uring_cqe** out) noexcept override {
        if (!available_ && held_) {
            const bool timeout = std::chrono::steady_clock::now() - admitted_at_ > 5s;
            if (timeout) escaped.store(true);
            const bool source_finished = !source_required.load() || source->returned.load();
            if ((release_original.load() && source_finished) || timeout) {
                // A pool-based controller must not deadlock the test: the
                // bounded escape releases only our scripted original CQE.
                source_done_at_original.store(!source_required.load() || source->returned.load());
                held_ = false;
                available_ = true;
                auto* completion = cqe();
                std::memset(completion, 0, sizeof(*completion));
                completion->user_data = original_cookie.load();
                completion->res = -EINTR;
            }
        }
        if (!available_) { *out = nullptr; return -EAGAIN; }
        *out = cqe();
        return 0;
    }
    void cqe_seen(io_uring_cqe*) noexcept override { available_ = false; }
    void idle() noexcept override { std::this_thread::sleep_for(1ms); }
    std::vector<Command> commands() const {
        const auto count = command_count_.load(std::memory_order_acquire);
        std::vector<Command> result;
        result.reserve(count);
        for (unsigned index = 0; index < count; ++index) {
            result.emplace_back(command_ops_[index].load(std::memory_order_relaxed),
                                command_cookies_[index].load(std::memory_order_relaxed));
        }
        return result;
    }
    bool command_overflow() const noexcept { return command_overflow_.load(); }
private:
    void record_command(uint32_t opcode, uint64_t cookie) noexcept {
        const auto index = command_count_.load(std::memory_order_relaxed);
        if (index == command_ops_.size()) {
            command_overflow_.store(true);
            return;
        }
        command_ops_[index].store(opcode, std::memory_order_relaxed);
        command_cookies_[index].store(cookie, std::memory_order_relaxed);
        command_count_.store(index + 1, std::memory_order_release);
    }
    io_uring_cqe* cqe() noexcept {
        return cqe_.get();
    }
    std::array<io_uring_sqe, 2> sqe_{};
    std::unique_ptr<io_uring_cqe> cqe_{new io_uring_cqe{}};
    std::array<std::atomic<uint32_t>, 16> command_ops_{};
    std::array<std::atomic<uint64_t>, 16> command_cookies_{};
    std::atomic<unsigned> command_count_{0};
    std::atomic<bool> command_overflow_{false};
    bool prepared_ = false, available_ = false, held_ = false;
    std::chrono::steady_clock::time_point admitted_at_;
};

enum class Scenario { setup_single, setup_pair, admission_gap, source_progress, deadline };
class Hooks final : public detail::StartupTestHooks {
public:
    explicit Hooks(Scenario which) : scenario(which) {}
    Scenario scenario;
    Script script;
    SourceState* source_state = nullptr;
    Gate failed_setup, paused_setup, admission, initial;
    std::array<Queue*, 2> queues{};
    std::atomic<int> clock_seconds{0};
    std::atomic<bool> native_setup{true}, native_admission{true}, overflow{false};

    detail::CtrlTransport* control_transport() noexcept override { return &script; }
    bool open_queue(Queue& queue, int shared) override {
        StartupCoordinatorTestAccess::open(queue, shared);
        queues.at(queue.q_id()) = &queue;
        return true;
    }
    void before_queue_setup(Queue& queue) override {
        if (elio::runtime::worker_thread::current()) native_setup.store(false);
        if (scenario == Scenario::setup_single || scenario == Scenario::setup_pair) {
            if (queue.q_id() == 0) {
                failed_setup.wait();
                throw std::bad_alloc(); // Queue::run normalizes setup errors to EIO.
            }
            paused_setup.wait();
        }
    }
    void before_start_admission() override {
        if (elio::runtime::worker_thread::current()) native_admission.store(false);
        if (scenario == Scenario::admission_gap) admission.wait();
    }
    void before_initial_submit(uint16_t) noexcept override { initial.wait(); }
    std::chrono::steady_clock::time_point startup_now() noexcept override {
        return std::chrono::steady_clock::time_point{} + std::chrono::seconds(clock_seconds.load());
    }
    void observe(detail::StartupTestEvent event) noexcept override {
        std::lock_guard lock(mutex_);
        if (source_state) {
            if (event.kind == Kind::owner_exit) source_state->owners.fetch_add(1);
            if (event.kind == Kind::bridge_destroyed) source_state->bridges.fetch_add(1);
            if (event.kind == Kind::cdev_closed) source_state->closed.fetch_add(1);
            if (event.kind == Kind::control_destroyed) source_state->controls.fetch_add(1);
        }
        if (size_ == events_.size()) { overflow.store(true); return; }
        events_[size_++] = event;
    }
    std::vector<detail::StartupTestEvent> events() {
        std::lock_guard lock(mutex_);
        return {events_.begin(), events_.begin() + static_cast<ptrdiff_t>(size_)};
    }
    size_t count(Kind kind) {
        const auto copy = events();
        return static_cast<size_t>(std::count_if(copy.begin(), copy.end(),
            [kind](const auto& event) { return event.kind == kind; }));
    }
    void rescue() {
        failed_setup.release(); paused_setup.release(); admission.release(); initial.release();
        script.release_original.store(true);
    }
private:
    std::mutex mutex_;
    std::array<detail::StartupTestEvent, 128> events_{};
    size_t size_ = 0;
};

struct Outcome { bool returned = false, ready = false, released = false; int error = 0; };

void run_scenario(bool recovery, Scenario scenario) {
    Hooks hooks(scenario);
    StartupControl startup(136);
    startup.set_test_hooks(&hooks);
    SourceState source_state;
    hooks.source_state = &source_state;
    source_state.expected_queues = scenario == Scenario::setup_pair ? 2 : 1;
    hooks.script.source = &source_state;
    hooks.script.source_required.store(scenario == Scenario::source_progress);
    Outcome outcome;
    bool cancel_window_seen = false;
    bool source_native_retired = false;
    std::optional<elio::coro::join_handle<void>> source_task;
    elio::runtime::run_config config;
    config.num_threads = 1;
    config.blocking_threads = 1;
    elio::run([&]() -> elio::coro::task<void> {
        auto source = std::make_unique<PoolSource>(source_state);
        auto* borrowed_source = source.get();
        DeviceParams params;
        params.nr_queues = scenario == Scenario::setup_pair ? 2 : 1;
        params.queue_depth = 2;
        params.max_io_buf_bytes = 4096;
        params.dev_sectors = 128;
        const bool setup = scenario == Scenario::setup_single || scenario == Scenario::setup_pair;
        // This callback fences the separate source/pool dependency task even
        // if the script's bounded failure escape fires. Device still owns src.
        Device::FailureCleanup cleanup = [&]() -> elio::coro::task<void> {
            if (source_task) {
                co_await source_task->wait_destroyed_async();
                source_native_retired = true;
            }
        };
        auto launch = [&]() -> elio::coro::task<void> {
            try {
                auto device = recovery
                    ? co_await Device::attach(42, params, std::move(source), startup, cleanup)
                    : co_await Device::create(params, std::move(source), startup, cleanup);
                outcome.ready = true;
                co_await device->stop_async();
            } catch (const StartupReleased&) { outcome.released = true; }
              catch (const std::system_error& error) { outcome.error = error.code().value(); }
              catch (...) { outcome.error = -1; }
            outcome.returned = true;
        };
        auto operation = elio::spawn(launch);
        std::exception_ptr failure;
        // No fatal assertion may strand synthetic gates or borrowed frames.
        try {
            if (setup) {
                const bool failed_entered = co_await until([&] { return hooks.failed_setup.entered.load(); });
                CHECK(failed_entered);
                if (scenario == Scenario::setup_pair) {
                    const bool paused = co_await until([&] { return hooks.paused_setup.entered.load(); });
                    CHECK(paused);
                }
                hooks.failed_setup.release();
                const bool caught = co_await until([&] { return startup.snapshot().reason_errno == EIO; });
                CHECK(caught);
                if (scenario == Scenario::setup_pair) {
                    // Acknowledge the actual failure coordinator's native
                    // bridge drain, not an arbitrary settling delay.
                    const bool drained = co_await until([&] {
                        return hooks.count(Kind::sources_drained) == 2 &&
                               hooks.count(Kind::owner_exit) == 1;
                    });
                    CHECK(drained);
                    CHECK_FALSE(operation.is_ready());
                    CHECK_FALSE(source_state.destroyed.load());
                    CHECK(hooks.count(Kind::cdev_closed) == 0);
                    CHECK(hooks.count(Kind::owner_exit) == 1);
                    CHECK(startup.snapshot().raw_published == 0);
                    if (recovery) {
                        CHECK(hooks.script.stops.load() == 0);
                        CHECK(hooks.script.deletes.load() == 0);
                    }
                }
                hooks.paused_setup.release();
            } else {
                const bool prepared = co_await until([&] { return hooks.initial.entered.load(); });
                CHECK(prepared);
                if (scenario == Scenario::admission_gap) {
                    const bool checked = co_await until([&] { return hooks.admission.entered.load(); });
                    CHECK(checked);
                    if (checked) hooks.queues[0]->fail(EIO);
                    CHECK(hooks.script.starts.load() == 0);
                    hooks.admission.release();
                }
                const bool admitted = co_await until([&] { return hooks.script.admitted.load(); });
                CHECK(admitted);
                if (admitted && scenario == Scenario::source_progress) hooks.queues[0]->fail(EIO);
                if (admitted && scenario == Scenario::deadline) hooks.clock_seconds.store(6);
                const bool cancelled = co_await until([&] {
                    const auto snapshot = startup.snapshot();
                    return snapshot.cancel_known && !snapshot.original_known && snapshot.control_pending;
                });
                cancel_window_seen = cancelled;
                CHECK(cancelled);
                if (cancelled) {
                    const auto snapshot = startup.snapshot();
                    CHECK(snapshot.reason_errno == (scenario == Scenario::deadline ? ETIMEDOUT : EIO));
                    CHECK(snapshot.cancel_result == -EALREADY);
                    CHECK(snapshot.release != StartupRelease::released);
                    CHECK_FALSE(operation.is_ready());
                    CHECK_FALSE(source_state.destroyed.load());
                    CHECK(hooks.count(Kind::control_destroyed) == 0);
                    CHECK(hooks.count(Kind::cdev_closed) == 0);
                    CHECK(hooks.script.stops.load() == 0);
                    CHECK(hooks.script.deletes.load() == 0);
                    CHECK(hooks.script.cancel_target.load() == hooks.script.original_cookie.load());
                    CHECK(hooks.script.cancel_cookie.load() != hooks.script.original_cookie.load());
                }
                if (admitted && scenario == Scenario::source_progress && !operation.is_ready()) {
                    // This call borrows the actual Device-owned source. It is
                    // not a fabricated kernel request or queue-buffer loan.
                    source_task.emplace(elio::spawn(read_device_source, borrowed_source));
                    co_await source_task->wait_destroyed_async();
                    source_native_retired = true;
                    CHECK(source_state.pool_ran.load());
                    CHECK_FALSE(hooks.script.release_original.load());
                    CHECK_FALSE(hooks.script.escaped.load());
                }
                hooks.initial.release();
                hooks.script.release_original.store(true);
                if (recovery) {
                    const bool settled = co_await until([&] {
                        const auto snapshot = startup.snapshot();
                        return snapshot.original_known && !snapshot.control_pending &&
                               snapshot.phase == StartupPhase::retained;
                    });
                    CHECK(settled);
                    const bool retained = co_await until([&] {
                        return hooks.count(Kind::retained_entered) == 1;
                    });
                    CHECK(retained);
                    CHECK_FALSE(operation.is_ready());
                    CHECK(hooks.script.stops.load() == 0);
                    CHECK(hooks.script.deletes.load() == 0);
                    CHECK_FALSE(source_state.destroyed.load());
                    CHECK(startup.snapshot().original_result == -EINTR);
                    CHECK(startup.snapshot().reason_errno == EIO);
                    CHECK(startup.snapshot().release_sequence == 0);
                    CHECK(startup.request_release(136, 1) == ReleaseRequestResult::accepted);
                }
            }
        } catch (...) { failure = std::current_exception(); }
        hooks.rescue();
        // For unexpected early assertion/setup failure, explicit cleanup of an
        // attach still uses the real mailbox, never writes owner state.
        if (failure || (!setup && recovery))
            (void)startup.request_release(136, 1);
        co_await operation;
        co_await operation.wait_destroyed_async();
        if (source_task) {
            co_await source_task->wait_destroyed_async();
            source_task->await_resume();
        }
        if (failure) std::rethrow_exception(failure);
    }, config);

    CHECK(outcome.returned);
    CHECK_FALSE(outcome.ready);
    CHECK_FALSE(hooks.overflow.load());
    CHECK_FALSE(hooks.failed_setup.escaped.load());
    CHECK_FALSE(hooks.paused_setup.escaped.load());
    CHECK_FALSE(hooks.admission.escaped.load());
    CHECK_FALSE(hooks.initial.escaped.load());
    CHECK_FALSE(hooks.script.escaped.load());
    CHECK(hooks.native_setup.load());
    CHECK(hooks.native_admission.load());
    CHECK(source_state.destroyed.load());
    CHECK(source_state.destroyed_after_retirement.load());
    CHECK(hooks.count(Kind::control_destroyed) == 1);
    const size_t queues = scenario == Scenario::setup_pair ? 2 : 1;
    CHECK(hooks.count(Kind::owner_exit) == queues);
    CHECK(hooks.count(Kind::bridge_destroyed) == queues);
    CHECK(hooks.count(Kind::cdev_closed) == queues);
    CHECK(hooks.count(Kind::adopted) == 0);
    const bool setup = scenario == Scenario::setup_single || scenario == Scenario::setup_pair;
    CHECK(hooks.script.starts.load() == (setup ? 0 : 1));
    CHECK(hooks.script.stops.load() == (setup && recovery ? 0 : 1));
    CHECK(hooks.script.deletes.load() == (setup && recovery ? 0 : 1));
    CHECK_FALSE(hooks.script.command_overflow());
    const auto commands = hooks.script.commands();
    std::vector<uint32_t> actual_opcodes;
    std::set<uint64_t> command_cookies;
    actual_opcodes.reserve(commands.size());
    for (const auto& [opcode, cookie] : commands) {
        actual_opcodes.push_back(opcode);
        CHECK(cookie != 0);
        CHECK(command_cookies.insert(cookie).second);
    }
    std::vector<uint32_t> expected_opcodes;
    if (recovery) {
        expected_opcodes = {UBLK_U_CMD_START_USER_RECOVERY, UBLK_U_CMD_GET_PARAMS};
        if (!setup) {
            expected_opcodes.push_back(UBLK_U_CMD_END_USER_RECOVERY);
            expected_opcodes.push_back(UBLK_U_CMD_STOP_DEV);
            expected_opcodes.push_back(UBLK_U_CMD_DEL_DEV);
        }
    } else {
        expected_opcodes = {UBLK_U_CMD_ADD_DEV, UBLK_U_CMD_SET_PARAMS};
        if (!setup) expected_opcodes.push_back(UBLK_U_CMD_START_DEV);
        expected_opcodes.push_back(UBLK_U_CMD_STOP_DEV);
        expected_opcodes.push_back(UBLK_U_CMD_DEL_DEV);
    }
    CHECK(actual_opcodes == expected_opcodes);
    if (setup) {
        CHECK(hooks.count(Kind::raw_prepared) == 0);
        CHECK(startup.snapshot().raw_published == 0);
        CHECK(outcome.error == EIO);
        CHECK_FALSE(outcome.released);
    } else {
        CHECK(cancel_window_seen);
        CHECK(hooks.script.cancels.load() == 1);
        CHECK(command_cookies.insert(hooks.script.cancel_cookie.load()).second);
        CHECK(outcome.released == recovery);
        CHECK(outcome.error == (recovery ? 0 : scenario == Scenario::deadline ? ETIMEDOUT : EIO));
        if (scenario == Scenario::source_progress) {
            CHECK(source_native_retired);
            CHECK(hooks.script.source_done_at_original.load());
        }
    }
    // Assert actual observed lifecycle order, including each original cookie.
    const auto events = hooks.events();
    size_t owners = 0, bridges = 0, closed = 0;
    bool original_finished = setup;
    unsigned original_results = 0, cancellation_results = 0;
    std::map<std::pair<uint32_t, uint64_t>, int> raw;
    for (const auto& event : events) {
        if (event.kind == Kind::raw_prepared) {
            CHECK(raw[std::make_pair(event.queue, event.cookie)] == 0);
            ++raw[std::make_pair(event.queue, event.cookie)];
        }
        if (event.kind == Kind::raw_cqe) {
            CHECK(raw[std::make_pair(event.queue, event.cookie)] == 1);
            --raw[std::make_pair(event.queue, event.cookie)];
        }
        if (event.kind == Kind::control_original && event.cookie == hooks.script.original_cookie.load()) {
            ++original_results;
            CHECK(event.result == -EINTR);
        }
        if (event.kind == Kind::control_cancel) {
            ++cancellation_results;
            CHECK(event.cookie == hooks.script.cancel_cookie.load());
            CHECK(event.value == hooks.script.original_cookie.load());
            CHECK(event.result == -EALREADY);
        }
        if (event.kind == Kind::owner_exit) {
            ++owners;
            CHECK(event.value == 1);
        }
        if (event.kind == Kind::bridge_destroyed) ++bridges;
        if (event.kind == Kind::control_finished &&
            (event.opcode == UBLK_U_CMD_START_DEV || event.opcode == UBLK_U_CMD_END_USER_RECOVERY)) {
            original_finished = true;
            CHECK(event.result == -EINTR);
            CHECK(event.cookie == hooks.script.original_cookie.load());
            CHECK(event.value == static_cast<uint64_t>(scenario == Scenario::deadline ? ETIMEDOUT : EIO));
        }
        if (event.kind == Kind::control_prepared && event.opcode == UBLK_U_CMD_STOP_DEV)
            CHECK(original_finished);
        if (event.kind == Kind::cdev_closed) {
            CHECK(event.result == 0);
            for (const auto& [identity, outstanding] : raw)
                if (identity.first == event.queue) CHECK(outstanding == 0);
            CHECK(owners == queues);
            CHECK(bridges == queues);
            ++closed;
        }
        if (event.kind == Kind::control_prepared && event.opcode == UBLK_U_CMD_DEL_DEV)
            CHECK(closed == queues);
    }
    CHECK(original_finished);
    CHECK(original_results == (setup ? 0 : 1));
    CHECK(cancellation_results == (setup ? 0 : 1));
    for (const auto& [identity, outstanding] : raw) {
        (void)identity;
        CHECK(outstanding == 0);
    }
    CHECK(startup.snapshot().kernel_debt_known);
    CHECK(startup.snapshot().raw_published == startup.snapshot().raw_retired);
    if (!setup && recovery) {
        CHECK(hooks.count(Kind::retained_entered) == 1);
        CHECK(startup.snapshot().release_sequence == 1);
        CHECK(startup.snapshot().release == StartupRelease::released);
    }
}
}

TEST_CASE("ublk: public create catches single queue setup failure", "[ublk][startup-coordinator]") {
    run_scenario(false, Scenario::setup_single);
}
TEST_CASE("ublk: public attach catches single queue setup failure", "[ublk][startup-coordinator]") {
    run_scenario(true, Scenario::setup_single);
}
TEST_CASE("ublk: public create retains paused peer after queue setup failure", "[ublk][startup-coordinator]") {
    run_scenario(false, Scenario::setup_pair);
}
TEST_CASE("ublk: public attach retains paused peer after queue setup failure", "[ublk][startup-coordinator]") {
    run_scenario(true, Scenario::setup_pair);
}
TEST_CASE("ublk: public create observes failure across START admission gap", "[ublk][startup-coordinator]") {
    run_scenario(false, Scenario::admission_gap);
}
TEST_CASE("ublk: public attach observes failure across END admission gap", "[ublk][startup-coordinator]") {
    run_scenario(true, Scenario::admission_gap);
}
TEST_CASE("ublk: public START cancellation leaves sole source blocking worker free", "[ublk][startup-coordinator]") {
    run_scenario(false, Scenario::source_progress);
}
TEST_CASE("ublk: public START deadline retains original after cancellation CQE", "[ublk][startup-coordinator]") {
    run_scenario(false, Scenario::deadline);
}
