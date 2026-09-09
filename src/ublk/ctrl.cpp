// ublk control plane. See ctrl.hpp.
#include "ublk/ctrl.hpp"

#include "common/errors.hpp"

#include <elio/log/macros.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cstring>

namespace obd::ublk {

std::string Ctrl::cdev_path(uint32_t dev_id) {
    return "/dev/ublkc" + std::to_string(dev_id);
}

std::string Ctrl::bdev_path(uint32_t dev_id) {
    return "/dev/ublkb" + std::to_string(dev_id);
}

Ctrl::Ctrl() {
    fd_ = ::open("/dev/ublk-control", O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        throw_errno(errno, "cannot open /dev/ublk-control (is ublk_drv "
                           "loaded and accessible?)");
    }
    io_uring_params params {};
    // The driver requires SQE128 for control commands; CQE32 pairs with
    // it in libublksrv, keep them together.
    params.flags = IORING_SETUP_SQE128 | IORING_SETUP_CQE32;
    const int ret = io_uring_queue_init_params(8, &ring_, &params);
    if (ret < 0) {
        const int e = -ret;
        ::close(fd_);
        fd_ = -1;
        throw_errno(e, "cannot init ublk control io_uring");
    }
}

Ctrl::~Ctrl() {
    if (added_dev_ >= 0) {
        stop_dev(static_cast<uint32_t>(added_dev_));
        del_dev(static_cast<uint32_t>(added_dev_));
    }
    io_uring_queue_exit(&ring_);
    if (fd_ >= 0) ::close(fd_);
}

int Ctrl::ctrl_cmd_raw(uint32_t cmd_op, uint32_t dev_id,
                       uint16_t queue_id, void* data, uint16_t len,
                       uint64_t data0) noexcept {
    ublksrv_ctrl_cmd cmd {};
    cmd.dev_id = dev_id;
    cmd.queue_id = queue_id;
    cmd.len = len;
    cmd.addr = reinterpret_cast<uint64_t>(data);
    cmd.data[0] = data0;

    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) return -EBUSY;
    sqe->opcode = IORING_OP_URING_CMD;
    sqe->fd = fd_;
    sqe->cmd_op = cmd_op;
    std::memcpy(sqe->cmd, &cmd, sizeof(cmd));
    sqe->user_data = 0;

    ELIO_LOG_INFO("ublk ctrl cmd {:#x} dev {} submitting", cmd_op, dev_id);
    const int submitted = io_uring_submit_and_wait(&ring_, 1);
    if (submitted < 0) {
        ELIO_LOG_INFO("ublk ctrl cmd {:#x} dev {} submit failed {}", cmd_op,
                      dev_id, submitted);
        return -errno;
    }
    io_uring_cqe* cqe = nullptr;
    int res = -EIO;
    if (io_uring_peek_cqe(&ring_, &cqe) == 0 && cqe != nullptr) {
        res = cqe->res;
        io_uring_cqe_seen(&ring_, cqe);
    }
    ELIO_LOG_INFO("ublk ctrl cmd {:#x} dev {} -> {}", cmd_op, dev_id, res);
    return res;
}

void Ctrl::ctrl_cmd(uint32_t cmd_op, uint32_t dev_id, uint16_t queue_id,
                    void* data, uint16_t len, uint64_t data0,
                    const char* what) {
    const int res = ctrl_cmd_raw(cmd_op, dev_id, queue_id, data, len,
                                 data0);
    if (res < 0) {
        throw_errno(-res, std::string("ublk ctrl: ") + what + " failed");
    }
}

uint64_t dev_info_flags(const DeviceParams& p) {
    uint64_t flags = UBLK_F_URING_CMD_COMP_IN_TASK;
    if (p.enable_recovery) {
        flags |= UBLK_F_USER_RECOVERY | UBLK_F_USER_RECOVERY_REISSUE;
    }
    return flags;
}

uint32_t Ctrl::add_dev(const DeviceParams& p) {
    ublksrv_ctrl_dev_info info {};
    info.nr_hw_queues = p.nr_queues;
    info.queue_depth = p.queue_depth;
    // NOTE: io_desc_size only exists on newer headers; when absent the
    // driver defaults to sizeof(struct ublksrv_io_desc), which is what we
    // program against (uapi_compat.hpp static_asserts).
    info.max_io_buf_bytes = p.max_io_buf_bytes;
    info.dev_id = p.dev_id == UINT32_MAX ? static_cast<uint32_t>(-1)
                                         : p.dev_id;
    info.ublksrv_pid = static_cast<int32_t>(::getpid());
    info.flags = dev_info_flags(p);

    ublksrv_ctrl_cmd cmd {};
    cmd.queue_id = static_cast<uint16_t>(-1);
    cmd.len = sizeof(info);
    cmd.addr = reinterpret_cast<uint64_t>(&info);
    for (;;) {
        cmd.dev_id = info.dev_id;
        const int res = ctrl_cmd_raw(UBLK_U_CMD_ADD_DEV, info.dev_id,
                                     static_cast<uint16_t>(-1), &info,
                                     sizeof(info), 0);
        if (res == 0) break;
        const int e = -res;
        if (e == EINVAL && p.enable_recovery &&
            (info.flags & UBLK_F_USER_RECOVERY) != 0) {
            // Older kernel without USER_RECOVERY: degrade to a
            // non-recoverable device rather than failing creation.
            ELIO_LOG_WARNING(
                "ublk ADD_DEV rejected USER_RECOVERY flags; creating "
                "device without crash recovery");
            info.flags &= ~(UBLK_F_USER_RECOVERY |
                            UBLK_F_USER_RECOVERY_REISSUE);
            continue;
        }
        throw_errno(e, "ublk ctrl: ADD_DEV failed");
    }
    added_dev_ = static_cast<int>(info.dev_id);
    return info.dev_id;
}

void Ctrl::set_params(uint32_t dev_id, const DeviceParams& p) {
    ublk_params params {};
    params.len = sizeof(params);
    params.types = UBLK_PARAM_TYPE_BASIC;
    params.basic.attrs = UBLK_ATTR_VOLATILE_CACHE;
    if (p.read_only) params.basic.attrs |= UBLK_ATTR_READ_ONLY;
    params.basic.logical_bs_shift = p.logical_bs_shift;
    params.basic.physical_bs_shift = p.physical_bs_shift;
    params.basic.io_opt_shift = p.physical_bs_shift;
    params.basic.io_min_shift = p.logical_bs_shift;
    params.basic.max_sectors = p.max_sectors;
    params.basic.dev_sectors = p.dev_sectors;
    if (!p.read_only) {
        // ADR-0009: discard/write-zeroes are served by the writable upper.
        // Sector granularity matches the layer contract (512B).
        params.types |= UBLK_PARAM_TYPE_DISCARD;
        params.discard.discard_alignment = 512;
        params.discard.discard_granularity = 512;
        params.discard.max_discard_sectors = p.max_sectors;
        params.discard.max_write_zeroes_sectors = p.max_sectors;
        params.discard.max_discard_segments = 1;
    }
    // read-only: discard limits stay 0, so the kernel never issues them.
    ctrl_cmd(UBLK_U_CMD_SET_PARAMS, dev_id, static_cast<uint16_t>(-1),
             &params, sizeof(params), 0, "SET_PARAMS");
}

void Ctrl::start_dev(uint32_t dev_id) {
    ctrl_cmd(UBLK_U_CMD_START_DEV, dev_id, static_cast<uint16_t>(-1),
             nullptr, 0, static_cast<uint64_t>(::getpid()), "START_DEV");
}

void Ctrl::start_user_recovery(uint32_t dev_id) {
    ctrl_cmd(UBLK_U_CMD_START_USER_RECOVERY, dev_id,
             static_cast<uint16_t>(-1), nullptr, 0, 0,
             "START_USER_RECOVERY");
}

void Ctrl::update_size(uint32_t dev_id, uint64_t sectors) {
    // D3 grow-only online resize (UBLK_U_CMD_UPDATE_SIZE): the new size
    // rides cmd.data[0], in sectors (kernel ABI, ublk_cmd.h). No data
    // buffer, no queue. A kernel without the command (it landed in the
    // 6.16 development cycle) rejects it with EOPNOTSUPP, reported as a
    // clean error by the caller.
    ctrl_cmd(UBLK_U_CMD_UPDATE_SIZE, dev_id, static_cast<uint16_t>(-1),
             nullptr, 0, sectors, "UPDATE_SIZE");
}

ublk_params Ctrl::get_params(uint32_t dev_id) {
    ublk_params params {};
    params.len = sizeof(params);
    // The driver fills `types` and the param blocks (basic at minimum)
    // into the data buffer.
    ctrl_cmd(UBLK_U_CMD_GET_PARAMS, dev_id, static_cast<uint16_t>(-1),
             &params, sizeof(params), 0, "GET_PARAMS");
    return params;
}

void Ctrl::end_user_recovery(uint32_t dev_id) {
    // The driver requires data[0] == ub->ublksrv_tgid, which is the
    // tgid recorded when the new daemon opened /dev/ublkcN (i.e. our
    // own pid); a zero data[0] is rejected with EINVAL.
    ctrl_cmd(UBLK_U_CMD_END_USER_RECOVERY, dev_id,
             static_cast<uint16_t>(-1), nullptr, 0,
             static_cast<uint64_t>(getpid()),
             "END_USER_RECOVERY");
}

void Ctrl::stop_dev(uint32_t dev_id) noexcept {
    ctrl_cmd_raw(UBLK_U_CMD_STOP_DEV, dev_id, static_cast<uint16_t>(-1),
                 nullptr, 0, 0);
}

void Ctrl::del_dev(uint32_t dev_id) noexcept {
    ctrl_cmd_raw(UBLK_U_CMD_DEL_DEV, dev_id, static_cast<uint16_t>(-1),
                 nullptr, 0, 0);
}

}  // namespace obd::ublk
