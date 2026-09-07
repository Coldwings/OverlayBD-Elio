// ublk control plane: /dev/ublk-control ioctls driving the device lifecycle
// ADD_DEV → SET_PARAMS → (queues start fetching) → START_DEV → … →
// STOP_DEV → DEL_DEV. Synchronous cold path — plain ioctl()s, never on a
// coroutine hot path.
#pragma once

#include "ublk/uapi_compat.hpp"

#include <cstdint>
#include <string>

namespace obd::ublk {

struct DeviceParams {
    uint32_t dev_id = UINT32_MAX;  // UINT32_MAX = let the driver pick
    uint16_t nr_queues = 1;
    uint16_t queue_depth = 128;
    uint32_t max_io_buf_bytes = 128 * 1024;
    uint64_t dev_sectors = 0;      // image virtual size / 512
    uint8_t logical_bs_shift = 9;    // 512B
    uint8_t physical_bs_shift = 12;  // 4K
    uint32_t max_sectors = 256;    // 128 KiB per request
    bool read_only = true;
};

/// One instance per device process; owns the /dev/ublk-control fd and
/// remembers the added device for best-effort cleanup on destruction.
class Ctrl {
public:
    /// Opens /dev/ublk-control. Throws obd::error when unavailable.
    Ctrl();
    ~Ctrl();
    Ctrl(const Ctrl&) = delete;
    Ctrl& operator=(const Ctrl&) = delete;

    /// ADD_DEV; returns the driver-assigned device id. Throws obd::error.
    uint32_t add_dev(const DeviceParams& p);

    /// SET_PARAMS (basic + discard). Throws obd::error.
    void set_params(uint32_t dev_id, const DeviceParams& p);

    /// START_DEV with this process as the ublk server. Throws obd::error
    /// (EBUSY until every queue has parked its FETCH commands).
    void start_dev(uint32_t dev_id);

    void stop_dev(uint32_t dev_id) noexcept;
    void del_dev(uint32_t dev_id) noexcept;

    static std::string cdev_path(uint32_t dev_id);  // /dev/ublkc<N>
    static std::string bdev_path(uint32_t dev_id);  // /dev/ublkb<N>

private:
    void ctrl_cmd(uint32_t cmd_op, uint32_t dev_id, uint16_t queue_id,
                  void* data, uint16_t len, uint64_t data0, const char* what);

    int fd_ = -1;
    int added_dev_ = -1;
};

}  // namespace obd::ublk
