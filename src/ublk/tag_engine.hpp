// Reusable Elio-side tag handlers for one ublk queue.
//
// The engine owns no raw ring. Its caller remains the only owner of request
// admission and result consumption. The current ADR-0006 bridge feeds it from
// Queue::pending_ and uses a sink that forwards results to Queue::done_. A
// future same-owner ring pump can replace that sink without changing handler
// lifetime or letting a foreign continuation touch ring state.
#pragma once

#include "ublk/queue.hpp"

#include "source/blob_source.hpp"

#include <elio/coro/task_group.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace obd::ublk {

struct OwnerResult {
    uint16_t tag = 0;
    uint64_t generation = 0;
    int32_t result = 0;

    friend bool operator==(const OwnerResult&, const OwnerResult&) = default;
};

/// Non-owning function-pointer result boundary. publish() is owner-only.
/// fail() and notify_owner() may be called from a foreign worker and therefore
/// cannot throw; they may publish only sticky facts/wakes, never owner ring
/// state.
class OwnerResultSink {
public:
    using Publish = void (*)(void*, OwnerResult);
    using Fail = void (*)(void*, int) noexcept;
    using Notify = void (*)(void*) noexcept;

    OwnerResultSink(void* context, Publish publish, Fail fail, Notify notify)
        : context_(context), publish_(publish), fail_(fail), notify_(notify) {}

    void publish(OwnerResult result) const { publish_(context_, result); }
    void fail(int error) const noexcept { fail_(context_, error); }
    void notify_owner() const noexcept { notify_(context_); }

private:
    void* context_;
    Publish publish_;
    Fail fail_;
    Notify notify_;
};

/// One preallocated single-producer result slot per tag. The owner arms an
/// exact nonzero generation before admitting work. A foreign publisher can
/// fill that generation once; stale and duplicate publications are rejected.
/// Only the owner consumes and clears a ready result.
class ForeignResultMailbox {
public:
    explicit ForeignResultMailbox(uint16_t depth);
    ~ForeignResultMailbox();
    ForeignResultMailbox(const ForeignResultMailbox&) = delete;
    ForeignResultMailbox& operator=(const ForeignResultMailbox&) = delete;

    bool arm(uint16_t tag, uint64_t generation) noexcept;
    bool retire(uint16_t tag, uint64_t generation) noexcept;
    bool publish(OwnerResult result) noexcept;
    bool take(uint16_t tag, uint64_t generation, OwnerResult& result) noexcept;

private:
    struct Slot;
    uint16_t depth_;
    std::unique_ptr<Slot[]> slots_;
};

/// Owner-only reusable tag state plus TaskGroup-owned handler frames. Methods
/// must run on `owner`; source awaits may migrate, but handlers restore that
/// worker before touching engine state or publishing through the owner sink.
class TagEngine {
public:
    TagEngine(Queue* queue, source::BlobSource* source, OwnerResultSink sink,
              size_t owner);
    ~TagEngine();
    TagEngine(const TagEngine&) = delete;
    TagEngine& operator=(const TagEngine&) = delete;

    void collect_foreign_results() noexcept;
    /// Admit one request and try to wake its reusable handler. Returns false
    /// only when the native event wake allocation must be retried by the owner.
    bool admit(IoRequest request);
    bool retry_signal(uint16_t tag);

    /// Close admission and mark every handler terminal. The owner retries
    /// signals, then directly awaits join_handlers() from its existing frame.
    void begin_close() noexcept;
    bool retry_close_signal(uint16_t tag);
    elio::coro::task_group::join_awaitable join_handlers() noexcept;
    bool handlers_drained() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace obd::ublk
