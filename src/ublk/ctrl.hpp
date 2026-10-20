// ublk control plane: /dev/ublk-control commands driving the device
// lifecycle ADD_DEV → SET_PARAMS → (queues start fetching) → START_DEV
// → … → STOP_DEV → DEL_DEV. Blocking cold path: coroutine callers must
// offload via elio::spawn_blocking (kernel commands may sleep on our
// own data plane, e.g. the START_DEV partition scan).
//
// NOTE: ublk-control has never had an unlocked_ioctl handler — control
// commands are IORING_OP_URING_CMD on the control fd, with SQE128
// (ublk_ctrl_uring_cmd rejects anything else). This is true since the
// driver first merged (v6.0); libublksrv works the same way.
#pragma once

#include "ublk/uapi_compat.hpp"
#include "ublk/startup_test_hooks.hpp"

#include <liburing.h>

#include <cstdint>
#include <atomic>
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
    /// ADR-0010: create the device with UBLK_F_USER_RECOVERY{,_REISSUE} so
    /// a crashed server process can be replaced without failing the block
    /// device. Default on; ADD_DEV falls back to no-recovery (with a
    /// warning) when the kernel rejects the flags.
    bool enable_recovery = true;
};

/// Feature flags for ADD_DEV given the params (pure helper, unit-tested
/// without a kernel).
uint64_t dev_info_flags(const DeviceParams& p);

struct CtrlProgress {
    uint32_t opcode = 0;
    int original_result = 0;
    int cancel_result = 0;
    int reason_errno = 0;
    bool original_known = false;
    bool cancel_known = false;
    bool pending = false;
};
struct CtrlObserver {
    void* context = nullptr;
    int (*cancel_reason)(void*, uint32_t) noexcept = nullptr;
    void (*completed)(void*, uint32_t, int) noexcept = nullptr;
    void (*progress)(void*, const CtrlProgress&) noexcept = nullptr;
};

namespace detail {
/// Injectable control-ring boundary for deterministic lifecycle tests. The
/// implementation must provide SQE128 storage and remain alive until Ctrl and
/// all pending commands are gone. No callback may unwind the command loop.
/// Production Ctrl instances use their own liburing ring directly.
class CtrlTransport {
public:
    virtual ~CtrlTransport() = default;
    virtual io_uring_sqe* get_sqe() noexcept = 0;
    virtual unsigned sq_ready() noexcept = 0;
    virtual int submit() noexcept = 0;
    virtual int enter() noexcept = 0;
    virtual int peek_cqe(io_uring_cqe** cqe) noexcept = 0;
    virtual void cqe_seen(io_uring_cqe* cqe) noexcept = 0;
    virtual void idle() noexcept = 0;
};
} // namespace detail

/// One instance per device process; owns the /dev/ublk-control fd and
/// remembers the added device for best-effort cleanup on destruction.
class Ctrl {
public:
    /// Opens /dev/ublk-control. Throws obd::error when unavailable.
    Ctrl();
    /// Borrows a test transport; does not open a kernel fd or ring.
    explicit Ctrl(detail::CtrlTransport& transport) noexcept;
    ~Ctrl();
    Ctrl(const Ctrl&) = delete;
    Ctrl& operator=(const Ctrl&) = delete;

    /// ADD_DEV; returns the driver-assigned device id. Throws obd::error.
    uint32_t add_dev(const DeviceParams& p);

    /// GET_DEV_INFO returns the kernel's actual flags, including whether
    /// ADD_DEV negotiated recovery support. Does not adopt the device.
    /// Blocking cold path; coroutine callers must use spawn_blocking.
    /// Throws obd::error on failure.
    ublksrv_ctrl_dev_info get_dev_info(uint32_t dev_id);

    /// SET_PARAMS (basic + discard). Throws obd::error.
    void set_params(uint32_t dev_id, const DeviceParams& p);

    /// START_DEV with this process as the ublk server. Throws obd::error
    /// (EBUSY until every queue has parked its FETCH commands).
    void start_dev(uint32_t dev_id);

    /// D3 grow-only online resize: tells the driver the device's new
    /// capacity (UBLK_U_CMD_UPDATE_SIZE; `sectors` in 512B units). The
    /// kernel ABI passes the size in cmd.data[0]. Drivers without the
    /// command (added in the 6.16 development cycle) reject it:
    /// pre-6.15 kernels answer ENOTSUPP (524, the control-dispatch
    /// default), 6.15+ kernels answer EOPNOTSUPP (95). Either surfaces
    /// as a thrown obd::error. Throws obd::error on failure.
    void update_size(uint32_t dev_id, uint64_t sectors);

    /// Reads the device's current parameters (UBLK_U_CMD_GET_PARAMS);
    /// `basic.dev_sectors` is the kernel's CURRENT capacity in sectors —
    /// which on a recovery attach may exceed the create-time params (a
    /// grown device keeps its capacity across USER_RECOVERY; the
    /// replacement never re-runs SET_PARAMS). Throws obd::error on
    /// failure.
    ublk_params get_params(uint32_t dev_id);

    /// ADR-0010 recovery handshake for a replacement server process:
    /// START_USER_RECOVERY announces the new server (EBUSY until the old
    /// cdev is closed and the driver is QUIESCED; Device::attach retries
    /// before opening replacement queues). END_USER_RECOVERY
    /// returns the device to live once every queue re-parked its FETCH
    /// commands (EBUSY until then). Throw obd::error.
    void start_user_recovery(uint32_t dev_id);
    void end_user_recovery(uint32_t dev_id);

    /// Remembers an existing (recovered) device for ~Ctrl cleanup.
    void adopt_dev(uint32_t dev_id) noexcept { added_dev_ = static_cast<int>(dev_id); }
    void set_observer(CtrlObserver observer) noexcept { observer_ = observer; }
#ifdef OBD_TEST_HOOKS
    void set_test_hooks(detail::StartupTestHooks* hooks) noexcept { test_hooks_ = hooks; }
#endif
    /// Device performs explicit, proven retirement before closing cdev/DEL.
    void disable_destructor_cleanup() noexcept { managed_lifetime_ = true; }
    int added_dev_id() const noexcept { return added_dev_; }
    bool command_pending() const noexcept { return pending_.load(); }
    int stop_dev_result(uint32_t dev_id) noexcept;
    int delete_dev_once(uint32_t dev_id) noexcept;

    void stop_dev(uint32_t dev_id) noexcept;
    void del_dev(uint32_t dev_id) noexcept;

    static std::string cdev_path(uint32_t dev_id);  // /dev/ublkc<N>
    static std::string bdev_path(uint32_t dev_id);  // /dev/ublkb<N>

private:
    /// Issues one control command; returns 0 or -errno.
    int ctrl_cmd_raw(uint32_t cmd_op, uint32_t dev_id, uint16_t queue_id,
                     void* data, uint16_t len, uint64_t data0) noexcept;
    void ctrl_cmd(uint32_t cmd_op, uint32_t dev_id, uint16_t queue_id,
                  void* data, uint16_t len, uint64_t data0, const char* what);

    int fd_ = -1;
    int added_dev_ = -1;
    bool managed_lifetime_ = false;
    bool delete_attempted_ = false;
    int delete_result_ = 0;
    uint64_t next_cookie_ = 1;
    std::atomic<bool> pending_{false};
    CtrlObserver observer_;
#ifdef OBD_TEST_HOOKS
    detail::StartupTestHooks* test_hooks_ = nullptr;
#endif
    detail::CtrlTransport* transport_ = nullptr;
    // ublk-control has never had an unlocked_ioctl handler: control
    // commands go through IORING_OP_URING_CMD, and the driver requires
    // SQE128 (ublk_ctrl_uring_cmd checks IO_URING_F_SQE128).
    io_uring ring_ {};
};

}  // namespace obd::ublk
