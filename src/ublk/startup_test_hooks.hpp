#pragma once

#ifdef OBD_TEST_HOOKS
#include <elio/coro/task.hpp>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace obd::ublk::detail {

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
    virtual elio::coro::task<void> after_start_control() { co_return; }
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
