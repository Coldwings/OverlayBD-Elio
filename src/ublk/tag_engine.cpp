#include "ublk/tag_engine.hpp"
#include "ublk/request_counts.hpp"
#include <array>

#include <elio/coro/task_group.hpp>
#include <elio/log/macros.hpp>
#include <elio/runtime/affinity.hpp>
#include <elio/sync/event.hpp>
#include <elio/time/timer.hpp>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace obd::ublk {
#ifdef OBD_TEST_HOOKS
namespace {
std::atomic<bool> count_requests{false};
std::array<std::atomic<uint64_t>, 256> request_counts{};
}
void count_requests_for_test(bool enable) {
    if (enable) for (auto& count : request_counts) count.store(0);
    count_requests.store(enable);
}
uint64_t request_count_for_test(uint8_t operation) { return request_counts[operation].load(); }
#endif

namespace {

constexpr uint64_t kStateBits = 2;
constexpr uint64_t kArmed = 1;
constexpr uint64_t kWriting = 2;
constexpr uint64_t kReady = 3;
constexpr uint64_t kMaxGeneration =
    std::numeric_limits<uint64_t>::max() >> kStateBits;

constexpr uint64_t mailbox_state(uint64_t generation, uint64_t state) {
    return (generation << kStateBits) | state;
}


}  // namespace

struct ForeignResultMailbox::Slot {
    std::atomic<uint64_t> state{0};
    std::atomic<int32_t> result{0};
};

ForeignResultMailbox::ForeignResultMailbox(uint16_t depth)
    : depth_(depth), slots_(std::make_unique<Slot[]>(depth)) {
    if (depth == 0) throw std::invalid_argument("zero tag mailbox depth");
}

ForeignResultMailbox::~ForeignResultMailbox() = default;

bool ForeignResultMailbox::arm(uint16_t tag, uint64_t generation) noexcept {
    if (tag >= depth_ || generation == 0 || generation > kMaxGeneration)
        return false;
    uint64_t empty = 0;
    return slots_[tag].state.compare_exchange_strong(
        empty, mailbox_state(generation, kArmed), std::memory_order_release,
        std::memory_order_relaxed);
}

bool ForeignResultMailbox::retire(uint16_t tag,
                                  uint64_t generation) noexcept {
    if (tag >= depth_ || generation == 0 || generation > kMaxGeneration)
        return false;
    uint64_t armed = mailbox_state(generation, kArmed);
    return slots_[tag].state.compare_exchange_strong(
        armed, 0, std::memory_order_acq_rel, std::memory_order_acquire);
}

bool ForeignResultMailbox::publish(OwnerResult result) noexcept {
    if (result.tag >= depth_ || result.generation == 0 ||
        result.generation > kMaxGeneration)
        return false;
    auto& slot = slots_[result.tag];
    uint64_t armed = mailbox_state(result.generation, kArmed);
    if (!slot.state.compare_exchange_strong(
            armed, mailbox_state(result.generation, kWriting),
            std::memory_order_acq_rel, std::memory_order_acquire))
        return false;
    slot.result.store(result.result, std::memory_order_relaxed);
    slot.state.store(mailbox_state(result.generation, kReady),
                     std::memory_order_release);
    return true;
}

bool ForeignResultMailbox::take(uint16_t tag, uint64_t generation,
                                OwnerResult& result) noexcept {
    if (tag >= depth_ || generation == 0 || generation > kMaxGeneration)
        return false;
    auto& slot = slots_[tag];
    const uint64_t ready = mailbox_state(generation, kReady);
    if (slot.state.load(std::memory_order_acquire) != ready) return false;
    const int32_t value = slot.result.load(std::memory_order_relaxed);
    uint64_t expected = ready;
    if (!slot.state.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel,
            std::memory_order_acquire))
        return false;
    result = {.tag = tag, .generation = generation, .result = value};
    return true;
}

struct TagEngine::Impl {
    struct TagState {
        IoRequest request;
        elio::sync::event ready;
        uint64_t generation = 0;
        uint64_t next_generation = 1;
        bool busy = false;
        bool started = false;
        bool closing = false;
        std::atomic<uint64_t> active_generation{0};
        std::atomic<bool> handler_failed{false};
    };

    class HandlerGuard {
    public:
        HandlerGuard(Impl* engine, uint16_t tag)
            : engine_(engine), tag_(tag) {}
        HandlerGuard(HandlerGuard&& other) noexcept
            : engine_(std::exchange(other.engine_, nullptr)), tag_(other.tag_) {}
        HandlerGuard(const HandlerGuard&) = delete;
        HandlerGuard& operator=(const HandlerGuard&) = delete;
        ~HandlerGuard() { mark_failed(); }

        void disarm() noexcept { engine_ = nullptr; }
        void mark_failed() noexcept {
            auto* engine = std::exchange(engine_, nullptr);
            if (!engine) return;
            auto& slot = engine->tags[tag_];
            // The guard can be destroyed by scheduler rejection or on a
            // foreign worker. Publish only atomics and a wake; the engine
            // owner remains the sole result-sink caller.
            const uint64_t generation =
                slot.active_generation.load(std::memory_order_acquire);
            if (generation != 0 &&
                !engine->mailbox.publish({tag_, generation, -EIO})) {
                engine->sink.fail(EPROTO);
            }
            slot.handler_failed.store(true, std::memory_order_release);
            engine->foreign_pending.store(true, std::memory_order_release);
            engine->sink.notify_owner();
        }

    private:
        Impl* engine_;
        uint16_t tag_;
    };

    Impl(Queue* queue_value, source::BlobSource* source_value,
         OwnerResultSink sink_value, size_t owner_value)
        : queue(queue_value), blob_source(source_value), sink(sink_value),
          owner(owner_value), tags(std::make_unique<TagState[]>(queue->depth())),
          mailbox(queue->depth()), handlers({
              .failure_policy =
                  elio::coro::task_group_failure_policy::collect_all}) {}

    ~Impl() {
#ifdef OBD_TEST_HOOKS
        queue->clear_bridge_admission_for_test();
#endif
    }

    void collect_foreign_results() noexcept {
        if (!foreign_pending.exchange(false, std::memory_order_acq_rel)) return;
        for (uint16_t tag = 0; tag < queue->depth(); ++tag) {
            auto& slot = tags[tag];
            if (!slot.handler_failed.exchange(false,
                                               std::memory_order_acq_rel))
                continue;
            OwnerResult result;
            const bool has_result = slot.generation != 0 &&
                mailbox.take(tag, slot.generation, result);
            // An active obligation and its exact-generation mailbox result
            // must agree. Either mismatch is sticky protocol failure; never
            // guess which generation a result belongs to.
            if (slot.busy != has_result) sink.fail(EPROTO);
            slot.active_generation.store(0, std::memory_order_release);
            slot.started = false;
            slot.busy = false;
            slot.ready.reset();
            if (has_result) {
                try {
                    sink.publish(result);
                } catch (...) {
                    sink.fail(EIO);
                }
            }
        }
    }

    elio::coro::task<void> run_tag(uint16_t tag, HandlerGuard guard) {
        do {
            co_await elio::set_affinity(owner);
            if (elio::current_worker_id() != owner) {
#ifdef OBD_TEST_HOOKS
                queue->observe_bridge_resume_for_test(
                    BridgeResumePoint::rejected_return);
#endif
                co_await elio::time::yield();
            }
        } while (elio::current_worker_id() != owner);
        auto& slot = tags[tag];
        for (;;) {
            if (slot.closing && !slot.busy) {
                guard.disarm();
                co_return;
            }
            co_await slot.ready.wait();
#ifdef OBD_TEST_HOOKS
            if (const auto& hook = queue->bridge_resume_hook_for_test(); hook)
                co_await hook(BridgeResumePoint::tag_wait);
#endif
            do {
                co_await elio::set_affinity(owner);
                if (elio::current_worker_id() != owner) {
#ifdef OBD_TEST_HOOKS
                    queue->observe_bridge_resume_for_test(
                        BridgeResumePoint::rejected_return);
#endif
                    co_await elio::time::yield();
                }
            } while (elio::current_worker_id() != owner);
#ifdef OBD_TEST_HOOKS
            queue->observe_bridge_resume_for_test(BridgeResumePoint::tag_wait);
#endif
            slot.ready.reset();
            if (!slot.busy) {
                if (slot.closing) {
                    guard.disarm();
                    co_return;
                }
                continue;
            }
            const IoRequest request = slot.request;
            const uint64_t generation = slot.generation;
            try {
                int32_t result = -EIO;
                if (!elio::coro::this_coro::cancel_token().is_cancelled()) {
                    result = 0;
#ifdef OBD_TEST_HOOKS
                    if (count_requests.load(std::memory_order_relaxed))
                        request_counts[static_cast<uint8_t>(request.op)].fetch_add(1, std::memory_order_relaxed);
#endif
                    try {
                        switch (request.op) {
                        case UBLK_IO_OP_READ: {
                            void* buffer = queue->io_buf(request.tag);
                            const uint32_t length = request.byte_len();
                            const ssize_t read =
                                co_await blob_source->pread(buffer, length,
                                                            request.byte_offset());
                            if (read < 0) {
                                result = static_cast<int32_t>(read);
                            } else {
                                if (static_cast<uint32_t>(read) < length) {
                                    std::memset(static_cast<uint8_t*>(buffer) + read, 0,
                                                length - read);
                                }
                                result = static_cast<int32_t>(length);
                            }
                            break;
                        }
                        case UBLK_IO_OP_FLUSH: {
                            auto* writable =
                                dynamic_cast<source::WritableBlobSource*>(blob_source);
                            result = writable ? co_await writable->flush() : 0;
                            break;
                        }
                        case UBLK_IO_OP_WRITE: {
                            auto* writable =
                                dynamic_cast<source::WritableBlobSource*>(blob_source);
                            if (!writable) {
                                result = -EROFS;
                                break;
                            }
                            const uint32_t length = request.byte_len();
                            const ssize_t written = co_await writable->pwrite(
                                queue->io_buf(request.tag), length, request.byte_offset());
                            result = written < 0 ? static_cast<int32_t>(written)
                                                 : static_cast<int32_t>(length);
                            break;
                        }
                        case UBLK_IO_OP_DISCARD: {
                            auto* writable =
                                dynamic_cast<source::WritableBlobSource*>(blob_source);
                            result = writable
                                ? co_await writable->discard(request.byte_offset(),
                                                             request.byte_len())
                                : -EROFS;
                            break;
                        }
                        case UBLK_IO_OP_WRITE_ZEROES: {
                            auto* writable =
                                dynamic_cast<source::WritableBlobSource*>(blob_source);
                            if (!writable) {
                                result = -EROFS;
                                break;
                            }
                            if (request.flags & UBLK_IO_F_NOUNMAP) {
                                const uint32_t length = request.byte_len();
                                void* buffer = queue->io_buf(request.tag);
                                std::memset(buffer, 0, length);
                                const ssize_t written = co_await writable->pwrite(
                                    buffer, length, request.byte_offset());
                                result = written < 0 ? static_cast<int32_t>(written) : 0;
                            } else {
                                result = co_await writable->discard(request.byte_offset(),
                                                                    request.byte_len());
                            }
                            break;
                        }
                        case UBLK_IO_OP_WRITE_SAME:
                            result = -EOPNOTSUPP;
                            break;
                        default:
                            result = -EOPNOTSUPP;
                            break;
                        }
                    } catch (const std::exception& error) {
                        ELIO_LOG_ERROR("ublk tag {} source failed: {}", request.tag,
                                       error.what());
                        result = -EIO;
                    } catch (...) {
                        ELIO_LOG_ERROR("ublk tag {} source failed: unknown exception",
                                       request.tag);
                        result = -EIO;
                    }
                    ELIO_LOG_DEBUG("ublk bridge: tag {} op {} -> {}", request.tag,
                                   request.op, result);
                }
#ifdef OBD_TEST_HOOKS
                if (const auto& hook = queue->bridge_resume_hook_for_test(); hook)
                    co_await hook(BridgeResumePoint::source_return);
#endif
                do {
                    co_await elio::set_affinity(owner);
                    if (elio::current_worker_id() != owner) {
#ifdef OBD_TEST_HOOKS
                        queue->observe_bridge_resume_for_test(
                            BridgeResumePoint::rejected_return);
#endif
                        co_await elio::time::yield();
                    }
                } while (elio::current_worker_id() != owner);
#ifdef OBD_TEST_HOOKS
                queue->observe_bridge_resume_for_test(
                    BridgeResumePoint::source_return);
#endif
                // Keep the mailbox armed until publication succeeds. If the
                // current Queue sink throws before enqueueing, guard failure
                // supplies the same generation's allocation-free fallback.
                sink.publish({tag, generation, result});
                if (!mailbox.retire(tag, generation)) sink.fail(EPROTO);
                slot.active_generation.store(0, std::memory_order_release);
                slot.busy = false;
            } catch (...) {
                guard.mark_failed();
                throw;
            }
        }
    }

    bool signal(uint16_t tag) {
        try {
            tags[tag].ready.set();
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    bool admit(IoRequest request) {
        // close is terminal. Reject before arming a generation or mutating a
        // tag so a caller cannot create an obligation with no live handler.
        if (closed) {
            sink.fail(ESHUTDOWN);
            throw std::logic_error("ublk tag engine admission is closed");
        }
        collect_foreign_results();
        if (request.tag >= queue->depth() || tags[request.tag].busy) {
            sink.fail(EIO);
            throw std::runtime_error("ublk admitted an invalid or busy tag");
        }
        auto& slot = tags[request.tag];
        if (slot.next_generation == 0 ||
            slot.next_generation > kMaxGeneration) {
            sink.fail(EOVERFLOW);
            throw std::runtime_error("ublk tag generation exhausted");
        }
        const uint64_t generation = slot.next_generation++;
        if (!mailbox.arm(request.tag, generation)) {
            sink.fail(EPROTO);
            throw std::runtime_error("ublk tag mailbox was not retired");
        }
        slot.request = request;
        slot.generation = generation;
        slot.busy = true;
        // Publish the obligation before the child can be admitted/rejected.
        slot.active_generation.store(generation, std::memory_order_release);
        if (!slot.started) {
            slot.started = true;
            handlers.spawn(&Impl::run_tag, this, request.tag,
                           HandlerGuard(this, request.tag));
        }
        const bool signaled = signal(request.tag);
#ifdef OBD_TEST_HOOKS
        if (signaled) queue->notify_bridge_admission_for_test(slot.ready);
#endif
        return signaled;
    }

    void begin_close() noexcept {
        if (closed) return;
        closed = true;
        for (uint16_t tag = 0; tag < queue->depth(); ++tag) {
            tags[tag].closing = true;
        }
    }

    Queue* queue;
    source::BlobSource* blob_source;
    OwnerResultSink sink;
    size_t owner;
    std::unique_ptr<TagState[]> tags;
    ForeignResultMailbox mailbox;
    elio::coro::task_group handlers;
    std::atomic<bool> foreign_pending{false};
    bool closed = false;
};

TagEngine::TagEngine(Queue* queue, source::BlobSource* source,
                     OwnerResultSink sink, size_t owner)
    : impl_(std::make_unique<Impl>(queue, source, sink, owner)) {}

TagEngine::~TagEngine() = default;

void TagEngine::collect_foreign_results() noexcept {
    impl_->collect_foreign_results();
}

bool TagEngine::admit(IoRequest request) {
    return impl_->admit(request);
}

bool TagEngine::retry_signal(uint16_t tag) {
    if (tag >= impl_->queue->depth()) {
        impl_->sink.fail(EIO);
        throw std::out_of_range("ublk tag signal is out of range");
    }
    const bool signaled = impl_->signal(tag);
#ifdef OBD_TEST_HOOKS
    if (signaled)
        impl_->queue->notify_bridge_admission_for_test(impl_->tags[tag].ready);
#endif
    return signaled;
}

void TagEngine::begin_close() noexcept {
    impl_->begin_close();
}

bool TagEngine::retry_close_signal(uint16_t tag) {
    if (tag >= impl_->queue->depth()) {
        impl_->sink.fail(EIO);
        throw std::out_of_range("ublk close signal is out of range");
    }
    return !impl_->tags[tag].started || impl_->signal(tag);
}

elio::coro::task_group::join_awaitable TagEngine::join_handlers() noexcept {
    return impl_->handlers.join();
}

bool TagEngine::handlers_drained() const noexcept {
    return impl_->handlers.outstanding_children() == 0;
}

}  // namespace obd::ublk
