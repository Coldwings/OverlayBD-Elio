// ublk per-queue data plane (ADR-0006).
//
// Each Queue owns one /dev/ublkc<N> queue: the kernel-shared command buffer
// (mmap, read-only to us), one raw liburing io_uring ring, and the per-tag
// IO buffers. The ring is owned EXCLUSIVELY by the queue thread: only it
// submits FETCH/COMMIT uring-cmds (the kernel pins io->task at FETCH and
// requires COMMIT from the same task).
//
// Cross-thread protocol:
//
//   queue thread --(pending_ + elio_efd)--> Elio bridge coroutines
//   Elio workers --(done_ + done_efd)-----> queue thread
//
// The queue thread additionally parks a POLL_ADD on done_efd in its ring,
// so Elio-side completions wake its io_uring wait without a separate poll
// thread.
#pragma once

#include "ublk/uapi_compat.hpp"

#include <liburing.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

namespace obd::ublk {

/// One IO command extracted from the kernel-shared ublksrv_io_desc.
struct IoRequest {
    uint16_t tag = 0;
    uint8_t op = 0;           // UBLK_IO_OP_*
    uint32_t flags = 0;       // ublksrv_get_flags() (UBLK_IO_F_*)
    uint64_t start_sector = 0;
    uint32_t nr_sectors = 0;

    uint64_t byte_offset() const { return start_sector * 512ULL; }
    uint32_t byte_len() const { return nr_sectors * 512U; }
};

class Queue {
public:
    Queue(uint32_t dev_id, uint16_t q_id, uint16_t depth,
          uint32_t max_io_buf_bytes);
    ~Queue();
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    /// Opens the queue char device, mmaps the command buffer and
    /// allocates the buffers. Throws obd::error on failure. The ring is
    /// NOT created here: see run().
    void open();

    uint16_t q_id() const noexcept { return q_id_; }
    uint16_t depth() const noexcept { return depth_; }

    /// Per-tag IO buffer (daemon vm space; the kernel copies request data
    /// in/out of it). Written by Elio workers for reads.
    void* io_buf(uint16_t tag) noexcept;

    // --- Elio side (any worker thread) -------------------------------------

    int elio_efd() const noexcept { return elio_efd_; }

    /// Pops one pending request; false when empty. Lock-free w.r.t. the
    /// queue thread except for a short mutex section.
    bool try_pop_request(IoRequest& out);

    /// Completes a request: result >= 0 = bytes transferred, < 0 = -errno.
    /// Wakes the queue thread via done_efd.
    void push_completion(uint16_t tag, int32_t result);

    // --- queue thread -------------------------------------------------------

    /// Blocking queue loop: parks FETCH commands, dispatches new requests
    /// to the pending queue, commits finished ones. Returns when `stop`
    /// becomes true (call wakeup() to interrupt the wait) or when the ring
    /// fails (check failed()).
    void run(std::atomic<bool>& stop);

    /// Interrupts the run() wait (e.g. after setting stop).
    void wakeup() noexcept;

    /// Marks the queue failed from outside the queue thread (e.g. an
    /// exception escaping run()).
    void fail(int err) noexcept {
        failure_.store(err);
        failed_.store(true);
    }
    bool failed() const noexcept { return failed_.load(); }
    int failure() const noexcept { return failure_.load(); }

private:
    /// Creates the queue io_uring. Must be called on the queue thread
    /// (SINGLE_ISSUER binds the creator; DEFER_TASKRUN binds the
    /// waiter). Throws obd::error on failure.
    void init_ring();
    void prep_io_cmd(uint32_t cmd_op, uint16_t tag, int32_t result);
    void arm_done_poll();
    void drain_done();
    void dispatch_cqe(const io_uring_cqe* cqe);
    void notify_elio() noexcept;

    uint32_t dev_id_;
    uint16_t q_id_;
    uint16_t depth_;
    uint32_t max_io_buf_bytes_;

    int ublkc_fd_ = -1;
    io_uring ring_ {};
    bool ring_ok_ = false;
    ublksrv_io_desc* cmd_buf_ = nullptr;  // PROT_READ mmap
    uint64_t cmd_buf_len_ = 0;
    uint8_t* io_bufs_ = nullptr;

    int elio_efd_ = -1;  // queue thread → Elio: new pending requests
    int done_efd_ = -1;  // Elio → queue thread: completions ready
    bool poll_armed_ = false;

    std::mutex mu_;
    std::deque<IoRequest> pending_;
    std::deque<std::pair<uint16_t, int32_t>> done_;

    std::atomic<bool> failed_{false};
    std::atomic<int> failure_{0};
};

}  // namespace obd::ublk
