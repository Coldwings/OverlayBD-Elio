// UblkDevice: full device lifecycle for one OverlayBD image — control
// plane (ADD_DEV/SET_PARAMS/START_DEV), and fixed-worker per-queue owner
// coroutines serving raw ublk commands and merged-source IO.
//
// Threading model (ADR-0004/0023): the device lives inside one obd-device
// process; one fixed Elio worker owns each queue ring and its reusable tag
// handlers. Teardown stops source admission, proves raw/native retirement,
// closes queue cdevs, and then deletes the device.
#pragma once

#include "source/blob_source.hpp"
#include "ublk/ctrl.hpp"
#include "ublk/queue.hpp"
#include "ublk/startup_control.hpp"

#include <elio/coro/task.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace obd::ublk {

class Device {
public:
    using FailureCleanup = std::function<elio::coro::task<void>()>;

    /// Creates and starts the device over `src` (the merged block view):
    /// opens queues, launches fixed-worker owners (which park their FETCH
    /// commands), issues SET_PARAMS, then START_DEV (retrying EBUSY until
    /// every queue is parked). Must be awaited on the Elio scheduler; owners
    /// are independent joinable tasks retained through raw retirement.
    /// Throws obd::error on failure.
    static elio::coro::task<std::unique_ptr<Device>> create(
        const DeviceParams& params, source::BlobSourcePtr src,
        FailureCleanup failure_cleanup = {});
    static elio::coro::task<std::unique_ptr<Device>> create(
        const DeviceParams& params, source::BlobSourcePtr src,
        StartupControl& startup, FailureCleanup failure_cleanup = {});

    /// ADR-0010: attaches to an EXISTING device created with
    /// UBLK_F_USER_RECOVERY after the previous server process died.
    /// Sequence: GET_DEV_INFO geometry validation → START_USER_RECOVERY
    /// (retrying EBUSY for up to 30s) → open
    /// queues → fixed workers re-park FETCH for every tag →
    /// END_USER_RECOVERY (retrying EBUSY). The replacement must not open a
    /// queue cdev before START succeeds. The params' queue geometry must
    /// match the original device's; a mismatch fails before START recovery.
    /// Must be
    /// awaited on the Elio scheduler. The caller must supply a StartupControl
    /// accessible to a concurrent observer/release service and keep both it and
    /// the runtime alive until this operation finishes. Published, unadopted
    /// failure retains this operation and all resource owners until explicit
    /// release succeeds; it throws StartupReleased after that release. Other
    /// failures throw obd::error only after eligible cleanup has completed.
    /// D3: the grow-only resize baseline (`size_bytes`) is seeded from
    /// the kernel's REAL current capacity (Ctrl::get_params), NOT from
    /// `params` — a grown device keeps its capacity across USER_RECOVERY,
    /// and a params-based baseline would let a post-recovery resize
    /// silently shrink the gendisk.
    static elio::coro::task<std::unique_ptr<Device>> attach(
        uint32_t dev_id, const DeviceParams& params,
        source::BlobSourcePtr src, StartupControl& startup,
        FailureCleanup failure_cleanup = {});

    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    uint32_t dev_id() const noexcept { return dev_id_; }
    size_t queue_count() const noexcept { return queues_.size(); }
    std::string bdev_path() const { return Ctrl::bdev_path(dev_id_); }

    /// True once the kernel gendisk is live (START_DEV succeeded).
    bool started() const noexcept { return started_; }

    /// Current device capacity in bytes (the grow-only resize baseline:
    /// on create, the capacity this process set via SET_PARAMS; on a
    /// recovery attach, the kernel's real capacity read back via
    /// GET_PARAMS — which may exceed the create-time size after a grow;
    /// updated by resize_blocking).
    uint64_t size_bytes() const noexcept {
        return cur_bytes_.load(std::memory_order_acquire);
    }

    /// D3 grow-only online resize. Issues UBLK_U_CMD_UPDATE_SIZE so the
    /// kernel gendisk grows to `bytes`. BLOCKING — the kernel control
    /// call may sleep on our own data plane, so this must run off an
    /// Elio worker via elio::spawn_blocking (the device command loop
    /// does that; see run_device_control). Grow-only is enforced HERE
    /// too (defense in depth, so any caller can never shrink): a request
    /// <= the current size is rejected with obd::error. Returns the new
    /// size in bytes; throws obd::error (EINVAL for a shrink/no-op or a
    /// misaligned/zero request, the kernel's errno for UPDATE_SIZE
    /// failure — on a driver without the command (it landed in the
    /// 6.16 development cycle: ENOTSUPP/524 on pre-6.15 kernels,
    /// EOPNOTSUPP/95 on 6.15+)).
    uint64_t resize_blocking(uint64_t bytes);

    /// Blocking source/owner drain. Call off an Elio worker,
    /// with scheduler and any source blocking executor able to progress.
    /// No resource is released while an owner/handler still uses it.
    void stop() noexcept;

    /// Scheduler-side drain: leaves both workers and the blocking pool free
    /// while source IO completes, then retires raw owners via cold control work.
    /// The caller must retain this Device until the await completes.
    elio::coro::task<void> stop_async();
    /// Read only after awaited stop_async (or blocking stop) has completed.
    /// Unlike the startup observer, this includes the final queue ledger.
    StartupSnapshot released_snapshot() const noexcept;

private:
    friend struct DeviceTestAccess;
    Device() = default;
    void open_queues();
    std::chrono::steady_clock::time_point startup_now() const noexcept;
    void request_stop() noexcept;
    static elio::coro::task<std::unique_ptr<Device>> start_impl(
        bool recovery, uint32_t dev_id, const DeviceParams& params,
        source::BlobSourcePtr src, StartupControl& startup,
        FailureCleanup failure_cleanup);
    elio::coro::task<void> prepare_owners();
    elio::coro::task<void> drain_sources();
    elio::coro::task<void> retained_startup();
    /// Reflect a mailbox admission in the observable startup snapshot before
    /// potentially long source/raw retirement. The owning Device coroutine is
    /// the sole snapshot writer; the control-channel caller only owns the
    /// mailbox CAS.
    uint64_t publish_pending_release() noexcept;
    void collect_bridge_results() noexcept;
    bool release_once() noexcept;
    void refresh_startup() noexcept;
    void publish_startup() noexcept;
    bool try_publish_ready();
    static int cancel_control(void*, uint32_t) noexcept;
    static void complete_control(void*, uint32_t, int) noexcept;
    static void observe_control(void*, const CtrlProgress&) noexcept;
    // Shared production failure path; the start_impl frame retains this
    // Device until cleanup or explicitly authorized release has completed.
    elio::coro::task<std::unique_ptr<Device>> fail_startup(
        bool recovery, std::exception_ptr failure, FailureCleanup& failure_cleanup);

    DeviceParams params_;
    source::BlobSourcePtr src_;
    std::unique_ptr<Ctrl> ctrl_;
    uint32_t dev_id_ = 0;
    bool started_ = false;
    bool owns_id_ = false;
    bool publication_released_ = false;
    bool recovery_started_ = false;
    bool release_authorized_ = false;
    bool released_ = false;
    bool stop_completed_ = false;
    bool device_absent_ = false;
    int release_error_ = 0;
    std::mutex release_mu_;
#ifdef OBD_TEST_HOOKS
    detail::StartupTestHooks* test_hooks_ = nullptr;
#endif
    StartupControl* startup_ = nullptr; // Cleared before returning a ready Device.
    StartupSnapshot startup_state_;
    std::chrono::steady_clock::time_point startup_deadline_;
    /// Live device capacity in bytes; the resize grow-only baseline.
    /// Atomic: resize_blocking runs on a spawn_blocking pool thread
    /// while the device command loop reads size_bytes() on a worker.
    std::atomic<uint64_t> cur_bytes_{0};

    std::atomic<bool> stop_{false};
    // One fixed-worker owner per production queue. It remains alive after
    // handler/source drain until STOP and every raw command have retired.
    std::vector<elio::coro::join_handle<void>> owner_tasks_;
    // Legacy bridge/thread owners remain only for deterministic test fixtures
    // whose synchronous gates model the older topology.
    std::vector<elio::coro::join_handle<void>> bridge_tasks_;
    std::mutex bridge_result_mu_;
    bool bridge_results_reported_ = false;
    std::vector<std::unique_ptr<Queue>> queues_;
    std::vector<std::thread> threads_;
};

}  // namespace obd::ublk
