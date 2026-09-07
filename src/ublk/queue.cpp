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
#include <vector>

namespace obd::ublk {

namespace {

// user_data encoding: bit 63 = done_efd POLL marker, bit 62 = commit cqe,
// low 16 bits = tag.
constexpr uint64_t kPollMarker = 1ULL << 63;
constexpr uint64_t kCommitBit = 1ULL << 62;

void write_efd(int efd) noexcept {
    const uint64_t one = 1;
    const ssize_t r = ::write(efd, &one, sizeof(one));
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
    if (ring_ok_) io_uring_queue_exit(&ring_);
    if (cmd_buf_ && cmd_buf_ != MAP_FAILED) {
        ::munmap(cmd_buf_, cmd_buf_len_);
    }
    ::free(io_bufs_);
    if (ublkc_fd_ >= 0) ::close(ublkc_fd_);
    if (elio_efd_ >= 0) ::close(elio_efd_);
    if (done_efd_ >= 0) ::close(done_efd_);
}

void Queue::open() {
    const std::string path = Ctrl::cdev_path(dev_id_);
    ublkc_fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (ublkc_fd_ < 0) {
        throw_errno(errno, "cannot open " + path);
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

    if (io_uring_queue_init(depth_ * 2, &ring_, 0) < 0) {
        throw_errno(errno, "cannot create ublk queue io_uring");
    }
    ring_ok_ = true;
}

void* Queue::io_buf(uint16_t tag) noexcept {
    return io_bufs_ + static_cast<size_t>(tag) * max_io_buf_bytes_;
}

bool Queue::try_pop_request(IoRequest& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (pending_.empty()) return false;
    out = pending_.front();
    pending_.pop_front();
    return true;
}

void Queue::push_completion(uint16_t tag, int32_t result) {
    {
        std::lock_guard<std::mutex> lk(mu_);
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
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        // Ring full: flush and retry once. Only the queue thread submits,
        // so this cannot livelock.
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            failed_.store(true);
            failure_.store(EIO);
            ELIO_LOG_ERROR("ublk queue {}: ring exhausted", q_id_);
            return;
        }
    }
    io_uring_prep_rw(IORING_OP_URING_CMD, sqe, ublkc_fd_, nullptr, 0, 0);
    sqe->cmd_op = cmd_op;
    auto* cmd = reinterpret_cast<ublksrv_io_cmd*>(sqe->cmd);
    cmd->q_id = q_id_;
    cmd->tag = tag;
    cmd->result = result;
    cmd->addr = reinterpret_cast<uint64_t>(io_buf(tag));
    sqe->user_data = tag | (cmd_op == UBLK_U_IO_COMMIT_AND_FETCH_REQ
                                ? kCommitBit
                                : 0);
}

void Queue::arm_done_poll() {
    if (poll_armed_) return;
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return;  // retried on the next loop pass
    io_uring_prep_poll_add(sqe, done_efd_, POLLIN);
    sqe->user_data = kPollMarker;
    poll_armed_ = true;
}

void Queue::drain_done() {
    uint64_t cnt;
    const ssize_t r = ::read(done_efd_, &cnt, sizeof(cnt));
    (void)r;
    std::deque<std::pair<uint16_t, int32_t>> batch;
    {
        std::lock_guard<std::mutex> lk(mu_);
        batch.swap(done_);
    }
    for (const auto& [tag, result] : batch) {
        prep_io_cmd(UBLK_U_IO_COMMIT_AND_FETCH_REQ, tag, result);
    }
}

void Queue::dispatch_cqe(const io_uring_cqe* cqe) {
    const uint16_t tag = static_cast<uint16_t>(cqe->user_data & 0xffff);
    if (cqe->res < 0) {
        const bool stopped =
            (cqe->res == -ENODEV || cqe->res == -EINTR ||
             cqe->res == -ECANCELED);
        if (!stopped) {
            failed_.store(true);
            failure_.store(-cqe->res);
            ELIO_LOG_ERROR("ublk queue {} tag {} cmd failed: {}", q_id_, tag,
                           std::strerror(-cqe->res));
        }
        return;
    }
    // A FETCH (or the fetch side of COMMIT_AND_FETCH) delivered a new IO:
    // the tag's ublksrv_io_desc is now valid in the shared command buffer.
    const ublksrv_io_desc& iod = cmd_buf_[tag];
    IoRequest req;
    req.tag = tag;
    req.op = ublksrv_get_op(&iod);
    req.flags = ublksrv_get_flags(&iod);
    req.start_sector = iod.start_sector;
    req.nr_sectors = iod.nr_sectors;
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_.push_back(req);
    }
}

void Queue::run(std::atomic<bool>& stop) {
    // Park the initial FETCH for every tag: the driver refuses START_DEV
    // (EBUSY) until all queue tags are waiting for work.
    for (uint16_t tag = 0; tag < depth_; ++tag) {
        prep_io_cmd(UBLK_U_IO_FETCH_REQ, tag, 0);
    }
    arm_done_poll();
    io_uring_submit(&ring_);

    std::vector<io_uring_cqe*> cqes(depth_ * 2);
    while (!stop.load(std::memory_order_relaxed) &&
           !failed_.load(std::memory_order_relaxed)) {
        const int ret = io_uring_submit_and_wait(&ring_, 1);
        if (ret < 0 && ret != -EINTR && ret != -ETIME) {
            failed_.store(true);
            failure_.store(-ret);
            ELIO_LOG_ERROR("ublk queue {}: submit_and_wait: {}", q_id_,
                           std::strerror(-ret));
            break;
        }
        bool new_requests = false;
        for (;;) {
            const unsigned n = io_uring_peek_batch_cqe(
                &ring_, cqes.data(), static_cast<unsigned>(cqes.size()));
            if (n == 0) break;
            for (unsigned i = 0; i < n; ++i) {
                const io_uring_cqe* cqe = cqes[i];
                if (cqe->user_data == kPollMarker) {
                    poll_armed_ = false;
                    drain_done();
                } else {
                    dispatch_cqe(cqe);
                    new_requests = true;
                }
            }
            io_uring_cq_advance(&ring_, n);
        }
        if (new_requests) notify_elio();
        arm_done_poll();
    }
}

}  // namespace obd::ublk
