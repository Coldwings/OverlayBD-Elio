// ublk control plane. See ctrl.hpp.
#include "ublk/ctrl.hpp"

#include "common/errors.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
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
}

Ctrl::~Ctrl() {
    if (added_dev_ >= 0) {
        stop_dev(static_cast<uint32_t>(added_dev_));
        del_dev(static_cast<uint32_t>(added_dev_));
    }
    if (fd_ >= 0) ::close(fd_);
}

void Ctrl::ctrl_cmd(uint32_t cmd_op, uint32_t dev_id, uint16_t queue_id,
                    void* data, uint16_t len, uint64_t data0,
                    const char* what) {
    ublksrv_ctrl_cmd cmd {};
    cmd.dev_id = dev_id;
    cmd.queue_id = queue_id;
    cmd.len = len;
    cmd.addr = reinterpret_cast<uint64_t>(data);
    cmd.data[0] = data0;
    if (::ioctl(fd_, cmd_op, &cmd) < 0) {
        throw_errno(errno, std::string("ublk ctrl: ") + what + " failed");
    }
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
    info.flags = UBLK_F_URING_CMD_COMP_IN_TASK;

    ublksrv_ctrl_cmd cmd {};
    cmd.dev_id = info.dev_id;
    cmd.queue_id = static_cast<uint16_t>(-1);
    cmd.len = sizeof(info);
    cmd.addr = reinterpret_cast<uint64_t>(&info);
    if (::ioctl(fd_, UBLK_U_CMD_ADD_DEV, &cmd) < 0) {
        throw_errno(errno, "ublk ctrl: ADD_DEV failed");
    }
    added_dev_ = static_cast<int>(info.dev_id);
    return info.dev_id;
}

void Ctrl::set_params(uint32_t dev_id, const DeviceParams& p) {
    ublk_params params {};
    params.len = sizeof(params);
    params.types = UBLK_PARAM_TYPE_BASIC | UBLK_PARAM_TYPE_DISCARD;
    params.basic.attrs = UBLK_ATTR_VOLATILE_CACHE;
    if (p.read_only) params.basic.attrs |= UBLK_ATTR_READ_ONLY;
    params.basic.logical_bs_shift = p.logical_bs_shift;
    params.basic.physical_bs_shift = p.physical_bs_shift;
    params.basic.io_opt_shift = p.physical_bs_shift;
    params.basic.io_min_shift = p.logical_bs_shift;
    params.basic.max_sectors = p.max_sectors;
    params.basic.dev_sectors = p.dev_sectors;
    // discard: not supported by a read-only view (all limits stay 0).
    ctrl_cmd(UBLK_U_CMD_SET_PARAMS, dev_id, static_cast<uint16_t>(-1),
             &params, sizeof(params), 0, "SET_PARAMS");
}

void Ctrl::start_dev(uint32_t dev_id) {
    ctrl_cmd(UBLK_U_CMD_START_DEV, dev_id, static_cast<uint16_t>(-1),
             nullptr, 0, static_cast<uint64_t>(::getpid()), "START_DEV");
}

void Ctrl::stop_dev(uint32_t dev_id) noexcept {
    ublksrv_ctrl_cmd cmd {};
    cmd.dev_id = dev_id;
    cmd.queue_id = static_cast<uint16_t>(-1);
    ::ioctl(fd_, UBLK_U_CMD_STOP_DEV, &cmd);
}

void Ctrl::del_dev(uint32_t dev_id) noexcept {
    ublksrv_ctrl_cmd cmd {};
    cmd.dev_id = dev_id;
    cmd.queue_id = static_cast<uint16_t>(-1);
    ::ioctl(fd_, UBLK_U_CMD_DEL_DEV, &cmd);
}

}  // namespace obd::ublk
