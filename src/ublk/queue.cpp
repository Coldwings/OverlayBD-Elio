// ublk per-queue data plane. See queue.hpp and ADR-0006.
#include "ublk/queue.hpp"

#include "common/errors.hpp"
#include "ublk/ctrl.hpp"

#include <elio/log/macros.hpp>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>

namespace obd::ublk {

namespace {

// Data cookies contain a fresh command generation and the low 16-bit tag.
// Poll cookies use a disjoint namespace.
constexpr uint64_t kPollMarker = 1ULL << 63;

void write_efd(int efd) noexcept {
    const uint64_t one = 1;
    ssize_t r;
    do { r = ::write(efd, &one, sizeof(one)); } while (r < 0 && errno == EINTR);
    (void)r;  // EAGAIN (saturated counter) is harmless: a wakeup is pending
}

}  // namespace

Queue::Queue(uint32_t dev_id, uint16_t q_id, uint16_t depth,
             uint32_t max_io_buf_bytes)
    : dev_id_(dev_id),
      q_id_(q_id),
      depth_(depth),
      max_io_buf_bytes_(max_io_buf_bytes) {
    if (depth == 0 || depth > UBLK_MAX_QUEUE_DEPTH) {
        throw error(EINVAL, "invalid ublk queue depth");
    }
}

Queue::~Queue() {
    while (!retirement().complete)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    close_retired();
    ::free(io_bufs_);
    if (elio_efd_ >= 0) ::close(elio_efd_);
    if (done_efd_ >= 0) ::close(done_efd_);
}

void Queue::close_retired() noexcept {
    if (!retirement().complete) return;
    if (ring_ok_) {
        io_uring_queue_exit(&ring_);
        ring_ok_ = false;
    }
    if (cmd_buf_ && cmd_buf_ != MAP_FAILED) {
        ::munmap(cmd_buf_, cmd_buf_len_);
        cmd_buf_ = nullptr;
    }
    if (ublkc_fd_ >= 0) {
        const int closed = ::close(ublkc_fd_);
#ifdef OBD_TEST_HOOKS
        if (test_hooks_) test_hooks_->observe({.kind = detail::StartupTestKind::cdev_closed,
            .queue = q_id_, .result = closed < 0 ? -errno : 0,
            .value = static_cast<uint64_t>(ublkc_fd_)});
#endif
        (void)closed;
        ublkc_fd_ = -1;
    }
}

void Queue::open(int shared_cdev_fd) {
    if (shared_cdev_fd >= 0) {
        ublkc_fd_ = ::fcntl(shared_cdev_fd, F_DUPFD_CLOEXEC, 0);
        if (ublkc_fd_ < 0) {
            throw_errno(errno, "cannot duplicate ublk char device for queue " +
                                   std::to_string(q_id_));
        }
    } else {
        const std::string path = Ctrl::cdev_path(dev_id_);
        ublkc_fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (ublkc_fd_ < 0) {
            throw_errno(errno, "cannot open " + path);
        }
    }

    cmd_buf_len_ = cmd_buf_size(depth_, sizeof(ublksrv_io_desc));
    const uint64_t offset =
        uint64_t{q_id_} * cmd_buf_stride(sizeof(ublksrv_io_desc));
    void* m = ::mmap(nullptr, cmd_buf_len_, PROT_READ, MAP_SHARED | MAP_POPULATE,
                     ublkc_fd_, static_cast<off_t>(offset));
    if (m == MAP_FAILED) {
        throw_errno(errno, "cannot mmap ublk command buffer for queue " +
                               std::to_string(q_id_));
    }
    cmd_buf_ = static_cast<ublksrv_io_desc*>(m);

    const size_t total =
        static_cast<size_t>(depth_) * max_io_buf_bytes_;
    io_bufs_ = static_cast<uint8_t*>(::aligned_alloc(4096, total));
    if (!io_bufs_) {
        throw_errno(ENOMEM, "cannot allocate ublk io buffers");
    }
    std::memset(io_bufs_, 0, total);

    elio_efd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    done_efd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (elio_efd_ < 0 || done_efd_ < 0) {
        throw_errno(errno, "cannot create ublk bridge eventfds");
    }

}

void Queue::init_ring() {
    // Must run on the queue thread (the first statement of run()), for
    // two chained reasons, both verified against real-kernel E2E
    // failures:
    //
    // 1. UBLK_F_URING_CMD_COMP_IN_TASK (dev_info_flags) makes the
    //    driver complete uring-cmds as task_work of the issuing task.
    //    A task_work only runs inside io_uring_enter when the ring has
    //    IORING_SETUP_DEFER_TASKRUN; without it a queue thread blocked
    //    in io_cqring_wait never reaps FETCH completions (observed: the
    //    START_DEV partition scan deadlocked in folio_wait_bit_common
    //    because its read was never serviced).
    // 2. DEFER_TASKRUN requires IORING_SETUP_SINGLE_ISSUER, and
    //    SINGLE_ISSUER binds the creating task as the only legal
    //    submitter — so the ring must be created by the queue thread
    //    itself, not by the device-creating Elio worker (otherwise
    //    every queue-thread io_uring_enter fails with EEXIST).
    io_uring_params ring_params {};
    ring_params.flags = IORING_SETUP_SINGLE_ISSUER |
                        IORING_SETUP_DEFER_TASKRUN;
    const int rc = io_uring_queue_init_params(depth_ * 2, &ring_, &ring_params);
    if (rc < 0) {
        throw_errno(-rc, "cannot create ublk queue io_uring");
    }
    ring_ok_ = true;
}

void* Queue::io_buf(uint16_t tag) noexcept {
    return io_bufs_ + static_cast<size_t>(tag) * max_io_buf_bytes_;
}

bool Queue::try_pop_request(IoRequest& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (releasing_.load()) return false;
    if (pending_.empty()) return false;
    out = pending_.front();
    pending_.pop_front();
    return true;
}

void Queue::push_completion(uint16_t tag, int32_t result) {
    {
        std::lock_guard<std::mutex> lk(mu_);
#ifdef OBD_TEST_HOOKS
        if (completion_enqueue_hook_for_test_) completion_enqueue_hook_for_test_();
#endif
        done_.emplace_back(tag, result);
    }
    write_efd(done_efd_);
}

void Queue::notify_elio() noexcept {
    write_efd(elio_efd_);
}

void Queue::wakeup() noexcept {
    write_efd(done_efd_);
}

void Queue::prep_io_cmd(uint32_t cmd_op, uint16_t tag, int32_t result) {
    if (tag >= raw_.size() || raw_[tag].pending ||
        next_cookie_ >= (uint64_t{1} << 47)) {
        debt_known_ = false;
        fail(EPROTO);
        return;
    }
    if (kernel_stopped_.load()) return;
    auto* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        const int rc = io_uring_submit(&ring_);
        if (rc < 0 && rc != -EINTR) fail(-rc);
        sqe = io_uring_get_sqe(&ring_);
        if (!sqe) { fail(EAGAIN); return; }
    }
    io_uring_prep_rw(IORING_OP_URING_CMD, sqe, ublkc_fd_, nullptr, 0, 0);
    sqe->cmd_op = cmd_op;
    auto* cmd = reinterpret_cast<ublksrv_io_cmd*>(sqe->cmd);
    cmd->q_id = q_id_; cmd->tag = tag; cmd->result = result;
    cmd->addr = reinterpret_cast<uint64_t>(io_buf(tag));
    auto& slot = raw_[tag];
    slot.cookie = (next_cookie_++ << 16) | tag;
    slot.pending = true; // Includes prepared/ambiguous publication.
    slot.completing = cmd_op == UBLK_U_IO_COMMIT_AND_FETCH_REQ;
    slot.delivered = false; // The pending command now covers this obligation.
    slot.reply_ready = false;
    sqe->user_data = slot.cookie;
    ++raw_published_;
#ifdef OBD_TEST_HOOKS
    if (test_hooks_) test_hooks_->observe({.kind = detail::StartupTestKind::raw_prepared,
        .queue = q_id_, .opcode = cmd_op, .result = result, .cookie = slot.cookie});
#endif
}

void Queue::arm_done_poll() {
    if (poll_armed_ || releasing_.load() || failed_.load()) return;
    auto* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return;
    io_uring_prep_poll_add(sqe, done_efd_, POLLIN);
    poll_cookie_ = kPollMarker | next_cookie_++;
    sqe->user_data = poll_cookie_;
    poll_armed_ = true;
    ++raw_published_;
#ifdef OBD_TEST_HOOKS
    if (test_hooks_) test_hooks_->observe({.kind = detail::StartupTestKind::raw_prepared,
        .queue = q_id_, .opcode = IORING_OP_POLL_ADD, .cookie = poll_cookie_});
#endif
}

void Queue::drain_done() {
    uint64_t cnt;
    ssize_t consumed;
    do {
        consumed = ::read(done_efd_, &cnt, sizeof(cnt));
    } while (consumed < 0 && errno == EINTR);
    if (consumed < 0 && errno != EAGAIN) fail(errno);
    else if (consumed >= 0 && consumed != static_cast<ssize_t>(sizeof(cnt))) fail(EIO);
    std::deque<std::pair<uint16_t, int32_t>> batch;
    {
        std::lock_guard lock(mu_);
        batch.swap(done_);
    }
    for (const auto& [tag, result] : batch) {
        if (tag >= raw_.size() || !raw_[tag].delivered ||
            raw_[tag].reply_ready || raw_[tag].pending) {
            debt_known_ = false;
            fail(EPROTO);
            continue;
        }
        raw_[tag].reply = result;
        raw_[tag].reply_ready = true;
        prep_io_cmd(UBLK_U_IO_COMMIT_AND_FETCH_REQ, tag, result);
    }
}

void Queue::dispatch_cqe(const io_uring_cqe* cqe) {
    const uint16_t tag = static_cast<uint16_t>(cqe->user_data & 0xffff);
    if (tag >= raw_.size() || !raw_[tag].pending ||
        raw_[tag].cookie != cqe->user_data || (cqe->flags & IORING_CQE_F_MORE)) {
        debt_known_ = false;
        fail(EPROTO);
        return;
    }
    auto& slot = raw_[tag];
    slot.pending = false;
    ++raw_retired_;
    if (cqe->res < 0) {
        // Rejection of COMMIT is not proof that its previous server-owned IO
        // retired. ENODEV is the supported driver's accepted-command abort.
        if (slot.completing && cqe->res != -ENODEV) {
            slot.delivered = true;
            debt_known_ = false;
        }
        slot.completing = false;
        if (cqe->res != -ENODEV && cqe->res != -EINTR && cqe->res != -ECANCELED)
            fail(-cqe->res);
        return;
    }
    slot.completing = false;
    slot.delivered = true;
    if (releasing_.load() || failed_.load()) {
        slot.reply = -ECANCELED;
        slot.reply_ready = true;
        return;
    }
    const auto& iod = cmd_buf_[tag];
    IoRequest req;
    req.tag = tag; req.op = ublksrv_get_op(&iod);
    req.flags = ublksrv_get_flags(&iod);
    req.start_sector = iod.start_sector; req.nr_sectors = iod.nr_sectors;
#ifdef OBD_TEST_HOOKS
    if (test_hooks_) test_hooks_->observe({.kind = detail::StartupTestKind::request_delivered,
        .queue = q_id_, .opcode = req.op, .cookie = cqe->user_data,
        .buffer = reinterpret_cast<uintptr_t>(io_buf(tag)), .offset = req.byte_offset(),
        .length = req.byte_len(), .tag = tag});
#endif
    try {
        std::lock_guard lock(mu_);
        pending_.push_back(req);
    } catch (...) {
        slot.reply = -ENOMEM;
        slot.reply_ready = true;
        throw;
    }
}

void Queue::prepare_replies() {
    // Cold maintenance only; normal completion batches touch just their tags.
    for (uint16_t tag = 0; tag < depth_; ++tag) {
        auto& slot = raw_[tag];
        if (slot.reply_ready && !slot.pending && !kernel_stopped_.load())
            prep_io_cmd(UBLK_U_IO_COMMIT_AND_FETCH_REQ, tag, slot.reply);
    }
}

void Queue::reject_pending() {
    std::lock_guard lock(mu_);
    while (!pending_.empty()) {
        const auto tag = pending_.front().tag;
        pending_.pop_front();
        if (tag >= raw_.size() || !raw_[tag].delivered) {
            debt_known_ = false;
            fail(EPROTO);
            continue;
        }
        raw_[tag].reply = -ECANCELED;
        raw_[tag].reply_ready = true;
    }
}

void Queue::recover_lost_replies() noexcept {
    // The ordinary completion queue may have failed to allocate. Only after
    // all source/bridge frames are destroyed can an absent completion mean
    // failure rather than a source still borrowing this tag's buffer.
    if (!sources_drained_.load()) return;
    for (auto& slot : raw_) {
        if (slot.delivered && !slot.pending && !slot.reply_ready) {
            slot.reply = -EIO;
            slot.reply_ready = true;
        }
    }
}

bool Queue::kernel_retired() const noexcept {
    if (!debt_known_ || poll_armed_ || raw_published_ != raw_retired_) return false;
    for (const auto& slot : raw_)
        if (slot.pending || slot.delivered || slot.reply_ready) return false;
    return true;
}

void Queue::publish_retirement() noexcept {
    uint32_t delivered = 0;
    for (const auto& slot : raw_) if (slot.delivered) ++delivered;
    retirement_version_.fetch_add(1);
    published_snapshot_.store(raw_published_);
    retired_snapshot_.store(raw_retired_);
    delivered_snapshot_.store(delivered);
    known_snapshot_.store(debt_known_);
    complete_snapshot_.store(kernel_retired());
    retirement_version_.fetch_add(1);
}

Queue::Retirement Queue::retirement() const noexcept {
    for (;;) {
        const auto version = retirement_version_.load();
        if (version & 1) continue;
        const Retirement result{published_snapshot_.load(), retired_snapshot_.load(),
                                delivered_snapshot_.load(), known_snapshot_.load(),
                                complete_snapshot_.load()};
        if (version == retirement_version_.load()) return result;
    }
}

void Queue::run(std::atomic<bool>& stop) {
    std::vector<io_uring_cqe*> cqes;
    try {
        init_ring();
        raw_.resize(depth_);
        cqes.resize(depth_ * 2);
    } catch (...) {
        fail(EIO);
        owner_exited_.store(true);
        return; // No SQE has been prepared or published.
    }
    setup_ready_.store(true);
    while (publication_gate_.load() == 0) publication_gate_.wait(0);
    if (publication_gate_.load() == 2) {
        owner_exited_.store(true);
        return;
    }
    // From the first prepared command onwards this owner cannot unwind.
    complete_snapshot_.store(false);
    for (uint16_t tag = 0; tag < depth_; ++tag) {
        prep_io_cmd(UBLK_U_IO_FETCH_REQ, tag, 0);
        if (failed_.load()) break;
    }
#ifdef OBD_TEST_HOOKS
    try {
        if (test_hooks_) {
            // Expose actual prepared debt while the test holds submission.
            publish_retirement();
            test_hooks_->before_initial_submit(q_id_);
        }
        if (initial_submit_hook_for_test_) initial_submit_hook_for_test_();
    } catch (...) {
        fail(EIO);
    }
#endif
    arm_done_poll();
    publish_retirement();
    for (;;) {
        const bool maintenance = releasing_.load() || failed_.load() ||
                                 stop.load(std::memory_order_relaxed);
        try {
            const bool initial_batch = !initial_fetches_issued_.load();
            const int ret = (maintenance || initial_batch)
                ? io_uring_submit(&ring_) : io_uring_submit_and_wait(&ring_, 1);
            if (ret < 0 && ret != -EINTR && ret != -ETIME) fail(-ret);
            // The initial commands have flags=0: no ASYNC, LINK or DRAIN.
            // This non-SQPOLL ring issues them on this owner under uring_lock.
            // Linux 6.6 ublk FETCH cannot return EAGAIN for io-wq retry.
            // Consequently SQ consumption after submit returns fences driver
            // issuance of this batch. It does NOT prove acceptance/readiness:
            // a rejected FETCH still needs its original CQE. GETEVENTS alone
            // would not fence deferred/io-wq work if these constraints changed.
            if (initial_batch && ret >= 0 && io_uring_sq_ready(&ring_) == 0) {
#ifdef OBD_TEST_HOOKS
                if (test_hooks_) test_hooks_->observe({.kind = detail::StartupTestKind::initial_issued,
                    .queue = q_id_, .result = ret, .value = raw_published_});
#endif
                // Publish only after recording the issuance fence: release may
                // submit STOP as soon as it observes this flag.
                initial_fetches_issued_.store(true);
            }
            if (maintenance) {
                const int entered = io_uring_enter(ring_.ring_fd, 0, 0,
                                                   IORING_ENTER_GETEVENTS, nullptr);
                if (entered < 0 && entered != -EINTR) fail(-entered);
            }
            bool new_requests = false;
            for (;;) {
                const unsigned n = io_uring_peek_batch_cqe(
                    &ring_, cqes.data(), static_cast<unsigned>(cqes.size()));
                if (n == 0) break;
                for (unsigned i = 0; i < n; ++i) {
                    auto* cqe = cqes[i];
#ifdef OBD_TEST_HOOKS
                    if (test_hooks_) test_hooks_->observe({.kind = detail::StartupTestKind::raw_cqe,
                        .queue = q_id_, .result = cqe->res, .flags = cqe->flags,
                        .cookie = cqe->user_data});
#endif
                    try {
                        if (cqe->user_data & kPollMarker) {
                            if (!poll_armed_ || cqe->user_data != poll_cookie_ ||
                                (cqe->flags & IORING_CQE_F_MORE)) {
                                debt_known_ = false;
                                fail(EPROTO);
                            } else {
                                poll_armed_ = false;
                                ++raw_retired_;
                                if (cqe->res < 0) fail(-cqe->res);
                                drain_done();
                            }
                        } else {
                            dispatch_cqe(cqe);
                            new_requests = true;
                        }
                    } catch (...) {
                        fail(EIO);
                    }
                    // A dispatch failure consumes only this CQE; it cannot be
                    // replayed against a new generation of the same tag.
                    io_uring_cqe_seen(&ring_, cqe);
                }
            }
            if (new_requests) notify_elio();
            if (maintenance || failed_.load()) {
                reject_pending();
                drain_done();
                recover_lost_replies();
                prepare_replies();
            }
            arm_done_poll();
        } catch (...) {
            fail(EIO);
        }
        if (maintenance || failed_.load()) {
            publish_retirement();
            if (exit_requested_.load() && kernel_retired()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    publish_retirement();
    owner_exited_.store(true);
}

} // namespace obd::ublk
