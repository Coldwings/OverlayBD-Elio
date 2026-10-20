// Elio bridge. See elio_bridge.hpp and ADR-0006.
#include "ublk/elio_bridge.hpp"

#include <elio/io/io_awaitables.hpp>
#include <elio/log/macros.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/runtime/affinity.hpp>
#include <elio/coro/task_group.hpp>
#include <elio/sync/event.hpp>
#include <elio/time/timer.hpp>

#include <sys/eventfd.h>

#include <cerrno>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace obd::ublk {

namespace {

// An admitted kernel request needs a completion even if scheduling rejects
// its child before invocation. This token describes the kernel obligation;
// TaskGroup independently owns coroutine/frame lifetime.
class RequestCompletion {
public:
    RequestCompletion(Queue* queue, uint16_t tag) : queue_(queue), tag_(tag) {}
    RequestCompletion(RequestCompletion&& other) noexcept
        : queue_(std::exchange(other.queue_, nullptr)), tag_(other.tag_) {}
    RequestCompletion(const RequestCompletion&) = delete;
    RequestCompletion& operator=(const RequestCompletion&) = delete;
    ~RequestCompletion() {
        if (queue_) {
            try {
                queue_->push_completion(tag_, -EIO);
            } catch (...) {
                queue_->fail(EIO);
            }
        }
    }
    void publish(int32_t result) {
        queue_->push_completion(tag_, result);
        queue_ = nullptr;
    }
private:
    Queue* queue_;
    uint16_t tag_;
};

// Only the bridge's worker accesses request/ready/busy/started/closing. A
// rejected or cancelled child may be destroyed on another worker, so its
// guard publishes only atomic failure facts for the bridge to consume.
struct TagState {
    IoRequest request;
    elio::sync::event ready;
    std::optional<RequestCompletion> completion;
    bool busy = false;
    bool started = false;
    bool closing = false;
    std::atomic<bool> failed{false};
};

struct TagPool {
    explicit TagPool(Queue* q)
        : queue(q), tags(std::make_unique<TagState[]>(q->depth())) {}
    ~TagPool() {
#ifdef OBD_TEST_HOOKS
        // Fixture rescue must never retain an event after its pool leaves.
        queue->clear_bridge_admission_for_test();
#endif
    }

    void collect_failures() noexcept {
        if (!failure_pending.load(std::memory_order_acquire)) return;
        failure_pending.exchange(false, std::memory_order_acq_rel);
        for (uint16_t tag = 0; tag < queue->depth(); ++tag) {
            auto& slot = tags[tag];
            if (!slot.failed.exchange(false, std::memory_order_acq_rel)) continue;
            slot.started = false;
            slot.busy = false;
            slot.ready.reset();
            slot.completion.reset();  // Any request skipped before invocation.
        }
    }

    Queue* queue;
    std::unique_ptr<TagState[]> tags;
    std::atomic<bool> failure_pending{false};
};

class TagHandlerGuard {
public:
    TagHandlerGuard(TagPool* pool, uint16_t tag) : pool_(pool), tag_(tag) {}
    TagHandlerGuard(TagHandlerGuard&& other) noexcept
        : pool_(std::exchange(other.pool_, nullptr)), tag_(other.tag_) {}
    TagHandlerGuard(const TagHandlerGuard&) = delete;
    TagHandlerGuard& operator=(const TagHandlerGuard&) = delete;
    ~TagHandlerGuard() { mark_failed(); }

    void disarm() noexcept { pool_ = nullptr; }
    void mark_failed() noexcept {
        auto* pool = std::exchange(pool_, nullptr);
        if (!pool) return;
        pool->tags[tag_].failed.store(true, std::memory_order_release);
        pool->failure_pending.store(true, std::memory_order_release);
        pool->queue->notify_elio();
    }

private:
    TagPool* pool_;
    uint16_t tag_;
};

elio::coro::task<int32_t> handle_io(Queue* q, source::BlobSource* src,
                                    IoRequest req) {
    int32_t result = 0;
    try {
        switch (req.op) {
        case UBLK_IO_OP_READ: {
            void* buf = q->io_buf(req.tag);
            const uint32_t len = req.byte_len();
            const ssize_t r = co_await src->pread(buf, len, req.byte_offset());
            if (r < 0) {
                result = static_cast<int32_t>(r);
            } else {
                if (static_cast<uint32_t>(r) < len) {
                    // Source clamped at EOF; the device must still answer the
                    // full request (kernel copies `len` bytes).
                    std::memset(static_cast<uint8_t*>(buf) + r, 0, len - r);
                }
                result = static_cast<int32_t>(len);
            }
            break;
        }
        case UBLK_IO_OP_FLUSH: {
            auto* w = dynamic_cast<source::WritableBlobSource*>(src);
            if (w) {
                result = co_await w->flush();
            } else {
                result = 0;  // read-only device: nothing to flush
            }
            break;
        }
        case UBLK_IO_OP_WRITE: {
            // Writable images (ADR-0008) dispatch through WritableBlobSource;
            // read-only images reject with -EROFS.
            auto* w = dynamic_cast<source::WritableBlobSource*>(src);
            if (!w) {
                result = -EROFS;
                break;
            }
            const void* buf = q->io_buf(req.tag);
            const uint32_t len = req.byte_len();
            const ssize_t r = co_await w->pwrite(buf, len, req.byte_offset());
            result = r < 0 ? static_cast<int32_t>(r)
                           : static_cast<int32_t>(len);
            break;
        }
        case UBLK_IO_OP_DISCARD: {
            // ADR-0009: writable images deallocate/mask; read-only reject.
            auto* w = dynamic_cast<source::WritableBlobSource*>(src);
            if (!w) {
                result = -EROFS;
                break;
            }
            result = co_await w->discard(req.byte_offset(), req.byte_len());
            break;
        }
        case UBLK_IO_OP_WRITE_ZEROES: {
            auto* w = dynamic_cast<source::WritableBlobSource*>(src);
            if (!w) {
                result = -EROFS;
                break;
            }
            if (req.flags & UBLK_IO_F_NOUNMAP) {
                // NOUNMAP: zero the data without deallocating — a real write
                // of zeroes through the writable layer.
                const uint32_t len = req.byte_len();
                void* buf = q->io_buf(req.tag);
                std::memset(buf, 0, len);
                const ssize_t r =
                    co_await w->pwrite(buf, len, req.byte_offset());
                result = r < 0 ? static_cast<int32_t>(r) : 0;
            } else {
                result = co_await w->discard(req.byte_offset(), req.byte_len());
            }
            break;
        }
        case UBLK_IO_OP_WRITE_SAME:
            // Not advertised (basic attrs); reject defensively.
            result = -EOPNOTSUPP;
            break;
        default:
            result = -EOPNOTSUPP;
            break;
        }
    } catch (const std::exception& e) {
        ELIO_LOG_ERROR("ublk tag {} source failed: {}", req.tag, e.what());
        result = -EIO;
    } catch (...) {
        ELIO_LOG_ERROR("ublk tag {} source failed: unknown exception", req.tag);
        result = -EIO;
    }
    ELIO_LOG_DEBUG("ublk bridge: tag {} op {} -> {}", req.tag, req.op,
                   result);
    co_return result;
}

elio::coro::task<void> run_tag(TagPool* pool, source::BlobSource* src,
                               uint16_t tag, size_t owner,
                               TagHandlerGuard guard) {
    // Native TaskGroup owns admission and lifetime. User affinity can fall
    // back to a foreign worker, so check actual placement after suspension.
    // Already-local set_affinity checks allocate and schedule nothing.
    do {
        co_await elio::set_affinity(owner);
        if (elio::current_worker_id() != owner) {
#ifdef OBD_TEST_HOOKS
            pool->queue->observe_bridge_resume_for_test(BridgeResumePoint::rejected_return);
#endif
            co_await elio::time::yield();
        }
    } while (elio::current_worker_id() != owner);
    auto& slot = pool->tags[tag];
    for (;;) {
        // Request and close notifications may coalesce. Finish an accepted
        // request, then leave instead of parking for a second close wake.
        if (slot.closing && !slot.completion) {
            guard.disarm();
            co_return;
        }
        co_await slot.ready.wait();
#ifdef OBD_TEST_HOOKS
        if (const auto& hook = pool->queue->bridge_resume_hook_for_test(); hook) {
            co_await hook(BridgeResumePoint::tag_wait);
        }
#endif
        do {
            co_await elio::set_affinity(owner);
            if (elio::current_worker_id() != owner) {
#ifdef OBD_TEST_HOOKS
                pool->queue->observe_bridge_resume_for_test(BridgeResumePoint::rejected_return);
#endif
                co_await elio::time::yield();
            }
        } while (elio::current_worker_id() != owner);
#ifdef OBD_TEST_HOOKS
        pool->queue->observe_bridge_resume_for_test(BridgeResumePoint::tag_wait);
#endif
        slot.ready.reset();
        if (!slot.completion) {
            if (slot.closing) {
                guard.disarm();
                co_return;
            }
            continue;
        }
        const IoRequest request = slot.request;
        RequestCompletion completion(std::move(*slot.completion));
        slot.completion.reset();
        try {
            int32_t result = -EIO;
            // The TaskGroup wrapper checks cancellation only at admission.
            // A reused tag must preserve the same policy for each request.
            if (!elio::coro::this_coro::cancel_token().is_cancelled()) {
                result = co_await handle_io(pool->queue, src, request);
            }
#ifdef OBD_TEST_HOOKS
            if (const auto& hook = pool->queue->bridge_resume_hook_for_test(); hook) {
                co_await hook(BridgeResumePoint::source_return);
            }
#endif
            // A source may deliberately change its execution affinity. Restore
            // both placement and context before touching owner-only tag state.
            do {
                co_await elio::set_affinity(owner);
                if (elio::current_worker_id() != owner) {
#ifdef OBD_TEST_HOOKS
                    pool->queue->observe_bridge_resume_for_test(BridgeResumePoint::rejected_return);
#endif
                    co_await elio::time::yield();
                }
            } while (elio::current_worker_id() != owner);
#ifdef OBD_TEST_HOOKS
            pool->queue->observe_bridge_resume_for_test(BridgeResumePoint::source_return);
#endif
            slot.busy = false;
            completion.publish(result);
        } catch (...) {
            // Publish failure before fallback completion permits tag reuse.
            // Disarming also prevents a second, stale generation marker when
            // this frame is destroyed after the bridge has restarted the tag.
            guard.mark_failed();
            throw;
        }
    }
}

elio::coro::task<void> run_bridge_local(Queue* q, source::BlobSource* src,
                                        std::atomic<bool>* stop) {
    const size_t owner = elio::current_worker_id();
    co_await elio::set_affinity(owner);
    TagPool pool(q);
    elio::coro::task_group handlers({
        .failure_policy = elio::coro::task_group_failure_policy::collect_all});
    const int efd = q->elio_efd();
    uint64_t cnt = 0;
    std::exception_ptr failure;
    try {
        while (!stop->load(std::memory_order_relaxed)) {
            const auto r = co_await elio::io::async_read(efd, &cnt, sizeof(cnt),
                                                         -1);
#ifdef OBD_TEST_HOOKS
            if (const auto& hook = q->bridge_resume_hook_for_test(); hook) {
                co_await hook(BridgeResumePoint::bridge_read);
            }
#endif
            do {
                co_await elio::set_affinity(owner);
                if (elio::current_worker_id() != owner) {
#ifdef OBD_TEST_HOOKS
                    q->observe_bridge_resume_for_test(BridgeResumePoint::rejected_return);
#endif
                    co_await elio::time::yield();
                }
            } while (elio::current_worker_id() != owner);
#ifdef OBD_TEST_HOOKS
            q->observe_bridge_resume_for_test(BridgeResumePoint::bridge_read);
#endif
            if (r.result < 0) {
                if (r.result == -EINTR || r.result == -EAGAIN) continue;
                if (stop->load(std::memory_order_relaxed)) break;
                ELIO_LOG_ERROR("ublk bridge: eventfd read failed: {}",
                               std::strerror(-r.result));
                break;
            }
            pool.collect_failures();
            IoRequest req;
            while (q->try_pop_request(req)) {
                pool.collect_failures();
                if (req.tag >= q->depth() || pool.tags[req.tag].busy) {
                    q->fail(EIO);
                    throw std::runtime_error("ublk admitted an invalid or busy tag");
                }
                auto& slot = pool.tags[req.tag];
                slot.request = req;
                slot.busy = true;
                slot.completion.emplace(q, req.tag);
                if (!slot.started) {
                    slot.started = true;
                    handlers.spawn(run_tag, &pool, src, req.tag, owner,
                                   TagHandlerGuard(&pool, req.tag));
                }
                // A failed dispatch allocation leaves the event and request
                // unchanged. Retain this bridge frame and retry notification.
                bool signaled = false;
                while (!signaled) {
                    try {
                        slot.ready.set();
                        signaled = true;
                    } catch (const std::bad_alloc&) {
                    }
                    if (!signaled) {
                        co_await elio::time::yield();
#ifdef OBD_TEST_HOOKS
                        if (const auto& hook = q->bridge_resume_hook_for_test(); hook) {
                            co_await hook(BridgeResumePoint::request_retry);
                        }
#endif
                        do {
                            co_await elio::set_affinity(owner);
                            if (elio::current_worker_id() != owner) {
#ifdef OBD_TEST_HOOKS
                                q->observe_bridge_resume_for_test(BridgeResumePoint::rejected_return);
#endif
                                co_await elio::time::yield();
                            }
                        } while (elio::current_worker_id() != owner);
#ifdef OBD_TEST_HOOKS
                        q->observe_bridge_resume_for_test(BridgeResumePoint::request_retry);
#endif
                    }
                }
#ifdef OBD_TEST_HOOKS
                q->notify_bridge_admission_for_test(slot.ready);
#endif
            }
        }
    } catch (...) {
        failure = std::current_exception();
    }
#ifdef OBD_TEST_HOOKS
    if (const auto& hook = q->bridge_resume_hook_for_test(); hook) {
        co_await hook(BridgeResumePoint::cleanup);
    }
#endif
    do {
        co_await elio::set_affinity(owner);
        if (elio::current_worker_id() != owner) {
#ifdef OBD_TEST_HOOKS
            q->observe_bridge_resume_for_test(BridgeResumePoint::rejected_return);
#endif
            co_await elio::time::yield();
        }
    } while (elio::current_worker_id() != owner);
#ifdef OBD_TEST_HOOKS
    q->observe_bridge_resume_for_test(BridgeResumePoint::cleanup);
#endif
    // Ordinary stop closes bridge admission through stop/efd, without
    // cancelling handler bodies: accepted source IO must finish normally.
    for (uint16_t tag = 0; tag < q->depth(); ++tag) {
        auto& slot = pool.tags[tag];
        slot.closing = true;
        if (!slot.started) continue;
        bool signaled = false;
        while (!signaled) {
            try {
                slot.ready.set();
                signaled = true;
            } catch (const std::bad_alloc&) {
            }
            if (!signaled) {
                co_await elio::time::yield();
#ifdef OBD_TEST_HOOKS
                if (const auto& hook = q->bridge_resume_hook_for_test(); hook) {
                    co_await hook(BridgeResumePoint::close_retry);
                }
#endif
                do {
                    co_await elio::set_affinity(owner);
                    if (elio::current_worker_id() != owner) {
#ifdef OBD_TEST_HOOKS
                        q->observe_bridge_resume_for_test(BridgeResumePoint::rejected_return);
#endif
                        co_await elio::time::yield();
                    }
                } while (elio::current_worker_id() != owner);
#ifdef OBD_TEST_HOOKS
                q->observe_bridge_resume_for_test(BridgeResumePoint::close_retry);
#endif
            }
        }
    }
    try {
        co_await handlers.join();
    } catch (const std::exception& e) {
        ELIO_LOG_ERROR("ublk queue {} handler failed: {}", q->q_id(), e.what());
    } catch (...) {
        ELIO_LOG_ERROR("ublk queue {} handler failed: unknown exception", q->q_id());
    }
#ifdef OBD_TEST_HOOKS
    if (const auto& hook = q->bridge_resume_hook_for_test(); hook) {
        co_await hook(BridgeResumePoint::handlers_join);
    }
#endif
    do {
        co_await elio::set_affinity(owner);
        if (elio::current_worker_id() != owner) {
#ifdef OBD_TEST_HOOKS
            q->observe_bridge_resume_for_test(BridgeResumePoint::rejected_return);
#endif
            co_await elio::time::yield();
        }
    } while (elio::current_worker_id() != owner);
#ifdef OBD_TEST_HOOKS
    q->observe_bridge_resume_for_test(BridgeResumePoint::handlers_join);
#endif
    pool.collect_failures();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace

elio::coro::task<void> run_bridge(Queue* q, source::BlobSource* src,
                                  std::atomic<bool>* stop) {
    try {
        co_await run_bridge_local(q, src, stop);
    } catch (...) {
        // This also covers cold tag-pool/group setup. Once children exist,
        // run_bridge_local retains their slots and joins them before throwing.
        q->fail(EIO);
        throw;
    }
}

}  // namespace obd::ublk
