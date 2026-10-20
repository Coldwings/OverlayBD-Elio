// ublk control plane. See ctrl.hpp.
#include "ublk/ctrl.hpp"

#include "common/errors.hpp"

#include <elio/log/macros.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <time.h>

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

Ctrl::Ctrl(detail::CtrlTransport& transport) noexcept : transport_(&transport) {}

Ctrl::~Ctrl() {
    while (pending_.load()) {
        timespec pause{0, 10000000};
        ::nanosleep(&pause, nullptr);
    }
    if (!managed_lifetime_ && added_dev_ >= 0) {
        stop_dev(static_cast<uint32_t>(added_dev_));
        del_dev(static_cast<uint32_t>(added_dev_));
    }
    if (!transport_) io_uring_queue_exit(&ring_);
    if (fd_ >= 0) ::close(fd_);
}

int Ctrl::ctrl_cmd_raw(uint32_t cmd_op, uint32_t dev_id,
                       uint16_t queue_id, void* data, uint16_t len,
                       uint64_t data0) noexcept {
    bool idle = false;
    if (!pending_.compare_exchange_strong(idle, true)) return -EBUSY;
    CtrlProgress state;
    state.opcode = cmd_op;
    state.pending = true;
    const auto report = [&]() noexcept {
        if (observer_.progress) observer_.progress(observer_.context, state);
    };
    auto* sqe = transport_ ? transport_->get_sqe() : io_uring_get_sqe(&ring_);
    if (!sqe) {
        pending_.store(false);
        state.pending = false;
        state.original_known = true;
        state.original_result = -EBUSY;
        report();
        return -EBUSY;
    }
    const uint64_t original = next_cookie_++;
    uint64_t cancellation = 0;
    bool poisoned = false;
    std::memset(sqe, 0, 2 * sizeof(*sqe));
    ublksrv_ctrl_cmd cmd{};
    cmd.dev_id = dev_id; cmd.queue_id = queue_id;
    cmd.len = len; cmd.addr = reinterpret_cast<uint64_t>(data);
    cmd.data[0] = data0;
    sqe->opcode = IORING_OP_URING_CMD;
    sqe->fd = fd_; sqe->cmd_op = cmd_op; sqe->user_data = original;
    std::memcpy(sqe->cmd, &cmd, sizeof(cmd));
    report();
    // From preparation until every matching CQE, no exception, submit error,
    // cancellation result, or timeout can end the payload/ring lifetime.
    for (;;) {
        if (transport_ ? transport_->sq_ready() : io_uring_sq_ready(&ring_)) {
            const int rc = transport_ ? transport_->submit() : io_uring_submit(&ring_);
            if (rc < 0 && rc != -EINTR && !state.reason_errno)
                state.reason_errno = -rc;
        }
        const int entered = transport_ ? transport_->enter() :
            io_uring_enter(ring_.ring_fd, 0, 0, IORING_ENTER_GETEVENTS, nullptr);
        if (entered < 0 && entered != -EINTR && !state.reason_errno)
            state.reason_errno = -entered;
        for (;;) {
            io_uring_cqe* cqe = nullptr;
            const int rc = transport_ ? transport_->peek_cqe(&cqe) :
                io_uring_peek_cqe(&ring_, &cqe);
            if (rc != 0 || !cqe) {
                if (rc != 0 && rc != -EAGAIN && rc != -EINTR && !state.reason_errno)
                    state.reason_errno = -rc;
                break;
            }
            if (cqe->flags & IORING_CQE_F_MORE) {
                poisoned = true;
            } else if (cqe->user_data == original && !state.original_known) {
                state.original_known = true;
                state.original_result = cqe->res;
                if (cqe->res == 0 && cmd_op == UBLK_U_CMD_ADD_DEV)
                    added_dev_ = static_cast<int>(
                        static_cast<ublksrv_ctrl_dev_info*>(data)->dev_id);
                // Ownership commit is at the actual original CQE, before any
                // error/result reporting or return through an await boundary.
                if (observer_.completed)
                    observer_.completed(observer_.context, cmd_op, cqe->res);
            } else if (cancellation && cqe->user_data == cancellation &&
                       !state.cancel_known) {
                state.cancel_known = true;
                state.cancel_result = cqe->res;
            } else {
                poisoned = true;
            }
            if (transport_) transport_->cqe_seen(cqe);
            else io_uring_cqe_seen(&ring_, cqe);
        }
        if (poisoned && !state.reason_errno) state.reason_errno = EPROTO;
        if (!state.original_known && observer_.cancel_reason) {
            const int reason = observer_.cancel_reason(observer_.context, cmd_op);
            if (reason && !state.reason_errno) state.reason_errno = reason;
            if (reason && !cancellation) {
                if (auto* cancel = transport_ ? transport_->get_sqe() :
                                               io_uring_get_sqe(&ring_)) {
                    std::memset(cancel, 0, 2 * sizeof(*cancel));
                    cancellation = next_cookie_++;
                    io_uring_prep_cancel64(cancel, original, 0);
                    cancel->user_data = cancellation;
                }
            }
        }
        if (!poisoned && state.original_known &&
            (!cancellation || state.cancel_known)) break;
        report();
        if (transport_) transport_->idle();
        else {
            timespec pause{0, 1000000};
            ::nanosleep(&pause, nullptr);
        }
    }
    state.pending = false;
    pending_.store(false);
    report();
    if (state.original_result == 0) return 0;
    if (state.reason_errno &&
        (state.original_result == -EINTR || state.original_result == -ECANCELED))
        return -state.reason_errno;
    return state.original_result;
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

    for (;;) {
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

ublksrv_ctrl_dev_info Ctrl::get_dev_info(uint32_t dev_id) {
    ublksrv_ctrl_dev_info info {};
    ctrl_cmd(UBLK_U_CMD_GET_DEV_INFO, dev_id, static_cast<uint16_t>(-1),
             &info, sizeof(info), 0, "GET_DEV_INFO");
    return info;
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
    // 6.16 development cycle) rejects it — ENOTSUPP (524) on pre-6.15
    // kernels, EOPNOTSUPP (95) on 6.15+ — reported as a clean error by
    // the caller.
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

namespace obd::ublk {
int Ctrl::stop_dev_result(uint32_t dev_id) noexcept {
    return ctrl_cmd_raw(UBLK_U_CMD_STOP_DEV, dev_id, UINT16_MAX, nullptr, 0, 0);
}
int Ctrl::delete_dev_once(uint32_t dev_id) noexcept {
    if (delete_attempted_) return delete_result_;
    delete_attempted_ = true;
    // Deletion may already have taken effect on an interrupted/error return.
    // Disarm any numeric-ID destructor retry before sending the command.
    added_dev_ = -1;
    delete_result_ = ctrl_cmd_raw(UBLK_U_CMD_DEL_DEV, dev_id,
                                  UINT16_MAX, nullptr, 0, 0);
    return delete_result_;
}
} // namespace obd::ublk
