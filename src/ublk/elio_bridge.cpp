// Fixed-worker owner and legacy test bridge. See ADR-0023.
#include "ublk/elio_bridge.hpp"

#include "ublk/tag_engine.hpp"

#include <elio/io/io_awaitables.hpp>
#include <elio/log/macros.hpp>
#include <elio/runtime/affinity.hpp>
#include <elio/time/timer.hpp>

#include <cerrno>
#include <chrono>
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

void publish_owner_result(void* context, OwnerResult result) {
    static_cast<Queue*>(context)->complete_on_owner(result.tag, result.result);
}

void notify_owner(void* context) noexcept {
    static_cast<Queue*>(context)->wakeup();
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

elio::coro::task<void> run_owner_pump(Queue* queue,
                                      source::BlobSource* source,
                                      std::atomic<bool>* stop) {
    using namespace std::chrono_literals;
    constexpr unsigned kCqTurn = 32;

    // This admission CAS is the first body operation. Device may seal a task
    // rejected before execution; a late scheduler start must then touch no
    // queue state and own no raw command.
    if (!queue->begin_owner()) co_return;

    const size_t owner = elio::current_worker_id();
    co_await elio::set_affinity(owner);
    try {
        queue->init_owner_pump(owner);
    } catch (...) {
        queue->fail(EIO);
        queue->mark_source_handlers_drained();
        queue->finish_owner();
        co_return;
    }

    std::unique_ptr<TagEngine> engine;
    try {
        OwnerResultSink sink(queue, publish_owner_result, fail_queue,
                             notify_owner);
        engine = std::make_unique<TagEngine>(queue, source, sink, owner);
    } catch (...) {
        queue->fail(EIO);
        queue->mark_source_handlers_drained();
        queue->finish_owner();
        co_return;
    }
    queue->mark_bridge_ready();

    std::exception_ptr failure;
    bool published = false;
    uint64_t counter = 0;

    // Publication is a coroutine wait: the fixed worker remains available to
    // unrelated queues, control work, and source/native-I/O continuations.
    while (!queue->publication_ready() && !queue->publication_aborted() &&
           !queue->failed()) {
        try {
            const auto read = co_await elio::io::async_read(
                queue->owner_eventfd(), &counter, sizeof(counter), -1);
            do {
                co_await elio::set_affinity(owner);
                if (elio::current_worker_id() != owner)
                    co_await elio::time::yield();
            } while (elio::current_worker_id() != owner);
            if (read.result < 0 && read.result != -EINTR &&
                read.result != -EAGAIN)
                queue->fail(-read.result);
        } catch (...) {
            failure = std::current_exception();
            queue->fail(EIO);
        }
    }

    if (queue->publication_ready() && !queue->failed()) {
        try {
            queue->prepare_owner_fetches();
#ifdef OBD_TEST_HOOKS
            try {
                co_await queue->await_initial_submit_for_test();
            } catch (...) {
                failure = std::current_exception();
                queue->fail(EIO);
            }
            // The fixture wait may resume elsewhere. Prepared FETCH debt must
            // still be issued by its recorded Linux task before STOP can run.
            do {
                co_await elio::set_affinity(owner);
                if (elio::current_worker_id() != owner)
                    co_await elio::time::yield();
            } while (elio::current_worker_id() != owner);
#endif
            queue->submit_owner_fetches();
            published = queue->initial_fetches_issued() ||
                        !queue->retirement().complete;
        } catch (...) {
            failure = std::current_exception();
            queue->fail(EIO);
            // prep_io_cmd records ambiguous debt before a submit can fail.
            published = !queue->retirement().complete;
        }
    }

    // Steady state. CQ work is bounded per turn. A saturated queue takes one
    // native-timer turn between batches so older owner-local work, other
    // queues, and Elio native I/O cannot be hidden by a permanently nonempty
    // raw CQ. The common QD1 path parks directly on the registered eventfd.
    while (published && !stop->load(std::memory_order_relaxed) &&
           !queue->failed()) {
        try {
            engine->collect_foreign_results();
            queue->flush_owner_completions();
            const bool more = queue->drive_owner_cq(kCqTurn);
            IoRequest request;
            while (queue->try_pop_request(request)) {
                engine->collect_foreign_results();
                bool signaled = engine->admit(request);
                while (!signaled) {
                    co_await elio::time::yield();
#ifdef OBD_TEST_HOOKS
                    if (const auto& hook = queue->bridge_resume_hook_for_test();
                        hook)
                        co_await hook(BridgeResumePoint::request_retry);
#endif
                    do {
                        co_await elio::set_affinity(owner);
                        if (elio::current_worker_id() != owner)
                            co_await elio::time::yield();
                    } while (elio::current_worker_id() != owner);
                    signaled = engine->retry_signal(request.tag);
                }
            }
            if (stop->load(std::memory_order_relaxed) || queue->failed())
                break;
            if (more) {
                co_await elio::time::sleep_for(1ns);
            } else {
                const auto read = co_await elio::io::async_read(
                    queue->owner_eventfd(), &counter, sizeof(counter), -1);
                if (read.result < 0 && read.result != -EINTR &&
                    read.result != -EAGAIN)
                    queue->fail(-read.result);
            }
            do {
                co_await elio::set_affinity(owner);
                if (elio::current_worker_id() != owner)
                    co_await elio::time::yield();
            } while (elio::current_worker_id() != owner);
        } catch (...) {
            failure = std::current_exception();
            queue->fail(EIO);
        }
    }

    // Admission closes before the source-loan fence. Existing handlers may
    // finish and submit COMMIT_AND_FETCH directly on this worker; no raw CQ
    // processing is required for those source frames to retire.
    queue->close_owner_completion_batching();
    engine->begin_close();
    for (uint16_t tag = 0; tag < queue->depth(); ++tag) {
        while (!engine->retry_close_signal(tag)) {
            try {
                co_await elio::time::yield();
                do {
                    co_await elio::set_affinity(owner);
                    if (elio::current_worker_id() != owner)
                        co_await elio::time::yield();
                } while (elio::current_worker_id() != owner);
            } catch (...) {
                if (!failure) failure = std::current_exception();
                queue->fail(EIO);
            }
        }
    }
    try {
        co_await engine->join_handlers();
    } catch (...) {
        if (!engine->handlers_drained()) std::terminate();
        if (!failure) failure = std::current_exception();
        queue->fail(EIO);
    }
    do {
        co_await elio::set_affinity(owner);
        if (elio::current_worker_id() != owner)
            co_await elio::time::yield();
    } while (elio::current_worker_id() != owner);
    engine->collect_foreign_results();
    queue->mark_source_handlers_drained();

    if (!published) {
        queue->finish_owner();
        co_return;
    }

    // Source frames are gone, but the raw ring stays live. Device now has a
    // safe STOP boundary; after STOP it asks this same task to exit only when
    // every original command cookie has produced its terminal CQE.
    while (!queue->owner_may_exit()) {
        bool retry_delay = false;
        try {
            const bool more = queue->drive_owner_cq(kCqTurn);
            queue->maintain_owner();
            if (queue->owner_may_exit()) break;
            if (more) {
                co_await elio::time::sleep_for(1ns);
            } else {
                const auto read = co_await elio::io::async_read(
                    queue->owner_eventfd(), &counter, sizeof(counter), -1);
                if (read.result < 0 && read.result != -EINTR &&
                    read.result != -EAGAIN)
                    queue->fail(-read.result);
            }
            do {
                co_await elio::set_affinity(owner);
                if (elio::current_worker_id() != owner)
                    co_await elio::time::yield();
            } while (elio::current_worker_id() != owner);
        } catch (...) {
            if (!failure) failure = std::current_exception();
            queue->fail(EIO);
            retry_delay = true;
        }
        if (retry_delay) {
            try { co_await elio::time::sleep_for(1ms); }
            catch (...) {}
        }
    }
    queue->finish_owner();
    if (failure) std::rethrow_exception(failure);
}

}  // namespace obd::ublk
