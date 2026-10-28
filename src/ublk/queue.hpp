// ublk per-queue data plane (ADR-0023).
//
// Each Queue owns one /dev/ublkc<N> queue: the kernel-shared command buffer
// (mmap, read-only to us), one raw liburing io_uring ring, and the per-tag
// IO buffers. The ring is owned EXCLUSIVELY by one fixed Elio worker: only
// coroutines running on that worker submit FETCH/COMMIT uring-cmds (the
// kernel pins io->task at FETCH and requires COMMIT from the same task).
//
// The raw ring registers elio_efd with io_uring. The owner's Elio-native ring
// waits on that eventfd while the raw ring is idle. Normal tag completion stays
// on the owner worker and submits COMMIT_AND_FETCH directly, so it does not
// cross a thread or write an eventfd. Stop/failure and exceptional foreign
// handler retirement write elio_efd to wake the pump.
#pragma once

#include "ublk/startup_test_hooks.hpp"

#include "ublk/uapi_compat.hpp"

#include <liburing.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <deque>
#include <mutex>
#include <sys/types.h>
#include <thread>
#include <vector>
#ifdef OBD_TEST_HOOKS
#include <elio/coro/task.hpp>
#include <functional>
namespace elio::sync { class event; }
#endif

namespace obd::ublk {

namespace detail {

struct SubmissionDrain {
    unsigned remaining = 0;
    int last = 0;
    int error = 0;
};

/// Drain `expected` SQEs after liburing has flushed them to the shared SQ.
/// `submit` is called exactly once for a fresh batch; all remaining progress,
/// including an initial EINTR or partial return, uses direct ring entry so it
/// cannot be lost when liburing's private sqe_head has already advanced.
template<class Submit, class Enter>
SubmissionDrain drain_submissions(unsigned expected, bool fresh,
                                  Submit&& submit, Enter&& enter) {
    SubmissionDrain result{.remaining = expected};
    bool first = fresh;
    while (result.remaining != 0) {
        const int rc = first ? submit() : enter(result.remaining);
        first = false;
        if (rc == -EINTR) continue;
        if (rc < 0) {
            result.error = -rc;
            return result;
        }
        if (rc == 0) {
            result.error = EIO;
            return result;
        }
        if (static_cast<unsigned>(rc) > result.remaining) {
            result.error = EPROTO;
            return result;
        }
        result.last = rc;
        result.remaining -= static_cast<unsigned>(rc);
    }
    return result;
}

struct CqDrain {
    unsigned consumed = 0;
    bool more = false;
};

struct OwnerCqTurn {
    unsigned consumed = 0;
    unsigned enters = 0;
    unsigned hints = 0;
    bool immediate = false;
    int error = 0;
};

/// Execute one bounded raw-CQ turn. A registered eventfd notification can be
/// either the pre-CQE DEFER_TASKRUN hint or the later CQ-tail notification.
/// Every consumed hint is therefore followed by another GETEVENTS entry and
/// CQ drain before this function may return. The hint budget prevents a noisy
/// eventfd from monopolising the fixed worker.
template<class Enter, class Drain, class ReadHint>
OwnerCqTurn drain_owner_cq_turn(unsigned budget, Enter&& enter,
                                Drain&& drain, ReadHint&& read_hint) {
    OwnerCqTurn result;
    if (budget == 0) {
        result.immediate = true;
        return result;
    }

    for (;;) {
        int entered;
        do { entered = enter(); } while (entered == -EINTR);
        ++result.enters;
        if (entered < 0) {
            result.error = -entered;
            return result;
        }

        const auto batch = drain(budget - result.consumed);
        if (batch.consumed > budget - result.consumed) {
            result.error = EPROTO;
            return result;
        }
        result.consumed += batch.consumed;
        if (batch.more || result.consumed == budget) {
            result.immediate = true;
            return result;
        }

        // This point is reached only after satisfying the entry owed by the
        // previously consumed hint. Do not consume another hint once its
        // matching entry would exceed this turn's bound.
        if (result.hints == budget) {
            result.immediate = true;
            return result;
        }
        int hinted;
        do { hinted = read_hint(); } while (hinted == -EINTR);
        if (hinted < 0) {
            result.error = -hinted;
            return result;
        }
        if (hinted == 0) return result;
        ++result.hints;
    }
}

}  // namespace detail

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

#ifdef OBD_TEST_HOOKS
// Deterministic boundary injection, not a claim that a steal was observed.
enum class BridgeResumePoint {
    tag_wait, source_return, bridge_read, request_retry, cleanup, close_retry,
    handlers_join, rejected_return,
};
#endif

class Queue {
public:
    struct Retirement {
        uint64_t published = 0;
        uint64_t retired = 0;
        uint32_t delivered = 0;
        bool known = true;
        bool complete = true;
    };
    Queue(uint32_t dev_id, uint16_t q_id, uint16_t depth,
          uint32_t max_io_buf_bytes);
    ~Queue();
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    /// Opens the device char node, or duplicates the borrowed descriptor
    /// shared_cdev_fd. Each queue owns its descriptor, while all queues of
    /// a Device share one open file description (the driver permits only
    /// one open). Mmaps this queue's command buffer and allocates its
    /// buffers. Throws obd::error on failure. The ring is NOT created here:
    /// production creates it in init_owner_pump(); deterministic legacy tests
    /// create it in run().
    void open(int shared_cdev_fd = -1);

    uint16_t q_id() const noexcept { return q_id_; }
    uint16_t depth() const noexcept { return depth_; }
    int cdev_fd() const noexcept { return ublkc_fd_; }

    /// Per-tag IO buffer (daemon vm space; the kernel copies request data
    /// in/out of it). Written by Elio workers for reads.
    void* io_buf(uint16_t tag) noexcept;

    // --- Same-worker owner pump (ADR-0023) ---------------------------------

    /// Creates and eventfd-registers the raw ring on the current Elio worker.
    /// No ublk command has been prepared when this returns.
    void init_owner_pump(size_t owner_worker);
    /// Claims the scheduler-admitted owner before its first suspension.
    /// False means launch failure already sealed this queue as ownerless.
    bool begin_owner() noexcept;
    /// Seals a queue whose joinable task was never admitted. No ring command
    /// exists, so source drain and raw retirement are immediately complete.
    bool abort_unstarted_owner(int error) noexcept;
    bool owner_started() const noexcept {
        return owner_admission_.load(std::memory_order_acquire) == 1;
    }
    int owner_eventfd() const noexcept { return elio_efd_; }
    bool publication_ready() const noexcept {
        return publication_gate_.load(std::memory_order_acquire) == 1;
    }
    bool publication_aborted() const noexcept {
        return publication_gate_.load(std::memory_order_acquire) == 2;
    }
    /// Separates preparation from synchronous submission so an opt-in
    /// integration fixture can hold the exact issuance boundary without
    /// blocking the fixed worker. Once prepared, the owner must eventually
    /// call submit_owner_fetches even if that fixture wait fails.
    void prepare_owner_fetches();
    void submit_owner_fetches();
#ifdef OBD_TEST_HOOKS
    elio::coro::task<void> await_initial_submit_for_test();
#endif

    /// Consume at most `budget` raw CQEs and queue delivered requests in the
    /// preallocated owner-local request ring. True means CQ work remains.
    bool drive_owner_cq(unsigned budget);

    /// Called by TagEngine only after it restored the owner worker. Validates
    /// the tag obligation, prepares COMMIT_AND_FETCH, and submits it from the
    /// same task identity that issued FETCH.
    void complete_on_owner(uint16_t tag, int32_t result);
    void flush_owner_completions();
    void close_owner_completion_batching();

    /// Reject delivered-but-unadmitted work and drive reply/raw retirement.
    void maintain_owner();
    bool owner_may_exit() const noexcept {
        return exit_requested_.load(std::memory_order_acquire) &&
               kernel_retired();
    }
    void finish_owner() noexcept;
    bool source_handlers_drained() const noexcept {
        return sources_drained_.load(std::memory_order_acquire);
    }
    void mark_source_handlers_drained() noexcept {
        sources_drained_.store(true, std::memory_order_release);
        wakeup();
    }

    // --- Legacy test bridge boundary --------------------------------------

    int elio_efd() const noexcept { return elio_efd_; }

    /// Pops one pending request; false when empty. Lock-free w.r.t. the
    /// queue thread except for a short mutex section.
    bool try_pop_request(IoRequest& out);

    /// Completes a request: result >= 0 = bytes transferred, < 0 = -errno.
    /// Wakes the queue thread via done_efd.
    void push_completion(uint16_t tag, int32_t result);

    // --- queue thread -------------------------------------------------------

    /// Initializes on the real owner, then waits for the publication gate.
    /// After publication, faults enter maintenance rather than exit with raw
    /// kernel loans. Only request_owner_exit() plus proven retirement permits
    /// return. The coordinator must keep this object and source graph alive.
    void run(std::atomic<bool>& stop);
    bool setup_ready() const noexcept { return setup_ready_.load(); }
#ifdef OBD_TEST_HOOKS
    void set_test_hooks(detail::StartupTestHooks* hooks) noexcept { test_hooks_ = hooks; }
#endif
    bool bridge_ready() const noexcept { return bridge_ready_.load(); }
    void mark_bridge_ready() noexcept { bridge_ready_.store(true); }
    void publish_initial_fetches() noexcept {
        publication_gate_.store(1);
        publication_gate_.notify_all();
        wakeup();
    }
    void abort_setup() noexcept {
        publication_gate_.store(2);
        publication_gate_.notify_all();
        wakeup();
    }
    void request_release() noexcept {
        releasing_.store(true);
        wakeup();
    }
    void kernel_stopped() noexcept {
        kernel_stopped_.store(true);
        wakeup();
    }
    void request_owner_exit() noexcept {
        exit_requested_.store(true);
        abort_setup();
        wakeup();
    }
    bool owner_exited() const noexcept { return owner_exited_.load(); }
    /// Owner has finished initial FETCH preparation and synchronously issued
    /// that batch. This is a STOP ordering fence, NOT driver acceptance,
    /// queue readiness, or original-CQE retirement.
    bool initial_fetches_issued() const noexcept {
        return initial_fetches_issued_.load();
    }
    Retirement retirement() const noexcept;
    void sources_drained() noexcept {
        mark_source_handlers_drained();
    }
    /// Only after owner-thread join and positive command retirement. Close
    /// the ring/mapping/cdev before DEL, retaining diagnostic queues/buffers.
    void close_retired() noexcept;

    /// Interrupts the run() wait (e.g. after setting stop).
    void wakeup() noexcept;

    /// Pokes the bridge side (elio_efd_). Called by run() when new
    /// requests arrive and by Device::stop() so parked bridge
    /// coroutines can observe the stop flag.
    void notify_elio() noexcept;

    /// Marks the queue failed from outside the queue thread (e.g. an
    /// exception escaping run()) and interrupts its current wait.
    void fail(int err) noexcept {
        failure_.store(err);
        failed_.store(true);
        wakeup();
    }
    bool failed() const noexcept { return failed_.load(); }
    int failure() const noexcept { return failure_.load(); }

#ifdef OBD_TEST_HOOKS
    const auto& bridge_resume_hook_for_test() const noexcept {
        return bridge_resume_hook_for_test_;
    }
    void observe_bridge_resume_for_test(BridgeResumePoint point) const {
        if (bridge_resume_observer_for_test_) bridge_resume_observer_for_test_(point);
    }
    void notify_bridge_admission_for_test(elio::sync::event& ready) {
        bridge_ready_for_test_ = &ready;
        if (bridge_admission_hook_for_test_) bridge_admission_hook_for_test_();
    }
    void clear_bridge_admission_for_test() noexcept {
        bridge_ready_for_test_ = nullptr;
    }
#endif

private:
    friend struct DeviceTestAccess;
    friend struct QueueRetirementTestAccess;
#ifdef OBD_TEST_HOOKS
    friend struct StartupCoordinatorTestAccess;
#endif
    /// Creates the queue io_uring. Must be called on its final owner task;
    /// SINGLE_ISSUER binds the creator and DEFER_TASKRUN binds local-work
    /// execution to explicit GETEVENTS by that task.
    void init_ring();
    void register_owner_eventfd();
    void assert_owner() const;
    pid_t capture_owner_tid() noexcept;
    void submit_owner();
    void prep_io_cmd(uint32_t cmd_op, uint16_t tag, int32_t result);
    void arm_done_poll();
    void drain_done();
    void dispatch_cqe(const io_uring_cqe* cqe);
    void publish_retirement() noexcept;
    bool kernel_retired() const noexcept;
    void prepare_replies();
    void reject_pending();
    void recover_lost_replies() noexcept;

    struct RawSlot {
        uint64_t cookie = 0;
        bool pending = false;
        bool delivered = false;
        bool completing = false;
        bool reply_ready = false;
        int32_t reply = 0;
    };
    std::vector<RawSlot> raw_;
    std::vector<io_uring_cqe*> owner_cqes_;
    std::vector<IoRequest> owner_pending_;
    uint16_t owner_pending_head_ = 0;
    uint16_t owner_pending_tail_ = 0;
    uint16_t owner_pending_count_ = 0;
    size_t owner_worker_ = static_cast<size_t>(-1);
    pid_t owner_tid_ = -1;
    std::thread::id owner_thread_id_{};
    bool same_owner_pump_ = false;
    uint64_t next_cookie_ = 1;
    uint64_t raw_published_ = 0;
    uint64_t raw_retired_ = 0;
    unsigned owner_submit_remaining_ = 0;
    unsigned delivered_count_ = 0;
    bool owner_completion_pending_ = false;
    bool owner_batching_closed_ = false;
    bool owner_initial_preparing_ = false;
    uint64_t poll_cookie_ = 0;
    bool debt_known_ = true;

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

    int elio_efd_ = -1;  // raw CQ plus stop/failure wake for owner pump
    int done_efd_ = -1;  // Elio → queue thread: completions ready
    bool poll_armed_ = false;
    std::atomic<uint8_t> publication_gate_{0};
    std::atomic<bool> setup_ready_{false};
    std::atomic<bool> bridge_ready_{false};
    std::atomic<bool> releasing_{false};
    std::atomic<bool> kernel_stopped_{false};
    std::atomic<bool> exit_requested_{false};
    std::atomic<bool> owner_exited_{false};
    // Published by the fixed owner while coordinator/control paths may
    // concurrently choose which eventfd wakes this queue. Keep the owner-local
    // topology flag above off the hot-path atomic surface.
    std::atomic<bool> owner_wakeup_on_elio_{false};
    // 0 = unclaimed, 1 = owner body began, 2 = launch rejected/absent.
    std::atomic<uint8_t> owner_admission_{0};
    std::atomic<bool> initial_fetches_issued_{false};
    std::atomic<bool> sources_drained_{false};
    // Updated only at setup/maintenance/exit, never per successful hot IO.
    std::atomic<uint64_t> retirement_version_{0};
    std::atomic<uint64_t> published_snapshot_{0};
    std::atomic<uint64_t> retired_snapshot_{0};
    std::atomic<uint32_t> delivered_snapshot_{0};
    std::atomic<bool> known_snapshot_{true};
    std::atomic<bool> complete_snapshot_{true};

    std::mutex mu_;
    std::deque<IoRequest> pending_;
    std::deque<std::pair<uint16_t, int32_t>> done_;
#ifdef OBD_TEST_HOOKS
    // Inject failure before the potentially allocating completion insertion.
    // Private and per-queue; absent from builds with tests disabled.
    std::function<void()> completion_enqueue_hook_for_test_;
    std::function<void()> bridge_admission_hook_for_test_;
    detail::StartupTestHooks* test_hooks_ = nullptr;
    std::function<void()> initial_submit_hook_for_test_;
    std::function<elio::coro::task<void>(BridgeResumePoint)> bridge_resume_hook_for_test_;
    std::function<void(BridgeResumePoint)> bridge_resume_observer_for_test_;
    elio::sync::event* bridge_ready_for_test_ = nullptr;
    unsigned owner_tid_syscalls_for_test_ = 0;
#endif

    std::atomic<bool> failed_{false};
    std::atomic<int> failure_{0};
};

}  // namespace obd::ublk
