// Scoped native Elio fault injection for lifecycle regression tests only.
#pragma once

#include <elio/coro/task.hpp>
#include <elio/sync/event.hpp>

#include <atomic>
#include <new>

namespace obd::test {

namespace runtime_fault_detail {
inline std::atomic<unsigned> destroyed_remaining{0};
inline std::atomic<unsigned> destroyed_failures{0};
inline std::atomic<unsigned> event_remaining{0};
inline std::atomic<unsigned> event_failures{0};

inline bool take_failure(std::atomic<unsigned>& remaining) {
    auto value = remaining.load();
    while (value != 0) {
        if (remaining.compare_exchange_weak(value, value - 1)) return true;
    }
    return false;
}
inline void fail_destroyed_setup() {
    if (take_failure(destroyed_remaining)) {
        destroyed_failures.fetch_add(1);
        throw std::bad_alloc();
    }
}
inline void fail_writer_event_dispatch() {
    if (elio::runtime::worker_thread::current() == nullptr &&
        take_failure(event_remaining)) {
        event_failures.fetch_add(1);
        throw std::bad_alloc();
    }
}
}  // namespace runtime_fault_detail

class DestroyedWaitFault {
public:
    explicit DestroyedWaitFault(unsigned failures = 1) {
        runtime_fault_detail::destroyed_remaining.store(failures);
        runtime_fault_detail::destroyed_failures.store(0);
        old_ = elio::coro::detail::join_destroyed_observer_setup_for_test.exchange(
            runtime_fault_detail::fail_destroyed_setup);
    }
    ~DestroyedWaitFault() {
        elio::coro::detail::join_destroyed_observer_setup_for_test.store(old_);
        runtime_fault_detail::destroyed_remaining.store(0);
    }
    unsigned failures() const { return runtime_fault_detail::destroyed_failures.load(); }
private:
    void (*old_)() = nullptr;
};

class WriterEventFault {
public:
    explicit WriterEventFault(unsigned failures = 1) {
        runtime_fault_detail::event_remaining.store(failures);
        runtime_fault_detail::event_failures.store(0);
        old_ = elio::sync::detail::event_dispatch_storage_for_test.exchange(
            runtime_fault_detail::fail_writer_event_dispatch);
    }
    ~WriterEventFault() {
        elio::sync::detail::event_dispatch_storage_for_test.store(old_);
        runtime_fault_detail::event_remaining.store(0);
    }
    unsigned failures() const { return runtime_fault_detail::event_failures.load(); }
private:
    void (*old_)() = nullptr;
};

class WakeStateFault {
public:
    WakeStateFault()
        : old_(elio::sync::detail::fail_next_wake_state_allocation_for_test.exchange(false)) {}
    ~WakeStateFault() {
        elio::sync::detail::fail_next_wake_state_allocation_for_test.store(old_);
    }
    void arm() { elio::sync::detail::fail_next_wake_state_allocation_for_test.store(true); }
    bool consumed() const {
        return !elio::sync::detail::fail_next_wake_state_allocation_for_test.load();
    }
private:
    bool old_;
};

}  // namespace obd::test
