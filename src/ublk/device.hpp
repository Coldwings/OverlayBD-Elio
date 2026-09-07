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

    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    uint32_t dev_id() const noexcept { return dev_id_; }
    std::string bdev_path() const { return Ctrl::bdev_path(dev_id_); }

    /// True once the kernel gendisk is live (START_DEV succeeded).
    bool started() const noexcept { return started_; }

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

    std::atomic<bool> stop_{false};
    std::vector<std::unique_ptr<Queue>> queues_;
    std::vector<std::thread> threads_;
};

}  // namespace obd::ublk
