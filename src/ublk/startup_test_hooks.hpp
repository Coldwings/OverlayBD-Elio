#pragma once

#ifdef OBD_TEST_HOOKS
#include <elio/coro/task.hpp>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <chrono>

namespace obd::ublk { class Queue; }

namespace obd::ublk::detail {
class CtrlTransport;

// Test-only observation boundary. The caller owns the instance through the
// entire startup/retention/teardown lifetime. Observers must never throw or
// change command results. No process-global callback or environment lookup is
// performed by Device, Ctrl or Queue.
enum class StartupTestKind : uint32_t {
    control_prepared = 1, control_submitted, control_original,
    control_cancel_prepared, control_cancel, control_finished,
    raw_prepared, raw_cqe, initial_issued, initial_gate_enter,
    initial_gate_leave, owner_exit, bridge_destroyed, sources_drained,
    cdev_closed, adopted, startup_snapshot, caller_fills_parked,
    caller_terminal, post_control_enter, post_control_fail, source_enter,
    source_result, source_scope_exit, source_destroyed, request_delivered, release_admitted,
    failure_report_throw, failure_report_caught, failure_report_constructed, release_setup_throw,
    control_destroyed, retained_entered, owner_batching_closed,
};
struct StartupTestEvent {
    StartupTestKind kind{};
    uint32_t queue = UINT32_MAX;
    uint32_t opcode = 0;
    int32_t result = 0;
    uint32_t flags = 0;
    uint64_t cookie = 0;
    uint64_t value = 0;
    uint64_t owner_flags = 0; // caller_terminal: bit 0 debt-known, bit 1 control-pending
    uint64_t release_sequence = 0;
    uint64_t buffer = 0, offset = 0, length = 0;
    uint32_t tag = UINT32_MAX;
};
class StartupTestHooks {
public:
    virtual ~StartupTestHooks() = default;
    virtual void observe(StartupTestEvent event) noexcept = 0;
    virtual void before_initial_submit(uint16_t queue) noexcept = 0;
    virtual elio::coro::task<void> before_initial_submit_async(uint16_t queue) {
        before_initial_submit(queue);
        co_return;
    }
    virtual elio::coro::task<void> after_start_control() { co_return; }
    virtual void before_release_attempt(uint64_t) {}
    // Ordinary coordinator fixtures borrow dependencies through the same
    // startup owner. Null/false preserves the kernel implementation. Queue
    // adapters supply descriptors/buffers only, never lifecycle facts.
    virtual CtrlTransport* control_transport() noexcept { return nullptr; }
    virtual bool open_queue(Queue&, int) { return false; }
    // Retains the pre-ADR-0023 native-thread topology only for deterministic
    // coordinator fixtures whose synchronous gates deliberately block an
    // owner. Production and real-kernel fixtures always return false.
    virtual bool legacy_thread_topology() const noexcept { return false; }
    // Native queue/control threads only; throwing setup failures are caught
    // by Queue::run before it can prepare any FETCH.
    virtual void before_queue_setup(Queue&) {}
    // Runs immediately before constructing each fixed-owner join handle.
    // Tests may ask Elio to reject that one scheduler admission.
    virtual void before_owner_spawn(uint16_t) {}
    virtual void before_start_admission() {}
    virtual std::chrono::steady_clock::time_point startup_now() noexcept {
        return std::chrono::steady_clock::now();
    }
};

// Finite shared-memory fixture wire layout, internal to test-enabled binaries.
// Records are append-only and committed independently; readers must acquire
// committed before reading the payload. This Linux fixture requires lock-free
// interprocess atomics. Overflow invalidates evidence without changing owners.
inline constexpr uint64_t kStartupFixtureMagic = 0x4f42444649585431ULL;
inline constexpr size_t kStartupFixtureCapacity = 16384;
struct StartupFixtureRecord {
    std::atomic<uint64_t> committed{0};
    uint64_t monotonic_ns = 0;
    StartupTestEvent event;
};
struct StartupFixtureLog {
    uint64_t magic = kStartupFixtureMagic;
    uint64_t generation = 0;
    uint64_t pid = 0;
    uint64_t process_start_ticks = 0;
    uint32_t recovery = 0;
    uint32_t queues = 2;
    uint32_t depth = 4;
    uint32_t adopted_source = 0;
    uint32_t report_refusal = 0;
    std::atomic<uint32_t> queue_one_open{0};
    std::atomic<uint32_t> post_control_open{0};
    std::atomic<uint32_t> source_open{0};
    std::atomic<uint64_t> allocated{0};
    std::atomic<uint32_t> overflow{0};
    std::atomic<uint32_t> initialized{0};
    std::array<StartupFixtureRecord, kStartupFixtureCapacity> records;
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);

} // namespace obd::ublk::detail
#endif
