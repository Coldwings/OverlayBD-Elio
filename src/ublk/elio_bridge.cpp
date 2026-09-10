// Elio bridge. See elio_bridge.hpp and ADR-0006.
#include "ublk/elio_bridge.hpp"

#include <elio/io/io_awaitables.hpp>
#include <elio/log/macros.hpp>
#include <elio/runtime/spawn.hpp>

#include <sys/eventfd.h>

#include <cerrno>
#include <cstring>

namespace obd::ublk {

namespace {

elio::coro::task<void> handle_io(Queue* q, source::BlobSource* src,
                                 IoRequest req, std::atomic<int>* running) {
    // Decrement on every exit path: Device::stop() waits for all
    // coroutines touching `q` to finish before ~Queue.
    struct Guard {
        std::atomic<int>* r;
        ~Guard() { r->fetch_sub(1, std::memory_order_acq_rel); }
    } guard{running};
    int32_t result = 0;
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
    q->push_completion(req.tag, result);
    ELIO_LOG_DEBUG("ublk bridge: tag {} op {} -> {}", req.tag, req.op,
                   result);
}

}  // namespace

elio::coro::task<void> run_bridge(Queue* q, source::BlobSource* src,
                                  std::atomic<bool>* stop,
                                  std::atomic<int>* running) {
    struct Guard {
        std::atomic<int>* r;
        ~Guard() { r->fetch_sub(1, std::memory_order_acq_rel); }
    } guard{running};
    const int efd = q->elio_efd();
    uint64_t cnt = 0;
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
            // The counter is incremented BEFORE the spawn so stop()
            // never observes a zero while a coroutine is still queued.
            running->fetch_add(1, std::memory_order_acq_rel);
            try {
                elio::go(handle_io, q, src, req, running);
            } catch (...) {
                running->fetch_sub(1, std::memory_order_acq_rel);
                throw;
            }
        }
    }
}

}  // namespace obd::ublk
