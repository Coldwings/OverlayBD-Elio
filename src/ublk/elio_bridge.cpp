// Elio bridge. See elio_bridge.hpp and ADR-0006.
#include "ublk/elio_bridge.hpp"

#include "ublk/tag_engine.hpp"

#include <elio/io/io_awaitables.hpp>
#include <elio/log/macros.hpp>
#include <elio/runtime/affinity.hpp>
#include <elio/time/timer.hpp>

#include <cerrno>
#include <cstring>
#include <exception>

namespace obd::ublk {

namespace {

void publish_queue_result(void* context, OwnerResult result) {
    static_cast<Queue*>(context)->push_completion(result.tag, result.result);
}

void fail_queue(void* context, int error) noexcept {
    static_cast<Queue*>(context)->fail(error);
}

void notify_bridge(void* context) noexcept {
    static_cast<Queue*>(context)->notify_elio();
}

elio::coro::task<void> run_bridge_local(Queue* queue,
                                        source::BlobSource* source,
                                        std::atomic<bool>* stop) {
    const size_t owner = elio::current_worker_id();
    co_await elio::set_affinity(owner);
    OwnerResultSink sink(queue, publish_queue_result, fail_queue, notify_bridge);
    TagEngine engine(queue, source, sink, owner);
    queue->mark_bridge_ready();
    const int event_fd = queue->elio_efd();
    uint64_t counter = 0;
    std::exception_ptr failure;
#ifdef OBD_TEST_HOOKS
    const auto observe_safely = [&](BridgeResumePoint point) noexcept {
        try {
            queue->observe_bridge_resume_for_test(point);
        } catch (...) {
            if (!failure) failure = std::current_exception();
        }
    };
#endif
    try {
        while (!stop->load(std::memory_order_relaxed)) {
            const auto read = co_await elio::io::async_read(
                event_fd, &counter, sizeof(counter), -1);
#ifdef OBD_TEST_HOOKS
            if (const auto& hook = queue->bridge_resume_hook_for_test(); hook)
                co_await hook(BridgeResumePoint::bridge_read);
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
            queue->observe_bridge_resume_for_test(BridgeResumePoint::bridge_read);
#endif
            if (read.result < 0) {
                if (read.result == -EINTR || read.result == -EAGAIN) continue;
                if (stop->load(std::memory_order_relaxed)) break;
                ELIO_LOG_ERROR("ublk bridge: eventfd read failed: {}",
                               std::strerror(-read.result));
                break;
            }
            engine.collect_foreign_results();
            IoRequest request;
            while (queue->try_pop_request(request)) {
                engine.collect_foreign_results();
                bool signaled = engine.admit(request);
                while (!signaled) {
                    co_await elio::time::yield();
#ifdef OBD_TEST_HOOKS
                    if (const auto& hook = queue->bridge_resume_hook_for_test();
                        hook)
                        co_await hook(BridgeResumePoint::request_retry);
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
                        BridgeResumePoint::request_retry);
#endif
                    signaled = engine.retry_signal(request.tag);
                }
            }
        }
    } catch (...) {
        failure = std::current_exception();
    }
#ifdef OBD_TEST_HOOKS
    try {
        if (const auto& hook = queue->bridge_resume_hook_for_test(); hook)
            co_await hook(BridgeResumePoint::cleanup);
    } catch (...) {
        if (!failure) failure = std::current_exception();
    }
#endif
    // From here until handlers.join(), remain in this already-allocated bridge
    // frame. A failed nested-coroutine allocation must never bypass the child
    // lifetime fence and destroy TagEngine underneath a HandlerGuard.
    do {
        co_await elio::set_affinity(owner);
        if (elio::current_worker_id() != owner) {
#ifdef OBD_TEST_HOOKS
            observe_safely(BridgeResumePoint::rejected_return);
#endif
            co_await elio::time::yield();
        }
    } while (elio::current_worker_id() != owner);
#ifdef OBD_TEST_HOOKS
    observe_safely(BridgeResumePoint::cleanup);
#endif
    engine.begin_close();
    for (uint16_t tag = 0; tag < queue->depth(); ++tag) {
        while (!engine.retry_close_signal(tag)) {
            co_await elio::time::yield();
#ifdef OBD_TEST_HOOKS
            try {
                if (const auto& hook = queue->bridge_resume_hook_for_test(); hook)
                    co_await hook(BridgeResumePoint::close_retry);
            } catch (...) {
                if (!failure) failure = std::current_exception();
            }
#endif
            do {
                co_await elio::set_affinity(owner);
                if (elio::current_worker_id() != owner) {
#ifdef OBD_TEST_HOOKS
                    observe_safely(BridgeResumePoint::rejected_return);
#endif
                    co_await elio::time::yield();
                }
            } while (elio::current_worker_id() != owner);
#ifdef OBD_TEST_HOOKS
            observe_safely(BridgeResumePoint::close_retry);
#endif
        }
    }
    try {
        co_await engine.join_handlers();
    } catch (const std::exception& error) {
        // TaskGroup reports child failures only after every frame has left.
        // A pre-wait contract failure must never be converted into ordinary
        // cleanup, because destroying a live HandlerGuard would be a UAF.
        if (!engine.handlers_drained()) std::terminate();
        ELIO_LOG_ERROR("ublk queue {} handler failed: {}", queue->q_id(),
                       error.what());
    } catch (...) {
        if (!engine.handlers_drained()) std::terminate();
        ELIO_LOG_ERROR("ublk queue {} handler failed: unknown exception",
                       queue->q_id());
    }
#ifdef OBD_TEST_HOOKS
    try {
        if (const auto& hook = queue->bridge_resume_hook_for_test(); hook)
            co_await hook(BridgeResumePoint::handlers_join);
    } catch (...) {
        if (!failure) failure = std::current_exception();
    }
#endif
    do {
        co_await elio::set_affinity(owner);
        if (elio::current_worker_id() != owner) {
#ifdef OBD_TEST_HOOKS
            observe_safely(BridgeResumePoint::rejected_return);
#endif
            co_await elio::time::yield();
        }
    } while (elio::current_worker_id() != owner);
#ifdef OBD_TEST_HOOKS
    observe_safely(BridgeResumePoint::handlers_join);
#endif
    engine.collect_foreign_results();
    if (failure) std::rethrow_exception(failure);
}

}  // namespace

elio::coro::task<void> run_bridge(Queue* queue, source::BlobSource* source,
                                  std::atomic<bool>* stop) {
    try {
        co_await run_bridge_local(queue, source, stop);
    } catch (...) {
        // This also covers cold engine/group setup. Once children exist,
        // run_bridge_local retains their tag state and joins them first.
        queue->fail(EIO);
        throw;
    }
}

}  // namespace obd::ublk
