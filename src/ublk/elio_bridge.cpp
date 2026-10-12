// Elio bridge. See elio_bridge.hpp and ADR-0006.
#include "ublk/elio_bridge.hpp"

#include <elio/io/io_awaitables.hpp>
#include <elio/log/macros.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/coro/task_group.hpp>

#include <sys/eventfd.h>

#include <cerrno>
#include <cstring>
#include <exception>
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

elio::coro::task<void> handle_io(Queue* q, source::BlobSource* src,
                                 IoRequest req, RequestCompletion completion) {
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
    completion.publish(result);
    ELIO_LOG_DEBUG("ublk bridge: tag {} op {} -> {}", req.tag, req.op,
                   result);
}

}  // namespace

elio::coro::task<void> run_bridge(Queue* q, source::BlobSource* src,
                                  std::atomic<bool>* stop) {
    elio::coro::task_group handlers({
        .failure_policy = elio::coro::task_group_failure_policy::collect_all});
    const int efd = q->elio_efd();
    uint64_t cnt = 0;
    std::exception_ptr failure;
    try {
        while (!stop->load(std::memory_order_relaxed)) {
            const auto r = co_await elio::io::async_read(efd, &cnt, sizeof(cnt),
                                                         -1);
            if (r.result < 0) {
                if (r.result == -EINTR || r.result == -EAGAIN) continue;
                if (stop->load(std::memory_order_relaxed)) break;
                ELIO_LOG_ERROR("ublk bridge: eventfd read failed: {}",
                               std::strerror(-r.result));
                break;
            }
            IoRequest req;
            while (q->try_pop_request(req)) {
                // Each tag gets its own coroutine; per-tag IO overlaps. The
                // completion ordering across tags is irrelevant to ublk.
                handlers.spawn(handle_io, q, src, req,
                               RequestCompletion(q, req.tag));
            }
        }
    } catch (...) {
        failure = std::current_exception();
    }
    // Ordinary stop closes bridge admission through stop/efd, without
    // cancelling handler bodies: accepted source IO must finish normally.
    try {
        co_await handlers.join();
    } catch (const std::exception& e) {
        ELIO_LOG_ERROR("ublk queue {} handler failed: {}", q->q_id(), e.what());
    } catch (...) {
        ELIO_LOG_ERROR("ublk queue {} handler failed: unknown exception", q->q_id());
    }
    if (failure) {
        q->fail(EIO);
        std::rethrow_exception(failure);
    }
}

}  // namespace obd::ublk
