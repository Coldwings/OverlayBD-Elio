// UblkDevice: full device lifecycle for one OverlayBD image — control
// plane (ADD_DEV/SET_PARAMS/START_DEV), per-queue data-plane threads, and
// the Elio bridge coroutines serving IO from the merged block source.
//
// Threading model (ADR-0004/0006): the device lives inside one obd-device
// process; queue threads own their rings; the Elio scheduler serves the
// image stack. Destruction stops queue threads, then STOP_DEV/DEL_DEV via
// ~Ctrl.
#pragma once

#include "source/blob_source.hpp"
#include "ublk/ctrl.hpp"
#include "ublk/queue.hpp"

#include <elio/coro/task.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace obd::ublk {

class Device {
public:
    /// Creates and starts the device over `src` (the merged block view):
    /// opens queues, launches queue threads (which park their FETCH
    /// commands), issues SET_PARAMS, spawns the bridge coroutines, then
    /// START_DEV (retrying EBUSY until every queue is parked). Must be
    /// awaited on the Elio scheduler (bridges spawn with elio::go()).
    /// Throws obd::error on failure.
    static elio::coro::task<std::unique_ptr<Device>> create(
        const DeviceParams& params, source::BlobSourcePtr src);

    /// ADR-0010: attaches to an EXISTING device created with
    /// UBLK_F_USER_RECOVERY after the previous server process died.
    /// Sequence: START_USER_RECOVERY → open queues → queue threads re-park
    /// FETCH for every tag → END_USER_RECOVERY (retrying EBUSY). The
    /// params' queue geometry must match the original device's. Must be
    /// awaited on the Elio scheduler. Throws obd::error on failure.
    /// D3: the grow-only resize baseline (`size_bytes`) is seeded from
    /// the kernel's REAL current capacity (Ctrl::get_params), NOT from
    /// `params` — a grown device keeps its capacity across USER_RECOVERY,
    /// and a params-based baseline would let a post-recovery resize
    /// silently shrink the gendisk.
    static elio::coro::task<std::unique_ptr<Device>> attach(
        uint32_t dev_id, const DeviceParams& params,
        source::BlobSourcePtr src);

    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    uint32_t dev_id() const noexcept { return dev_id_; }
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

    /// Signals queue threads and bridges to stop, joins the threads, and
    /// unregisters the device (STOP_DEV/DEL_DEV via ~Ctrl).
    void stop() noexcept;

private:
    Device() = default;

    DeviceParams params_;
    source::BlobSourcePtr src_;
    std::unique_ptr<Ctrl> ctrl_;
    uint32_t dev_id_ = 0;
    bool started_ = false;
    /// Live device capacity in bytes; the resize grow-only baseline.
    /// Atomic: resize_blocking runs on a spawn_blocking pool thread
    /// while the device command loop reads size_bytes() on a worker.
    std::atomic<uint64_t> cur_bytes_{0};

    std::atomic<bool> stop_{false};
    // Coroutines that dereference queues_/src_ (bridges + per-IO
    // handlers). stop() drains this before ~Queue may run.
    std::atomic<int> io_tasks_running_{0};
    std::vector<std::unique_ptr<Queue>> queues_;
    std::vector<std::thread> threads_;
};

}  // namespace obd::ublk
