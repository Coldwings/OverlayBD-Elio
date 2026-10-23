// UblkDevice lifecycle. See device.hpp.
#include "ublk/device.hpp"

#include "common/errors.hpp"
#include "ublk/elio_bridge.hpp"
#include "ublk/recovery.hpp"
#include "ublk/control_task.hpp"

#include <elio/log/macros.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/runtime/spawn_blocking.hpp>
#include <elio/time/timer.hpp>

#include <cerrno>
#include <chrono>
#include <exception>
#include <thread>
#include <type_traits>

namespace obd::ublk {

void Device::open_queues() {
    int shared_cdev_fd = -1;
    for (uint16_t q = 0; q < params_.nr_queues; ++q) {
        auto queue = std::make_unique<Queue>(dev_id_, q, params_.queue_depth,
                                             params_.max_io_buf_bytes);
#ifdef OBD_TEST_HOOKS
        queue->set_test_hooks(test_hooks_);
#endif
        // The driver's open exclusion is device-wide, not per queue.
        // Duplicate the first queue's descriptor so each queue retains
        // independent ownership without opening a second server session.
#ifdef OBD_TEST_HOOKS
        if (!test_hooks_ || !test_hooks_->open_queue(*queue, shared_cdev_fd))
#endif
            queue->open(shared_cdev_fd);
        if (shared_cdev_fd < 0) shared_cdev_fd = queue->cdev_fd();
        queues_.push_back(std::move(queue));
    }
}

std::chrono::steady_clock::time_point Device::startup_now() const noexcept {
#ifdef OBD_TEST_HOOKS
    if (test_hooks_) return test_hooks_->startup_now();
#endif
    return std::chrono::steady_clock::now();
}

namespace {
int exception_errno(const std::exception_ptr& failure) noexcept {
    try {
        if (failure) std::rethrow_exception(failure);
    } catch (const std::system_error& e) {
        return e.code().value() ? e.code().value() : EIO;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    } catch (...) {
        return EIO;
    }
    return 0;
}
} // namespace

elio::coro::task<std::unique_ptr<Device>> Device::create(
    const DeviceParams& params, source::BlobSourcePtr src,
    FailureCleanup failure_cleanup) {
    StartupControl startup(1);
    co_return co_await create(params, std::move(src), startup,
                              std::move(failure_cleanup));
}

elio::coro::task<std::unique_ptr<Device>> Device::create(
    const DeviceParams& params, source::BlobSourcePtr src,
    StartupControl& startup, FailureCleanup failure_cleanup) {
    co_return co_await start_impl(false, UINT32_MAX, params, std::move(src),
                                  startup, std::move(failure_cleanup));
}

elio::coro::task<std::unique_ptr<Device>> Device::attach(
    uint32_t dev_id, const DeviceParams& params, source::BlobSourcePtr src,
    StartupControl& startup, FailureCleanup failure_cleanup) {
    co_return co_await start_impl(true, dev_id, params, std::move(src),
                                  startup, std::move(failure_cleanup));
}

void Device::refresh_startup() noexcept {
    startup_state_.device_id = dev_id_;
    startup_state_.raw_published = 0;
    startup_state_.raw_retired = 0;
    startup_state_.kernel_debt_known = true;
    for (const auto& q : queues_) {
        const auto debt = q->retirement();
        startup_state_.raw_published += debt.published;
        startup_state_.raw_retired += debt.retired;
        startup_state_.kernel_debt_known &= debt.known;
        if (q->failed() && startup_state_.reason_errno == 0) {
            startup_state_.reason_errno = q->failure() ? q->failure() : EIO;
            startup_state_.queue_id = q->q_id();
        }
    }
}

void Device::publish_startup() noexcept {
    if (!startup_) return;
    refresh_startup();
    startup_->publish(startup_state_);
}

bool Device::try_publish_ready() {
    refresh_startup();
    for (size_t i = 0; i < queues_.size(); ++i) {
        const auto& q = queues_[i];
        const bool owner_ready = !owner_tasks_.empty()
            ? owner_tasks_[i].is_ready() : bridge_tasks_[i].is_ready();
        if (q->failed() || owner_ready)
            throw error(q->failure() ? q->failure() : EIO,
                        "ublk owner failed during startup control");
    }
    if (!startup_state_.kernel_debt_known)
        throw error(EPROTO, "ublk startup command ownership is unknown");
    if (!startup_->close_release_admission()) return false;

    // Original START/END success wins over earlier cancellation intent.
    // All failure checks precede admission closure. Publish this successful
    // handoff without rescanning queues and mixing a later runtime failure
    // into the ready record: the receiver requires a coherent ready state.
    startup_state_.phase = StartupPhase::ready;
    startup_state_.reason_errno = 0;
    startup_state_.control_pending = false;
    startup_->publish(startup_state_);
    ctrl_->set_observer({});
    startup_ = nullptr;
    return true;
}

int Device::cancel_control(void* opaque, uint32_t opcode) noexcept {
    auto& dev = *static_cast<Device*>(opaque);
    if (opcode != UBLK_U_CMD_START_DEV &&
        opcode != UBLK_U_CMD_END_USER_RECOVERY) return 0;
    if (dev.startup_ && dev.startup_->release_request()) return ECANCELED;
    for (const auto& q : dev.queues_)
        if (q->failed()) return q->failure() ? q->failure() : EIO;
    if (dev.startup_now() >= dev.startup_deadline_)
        return ETIMEDOUT;
    return 0;
}

void Device::complete_control(void* opaque, uint32_t opcode, int result) noexcept {
    auto& dev = *static_cast<Device*>(opaque);
    if (result != 0) return;
    if (opcode == UBLK_U_CMD_ADD_DEV) {
        dev.dev_id_ = static_cast<uint32_t>(dev.ctrl_->added_dev_id());
        dev.owns_id_ = true;
    } else if (opcode == UBLK_U_CMD_START_USER_RECOVERY) {
        dev.recovery_started_ = true;
    } else if (opcode == UBLK_U_CMD_START_DEV ||
               opcode == UBLK_U_CMD_END_USER_RECOVERY) {
        // Commit at the original CQE, even if cancellation is still pending.
        dev.ctrl_->adopt_dev(dev.dev_id_);
        dev.owns_id_ = true;
        dev.started_ = true;
#ifdef OBD_TEST_HOOKS
        if (dev.test_hooks_) dev.test_hooks_->observe({.kind = detail::StartupTestKind::adopted,
            .opcode = opcode, .result = result, .value = dev.dev_id_});
#endif
    }
}

void Device::observe_control(void* opaque, const CtrlProgress& progress) noexcept {
    auto& dev = *static_cast<Device*>(opaque);
    if (!dev.startup_) return;
    auto& state = dev.startup_state_;
    switch (progress.opcode) {
    case UBLK_U_CMD_START_DEV: state.operation = StartupOperation::start; break;
    case UBLK_U_CMD_END_USER_RECOVERY: state.operation = StartupOperation::end; break;
    case UBLK_U_CMD_STOP_DEV: state.operation = StartupOperation::stop; break;
    case UBLK_U_CMD_DEL_DEV: state.operation = StartupOperation::remove; break;
    default: state.operation = StartupOperation::none; break;
    }
    state.control_pending = progress.pending;
    state.original_known = progress.original_known;
    state.original_result = progress.original_result;
    state.cancel_known = progress.cancel_known;
    state.cancel_result = progress.cancel_result;
    if (progress.reason_errno) state.reason_errno = progress.reason_errno;
    if (state.phase != StartupPhase::releasing) {
        state.phase = state.reason_errno ? StartupPhase::retained
                     : progress.pending ? StartupPhase::control_pending
                                        : StartupPhase::preparing;
    }
    dev.publish_startup();
}

elio::coro::task<void> Device::prepare_owners() {
    open_queues();
    bool legacy_threads = false;
#ifdef OBD_TEST_HOOKS
    legacy_threads = test_hooks_ && test_hooks_->legacy_thread_topology();
#endif
    if (legacy_threads) {
        threads_.reserve(queues_.size());
        bridge_tasks_.reserve(queues_.size());
        for (auto& queue : queues_) {
            Queue* q = queue.get();
            threads_.emplace_back([this, q]() noexcept {
                try { q->run(stop_); }
                catch (...) { q->fail(EIO); }
#ifdef OBD_TEST_HOOKS
                if (test_hooks_) test_hooks_->observe({.kind = detail::StartupTestKind::owner_exit,
                    .queue = q->q_id(), .result = q->failure(), .value = q->owner_exited() ? 1U : 0U});
#endif
            });
            bridge_tasks_.push_back(elio::spawn(
                run_bridge, q, src_.get(), &stop_));
        }
    } else {
        auto abort_unstarted_from = [&](size_t first, int reason) noexcept {
            for (size_t index = first; index < queues_.size(); ++index)
                (void)queues_[index]->abort_unstarted_owner(reason);
        };
        auto* scheduler = elio::runtime::scheduler::current();
        if (!scheduler || scheduler->num_threads(std::memory_order_acquire) == 0) {
            abort_unstarted_from(0, EINVAL);
            throw error(EINVAL, "ublk owner requires a running Elio scheduler");
        }
        const size_t workers = scheduler->num_threads(std::memory_order_acquire);
        static_assert(std::is_nothrow_move_constructible_v<
                      elio::coro::join_handle<void>>);
        try {
            owner_tasks_.reserve(queues_.size());
        } catch (...) {
            abort_unstarted_from(0, exception_errno(std::current_exception()));
            throw;
        }
        for (size_t index = 0; index < queues_.size(); ++index) {
            Queue* q = queues_[index].get();
            try {
#ifdef OBD_TEST_HOOKS
                if (test_hooks_) test_hooks_->before_owner_spawn(q->q_id());
#endif
                const size_t owner = q->q_id() % workers;
                owner_tasks_.push_back(scheduler->go_joinable_to(
                    owner, run_owner_pump, q, src_.get(), &stop_));
            } catch (...) {
                abort_unstarted_from(index,
                    exception_errno(std::current_exception()));
                throw;
            }
            // Elio represents scheduler rejection as a ready exceptional join
            // handle. If the body never won begin_owner(), explicitly seal the
            // queue: it owns no SQE, source frame, or raw-ring debt.
            if (owner_tasks_.back().is_ready() && !q->owner_started())
                (void)q->abort_unstarted_owner(EIO);
        }
    }
    for (;;) {
        if (startup_->release_request())
            throw error(ECANCELED, "release requested before queue publication");
        bool ready = true;
        for (size_t i = 0; i < queues_.size(); ++i) {
            const auto& q = queues_[i];
            const bool owner_ready = !owner_tasks_.empty()
                ? owner_tasks_[i].is_ready() : bridge_tasks_[i].is_ready();
            if (!owner_tasks_.empty() && owner_ready && !q->owner_started())
                (void)q->abort_unstarted_owner(EIO);
            if (q->failed() || owner_ready)
                throw error(q->failure() ? q->failure() : EIO,
                            "ublk owner failed before FETCH publication");
            ready &= q->setup_ready() && q->bridge_ready();
        }
        if (ready) break;
        if (startup_now() >= startup_deadline_)
            throw error(ETIMEDOUT, "ublk owner setup deadline expired");
        co_await elio::time::sleep_for(std::chrono::milliseconds(1));
    }
    // Only after all queue rings and source bridges are ready may any FETCH
    // be published. A later failure is handled by the observed controller.
    publication_released_ = true;
    for (auto& q : queues_) q->publish_initial_fetches();
}

elio::coro::task<std::unique_ptr<Device>> Device::start_impl(
    bool recovery, uint32_t dev_id, const DeviceParams& params,
    source::BlobSourcePtr src, StartupControl& startup,
    FailureCleanup failure_cleanup) {
    if (!src) throw error(EINVAL, "ublk startup with null block source");
    if (!params.dev_sectors || !params.nr_queues)
        throw error(EINVAL, "ublk startup with zero size or queue count");
    auto dev = std::unique_ptr<Device>(new Device());
    if (startup.owner_entered_.exchange(true))
        throw error(EALREADY, "startup control already has an owner");
    dev->params_ = params;
    dev->src_ = std::move(src);
    dev->dev_id_ = dev_id;
    dev->startup_ = &startup;
#ifdef OBD_TEST_HOOKS
    dev->test_hooks_ = startup.test_hooks_;
#endif
    dev->startup_deadline_ = dev->startup_now() +
                             std::chrono::seconds(5);
    dev->publish_startup();
    std::exception_ptr failure;
    try {
        if (startup.release_request()) throw StartupReleased{};
        co_await detail::run_control([&] {
#ifdef OBD_TEST_HOOKS
            auto* transport = dev->test_hooks_ ? dev->test_hooks_->control_transport() : nullptr;
            if (transport) dev->ctrl_ = std::make_unique<Ctrl>(*transport);
            else
#endif
                dev->ctrl_ = std::make_unique<Ctrl>();
            dev->ctrl_->disable_destructor_cleanup();
#ifdef OBD_TEST_HOOKS
            dev->ctrl_->set_test_hooks(dev->test_hooks_);
#endif
            dev->ctrl_->set_observer({dev.get(), cancel_control,
                                      complete_control, observe_control});
        });
        if (recovery) {
            co_await detail::start_recovery_and_open_queues(
                [&] {
                    return detail::run_control([&] {
                        if (startup.release_request())
                            throw error(ECANCELED, "recovery release requested");
                        dev->ctrl_->start_user_recovery(dev_id);
                    });
                },
                [] { return std::chrono::steady_clock::now(); },
                [](std::chrono::steady_clock::duration delay) {
                    return elio::time::sleep_for(delay);
                }, [] {});
            co_await detail::run_control([&] {
                dev->cur_bytes_.store(
                    dev->ctrl_->get_params(dev_id).basic.dev_sectors * 512,
                    std::memory_order_release);
            });
        } else {
            co_await detail::run_control([&] {
                dev->dev_id_ = dev->ctrl_->add_dev(params);
                dev->ctrl_->set_params(dev->dev_id_, params);
            });
            dev->cur_bytes_.store(params.dev_sectors * 512,
                                  std::memory_order_release);
        }
        dev->startup_deadline_ = dev->startup_now() +
                                 std::chrono::seconds(5);
        co_await dev->prepare_owners();
        for (;;) {
            const int reason = cancel_control(dev.get(), recovery
                ? UBLK_U_CMD_END_USER_RECOVERY : UBLK_U_CMD_START_DEV);
            if (reason) throw error(reason, "ublk startup prerequisite failed");
            bool busy = false;
            try {
                co_await detail::run_control([&] {
#ifdef OBD_TEST_HOOKS
                    if (dev->test_hooks_) dev->test_hooks_->before_start_admission();
#endif
                    if (recovery) dev->ctrl_->end_user_recovery(dev_id);
                    else dev->ctrl_->start_dev(dev->dev_id_);
                });
            } catch (const std::system_error& e) {
                if (e.code().value() != EBUSY) throw;
                busy = true;
            }
            if (!busy) break;
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
#ifdef OBD_TEST_HOOKS
        if (recovery && dev->test_hooks_) co_await dev->test_hooks_->after_start_control();
#endif
        if (dev->try_publish_ready())
            co_return dev; // Ownership transfer after this point is nonthrowing.
    } catch (...) {
        failure = std::current_exception();
    }

    // Allocate/await the failure coordinator without allowing an allocation
    // failure at this boundary to unwind a published, unadopted owner. Keep
    // the cleanup callback here so a failed frame allocation can retry it.
    for (;;) {
        try {
            co_return co_await dev->fail_startup(recovery, failure, failure_cleanup);
        } catch (...) {
            if (dev->released_) throw;
        }
        try { co_await elio::time::sleep_for(std::chrono::milliseconds(10)); }
        catch (...) {}
    }
}

elio::coro::task<std::unique_ptr<Device>> Device::fail_startup(
    bool recovery, std::exception_ptr failure, FailureCleanup& failure_cleanup) {
    startup_state_.reason_errno = exception_errno(failure);
    startup_state_.phase = StartupPhase::retained;
    publish_startup();
    // Keep the whole source graph and this frame on failures of the native
    // destruction observer. A timeout cannot convert a borrowed buffer to free.
    bool drained = false;
    while (!drained) {
        try { co_await drain_sources(); drained = true; }
        catch (...) { startup_state_.reason_errno = ENOMEM; }
    }
    if (failure_cleanup) {
        bool parked = false;
        while (!parked) {
            try { co_await failure_cleanup(); parked = true; }
            catch (...) { startup_state_.reason_errno = EIO; }
        }
    }
    // A newly added/adopted device has cleanup authority. An unadopted old
    // recovery with published FETCH commands must await explicit permission.
    const bool preserve = recovery && publication_released_ && !owns_id_;
    if (preserve || !startup_->close_release_admission()) {
        bool complete = false;
        while (!complete) {
            try { co_await retained_startup(); complete = true; }
            catch (...) { startup_state_.reason_errno = ENOMEM; }
        }
        throw StartupReleased{};
    }
    while (!released_) {
        try {
            co_await detail::run_control([&] { (void)release_once(); });
        } catch (...) {
            release_error_ = ENOMEM;
        }
        if (!released_) {
            publish_startup();
            try { co_await elio::time::sleep_for(std::chrono::milliseconds(10)); }
            catch (...) {}
        }
    }
    startup_state_.phase = StartupPhase::released;
    publish_startup();
    if (failure) std::rethrow_exception(failure);
    throw StartupReleased{};
}

elio::coro::task<void> Device::retained_startup() {
#ifdef OBD_TEST_HOOKS
    if (test_hooks_) test_hooks_->observe({.kind = detail::StartupTestKind::retained_entered});
#endif
    for (;;) {
        const auto sequence = publish_pending_release();
        if (sequence) {
            try {
                co_await detail::run_control([&] {
#ifdef OBD_TEST_HOOKS
                    if (test_hooks_) test_hooks_->before_release_attempt(sequence);
#endif
                    (void)release_once();
                });
            } catch (...) {
                release_error_ = ENOMEM;
            }
            if (released_) {
                startup_->finish_release_admission(sequence);
                startup_state_.phase = StartupPhase::released;
                startup_state_.release = StartupRelease::released;
                startup_state_.release_errno = 0;
                publish_startup();
                co_return;
            }
            // EAGAIN means STOP has settled but raw/native retirement has
            // not. Continue observing the same attempt without resending it.
            if (release_error_ != EAGAIN) {
                startup_state_.phase = StartupPhase::retained;
                startup_state_.release = StartupRelease::refused;
                startup_state_.release_errno = release_error_;
                publish_startup();
                startup_->acknowledge_release(sequence);
            }
        }
        publish_startup();
        try { co_await elio::time::sleep_for(std::chrono::milliseconds(10)); }
        catch (...) {}
    }
}

uint64_t Device::publish_pending_release() noexcept {
    if (!startup_) return 0;
    const auto sequence = startup_->release_request();
    if (sequence == 0) return 0;
    release_authorized_ = true;
    if (startup_state_.release_sequence != sequence ||
        startup_state_.release != StartupRelease::pending ||
        startup_state_.phase != StartupPhase::releasing) {
        startup_state_.phase = StartupPhase::releasing;
        startup_state_.release_sequence = sequence;
        startup_state_.release = StartupRelease::pending;
        startup_state_.release_errno = 0;
        publish_startup();
    }
    return sequence;
}

uint64_t Device::resize_blocking(uint64_t bytes) {
    // D3 grow-only online resize. BLOCKING (kernel control call): every
    // caller must route this off an Elio worker via spawn_blocking —
    // the device command loop does so (see run_device_control).
    if (bytes == 0 || bytes % 512 != 0) {
        throw error(EINVAL, "resize size must be a positive multiple of "
                            "512 bytes");
    }
    const uint64_t cur = cur_bytes_.load(std::memory_order_acquire);
    // Grow-only (ADR-0014 dev_size model): shrinking or no-op'ing a live
    // device is rejected cleanly here — the executor's reply surfaces
    // this message to the CLI.
    if (bytes <= cur) {
        throw error(EINVAL, "resize rejected: grow-only (requested " +
                                std::to_string(bytes) +
                                " <= current " + std::to_string(cur) +
                                " bytes)");
    }
    ctrl_->update_size(dev_id_, bytes / 512);
    cur_bytes_.store(bytes, std::memory_order_release);
    ELIO_LOG_INFO("ublk device {} grew to {} bytes", dev_id_, bytes);
    return bytes;
}

void Device::request_stop() noexcept {
    stop_.store(true, std::memory_order_relaxed);
    for (auto& q : queues_) {
        if (ctrl_) q->request_release();
        if (!publication_released_) q->abort_setup();
        q->wakeup();
        q->notify_elio();
    }
}

void Device::collect_bridge_results() noexcept {
    std::lock_guard lock(bridge_result_mu_);
    if (bridge_results_reported_) return;
    bridge_results_reported_ = true;
    for (size_t index = 0; index < owner_tasks_.size(); ++index) {
        auto& owner = owner_tasks_[index];
#ifdef OBD_TEST_HOOKS
        if (test_hooks_) test_hooks_->observe({.kind = detail::StartupTestKind::bridge_destroyed,
            .queue = static_cast<uint32_t>(index)});
#endif
        try { owner.await_resume(); }
        catch (...) {
            try { ELIO_LOG_ERROR("ublk owner failed while draining"); }
            catch (...) {}
        }
    }
    for (auto& bridge : bridge_tasks_) {
        try { bridge.await_resume(); }
        catch (...) {
            // Destruction, not result delivery, is the ownership fence.
            try { ELIO_LOG_ERROR("ublk bridge failed while draining"); }
            catch (...) {}
        }
    }
}

elio::coro::task<void> Device::drain_sources() {
    request_stop();
    if (!owner_tasks_.empty()) {
        for (;;) {
            // Admission and source retirement are independent facts. A source
            // handler (or a prepared-but-held initial FETCH) may outlive the
            // supervisor's command timeout, so expose the accepted request
            // while preserving the drain fence before release_once().
            (void)publish_pending_release();
            bool drained = true;
            for (const auto& queue : queues_)
                drained &= queue->source_handlers_drained();
            if (drained) break;
            try { co_await elio::time::sleep_for(std::chrono::milliseconds(1)); }
            catch (...) {}
        }
#ifdef OBD_TEST_HOOKS
        if (test_hooks_) {
            for (const auto& queue : queues_)
                test_hooks_->observe({.kind = detail::StartupTestKind::sources_drained,
                    .queue = queue->q_id()});
        }
#endif
        co_return;
    }
    for (size_t index = 0; index < bridge_tasks_.size(); ++index) {
        auto& bridge = bridge_tasks_[index];
        while (!bridge.is_destroyed()) {
            try { co_await bridge.wait_destroyed_async(); }
            catch (...) {}
        }
#ifdef OBD_TEST_HOOKS
        if (test_hooks_) test_hooks_->observe({.kind = detail::StartupTestKind::bridge_destroyed,
            .queue = static_cast<uint32_t>(index)});
#endif
    }
    collect_bridge_results();
    for (auto& q : queues_) {
        q->sources_drained();
#ifdef OBD_TEST_HOOKS
        if (test_hooks_) test_hooks_->observe({.kind = detail::StartupTestKind::sources_drained,
            .queue = q->q_id()});
#endif
    }
}

bool Device::release_once() noexcept {
    std::lock_guard lock(release_mu_);
    if (released_) return true;
    if (publication_released_ && !owns_id_ && !release_authorized_) {
        release_error_ = EPERM;
        return false;
    }
    const bool has_target = dev_id_ != UINT32_MAX;
    if (!ctrl_ && release_authorized_ && has_target) {
        try {
            ctrl_ = std::make_unique<Ctrl>();
            ctrl_->disable_destructor_cleanup();
#ifdef OBD_TEST_HOOKS
            ctrl_->set_test_hooks(test_hooks_);
#endif
            ctrl_->set_observer({this, cancel_control, complete_control,
                                 observe_control});
        } catch (...) {
            release_error_ = exception_errno(std::current_exception());
            return false;
        }
    }
    // This routine runs off-worker, after every accepted source frame has
    // retired. Queue owners remain runnable to COMMIT delivered requests and
    // collect original FETCH completions while STOP is in progress.
    const size_t raw_owners = !owner_tasks_.empty()
        ? owner_tasks_.size() : threads_.size();
    for (auto& q : queues_) {
        if (ctrl_) q->request_release();
        if (!publication_released_) q->abort_setup();
    }
    // STOP only cancels commands the driver has already seen. A queue paused
    // after preparing an initial FETCH must finish its synchronous submission
    // before that cancellation pass; otherwise FETCH can park after STOP.
    // Source drain is a different fence and does not establish this ordering.
    if (publication_released_) {
        for (size_t index = 0; index < raw_owners; ++index) {
            const auto& q = queues_[index];
            if (!q->owner_exited() && !q->initial_fetches_issued()) {
                release_error_ = EAGAIN;
                return false;
            }
        }
    }
    if (ctrl_ && has_target && (owns_id_ || release_authorized_) && !stop_completed_) {
        const int rc = ctrl_->stop_dev_result(dev_id_);
        if (rc != 0 && rc != -ENODEV) {
            release_error_ = -rc;
            return false;
        }
        device_absent_ = rc == -ENODEV;
        stop_completed_ = true;
        for (auto& q : queues_) q->kernel_stopped();
    }
    for (auto& q : queues_) q->request_owner_exit();
    // An initial snapshot can precede actual FETCH preparation. Require the
    // raw owner's exit assertion as well, then re-read its final ledger.
    for (size_t index = 0; index < raw_owners; ++index) {
        if (!queues_[index]->owner_exited()) {
            release_error_ = EAGAIN;
            return false;
        }
    }
    for (const auto& q : queues_) {
        if (!q->retirement().complete) {
            release_error_ = EAGAIN;
            return false;
        }
    }
    for (auto& thread : threads_) if (thread.joinable()) thread.join();
    threads_.clear();
    for (const auto& owner : owner_tasks_) {
        if (!owner.is_destroyed()) {
            release_error_ = EAGAIN;
            return false;
        }
    }
    collect_bridge_results();
    for (auto& q : queues_) q->close_retired();
    // No cdev descriptor remains. A terminal ambiguous DEL result is retained
    // in Ctrl and cannot cause another deletion of the same numeric ID.
    if (ctrl_ && has_target && (owns_id_ || release_authorized_) && !device_absent_) {
        const int rc = ctrl_->delete_dev_once(dev_id_);
        if (rc != 0 && rc != -ENODEV) {
            release_error_ = -rc;
            return false;
        }
    }
    release_error_ = 0;
    released_ = true;
    return true;
}

elio::coro::task<void> Device::stop_async() {
    co_await drain_sources();
    while (!released_) {
        try {
            co_await detail::run_control([this] { (void)release_once(); });
        } catch (...) {}
        if (!released_) {
            try { co_await elio::time::sleep_for(std::chrono::milliseconds(10)); }
            catch (...) {}
        }
    }
}

void Device::stop() noexcept {
    request_stop();
    // Blocking callers must leave source executors and the scheduler alive.
    if (!owner_tasks_.empty()) {
        for (;;) {
            bool drained = true;
            for (const auto& queue : queues_)
                drained &= queue->source_handlers_drained();
            if (drained) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } else {
        for (auto& bridge : bridge_tasks_) bridge.wait_destroyed();
        collect_bridge_results();
        for (auto& q : queues_) q->sources_drained();
    }
    while (!release_once())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

Device::~Device() {
    stop();
}

StartupSnapshot Device::released_snapshot() const noexcept {
    auto result = startup_state_;
    result.device_id = dev_id_;
    result.phase = released_ ? StartupPhase::released : StartupPhase::retained;
    result.control_pending = ctrl_ && ctrl_->command_pending();
    result.raw_published = 0;
    result.raw_retired = 0;
    result.kernel_debt_known = true;
    for (const auto& q : queues_) {
        const auto ledger = q->retirement();
        result.raw_published += ledger.published;
        result.raw_retired += ledger.retired;
        result.kernel_debt_known &= ledger.known;
        if (!ledger.complete) result.phase = StartupPhase::retained;
    }
    return result;
}

} // namespace obd::ublk
