#include "ublk/startup_control.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <limits>

namespace obd::ublk {
// Exercise the owner's transition boundary without creating a device. These
// operations must remain serialized by the actual startup owner.
struct StartupControlTestAccess {
    static bool ready(StartupControl& control) {
        return control.close_release_admission();
    }
    static uint64_t pending(const StartupControl& control) {
        return control.release_request();
    }
    static void refuse(StartupControl& control, uint64_t sequence) {
        control.acknowledge_release(sequence);
    }
    static void released(StartupControl& control, uint64_t sequence) {
        control.finish_release_admission(sequence);
    }
    static void publish(StartupControl& control, const StartupSnapshot& value) {
        control.publish(value);
    }
#ifdef OBD_TEST_HOOKS
    static void before_request(StartupControl& control,
                               void (*hook)(void*) noexcept, void* context) {
        control.before_request_cas_ = hook;
        control.test_context_ = context;
    }
    static void before_close(StartupControl& control,
                             void (*hook)(void*) noexcept, void* context) {
        control.before_close_cas_ = hook;
        control.test_context_ = context;
    }
#endif
};
} // namespace obd::ublk

using namespace obd::ublk;
using Owner = StartupControlTestAccess;

TEST_CASE("ublk: startup release rejects foreign and invalid identities",
          "[ublk-unit][startup-control]") {
    REQUIRE_THROWS_AS(StartupControl(0), std::invalid_argument);
    StartupControl control(19);
    const auto initial = control.snapshot();
    for (const auto sequence : {uint64_t{0}, uint64_t{1} << 63,
                                std::numeric_limits<uint64_t>::max()}) {
        REQUIRE(control.request_release(19, sequence) ==
                ReleaseRequestResult::stale);
        REQUIRE(Owner::pending(control) == 0);
    }
    REQUIRE(control.request_release(18, 1) == ReleaseRequestResult::stale);
    REQUIRE(Owner::pending(control) == 0);
    REQUIRE(control.request_release(19, 1) == ReleaseRequestResult::accepted);
    REQUIRE(Owner::pending(control) == 1);
    const auto pending = control.snapshot();
    REQUIRE(pending.version > initial.version);
    REQUIRE(pending.version % 2 == 1);
    REQUIRE(pending.phase == StartupPhase::releasing);
    REQUIRE(pending.release_sequence == 1);
    REQUIRE(pending.release == StartupRelease::pending);
}

TEST_CASE("ublk: ready handoff excludes a later startup release",
          "[ublk-unit][startup-control]") {
    StartupControl control(19);
    // Ready wins the admission CAS. The caller must route later destroy to
    // the ready Device; it cannot mistake it for an accepted startup release.
    REQUIRE(Owner::ready(control));
    REQUIRE(control.request_release(19, 1) ==
            ReleaseRequestResult::not_startup);
    REQUIRE(Owner::pending(control) == 0);
    REQUIRE(Owner::ready(control));
}

TEST_CASE("ublk: accepted release prevents ready handoff until resolved",
          "[ublk-unit][startup-control]") {
    StartupControl control(19);
    // Release wins first. A ready outcome cannot orphan this command even
    // if the original END completes successfully before release is handled.
    REQUIRE(control.request_release(19, 7) == ReleaseRequestResult::accepted);
    REQUIRE_FALSE(Owner::ready(control));
    REQUIRE(Owner::pending(control) == 7);
    REQUIRE(control.request_release(19, 7) == ReleaseRequestResult::stale);
    REQUIRE(control.request_release(19, 8) == ReleaseRequestResult::busy);
    REQUIRE(Owner::pending(control) == 7);

    SECTION("a refusal permits a new correlated attempt") {
        Owner::refuse(control, 7);
        REQUIRE(Owner::pending(control) == 0);
        REQUIRE(control.request_release(19, 7) == ReleaseRequestResult::stale);
        REQUIRE(control.request_release(19, 8) == ReleaseRequestResult::accepted);
        REQUIRE_FALSE(Owner::ready(control));
        REQUIRE(Owner::pending(control) == 8);
    }
    SECTION("ready can take over after the refusal is settled") {
        Owner::refuse(control, 7);
        REQUIRE(Owner::ready(control));
        REQUIRE(control.request_release(19, 8) ==
                ReleaseRequestResult::not_startup);
    }
    SECTION("successful release closes admission before exposing the ack") {
        Owner::released(control, 7);
        REQUIRE(Owner::pending(control) == 0);
        REQUIRE(control.request_release(19, 8) ==
                ReleaseRequestResult::not_startup);
        REQUIRE(Owner::ready(control));
    }
}

TEST_CASE("ublk: startup sequence boundary does not alias ready state",
          "[ublk-unit][startup-control]") {
    StartupControl control(std::numeric_limits<uint64_t>::max());
    constexpr auto last = (uint64_t{1} << 63) - 1;
    REQUIRE(control.request_release(std::numeric_limits<uint64_t>::max(), last) ==
            ReleaseRequestResult::accepted);
    REQUIRE(Owner::pending(control) == last);
    REQUIRE_FALSE(Owner::ready(control));
    Owner::refuse(control, last);
    REQUIRE(control.request_release(std::numeric_limits<uint64_t>::max(), last) ==
            ReleaseRequestResult::stale);
    REQUIRE(Owner::ready(control));
    REQUIRE(Owner::pending(control) == 0);
}

TEST_CASE("ublk: release before startup grant settles without device publication",
          "[ublk-unit][startup-control]") {
    StartupControl control(19);
    REQUIRE_FALSE(control.release_before_start());
    REQUIRE(control.request_release(19, 7) == ReleaseRequestResult::accepted);
    REQUIRE(control.release_before_start());
    const auto terminal = control.snapshot();
    REQUIRE(terminal.phase == StartupPhase::released);
    REQUIRE(terminal.release == StartupRelease::released);
    REQUIRE(terminal.release_sequence == 7);
    REQUIRE(terminal.device_id == UINT32_MAX);
    REQUIRE_FALSE(terminal.control_pending);
    REQUIRE(terminal.kernel_debt_known);
    REQUIRE(terminal.raw_published == 0);
    REQUIRE(terminal.raw_retired == 0);
    REQUIRE_FALSE(control.release_before_start());
    REQUIRE(control.request_release(19, 8) ==
            ReleaseRequestResult::not_startup);
}

#ifdef OBD_TEST_HOOKS
TEST_CASE("ublk: ready closure wins after release has read open admission",
          "[ublk-unit][startup-control]") {
    StartupControl control(19);
    struct Context {
        StartupControl& control;
        bool closed = false;
    } context{control};
    Owner::before_request(control, [](void* opaque) noexcept {
        auto& state = *static_cast<Context*>(opaque);
        state.closed = Owner::ready(state.control);
    }, &context);
    REQUIRE(control.request_release(19, 1) ==
            ReleaseRequestResult::not_startup);
    REQUIRE(context.closed);
    REQUIRE(Owner::pending(control) == 0);
}

TEST_CASE("ublk: release wins after ready has read idle admission",
          "[ublk-unit][startup-control]") {
    StartupControl control(19);
    struct Context {
        StartupControl& control;
        ReleaseRequestResult result = ReleaseRequestResult::stale;
    } context{control};
    Owner::before_close(control, [](void* opaque) noexcept {
        auto& state = *static_cast<Context*>(opaque);
        state.result = state.control.request_release(19, 1);
    }, &context);
    REQUIRE_FALSE(Owner::ready(control));
    REQUIRE(context.result == ReleaseRequestResult::accepted);
    REQUIRE(Owner::pending(control) == 1);
    Owner::before_close(control, nullptr, nullptr);
    Owner::released(control, 1);
    REQUIRE(control.request_release(19, 2) ==
            ReleaseRequestResult::not_startup);
}
#endif

TEST_CASE("ublk: startup observations distinguish cancellation from retirement",
          "[ublk-unit][startup-control]") {
    StartupControl control(19);
    const auto initial = control.snapshot();
    REQUIRE(initial.generation == 19);
    REQUIRE(initial.device_id == UINT32_MAX);
    REQUIRE(initial.queue_id == UINT16_MAX);
    REQUIRE(initial.phase == StartupPhase::preparing);

    StartupSnapshot value;
    value.phase = StartupPhase::retained;
    value.operation = StartupOperation::end;
    value.device_id = 7;
    value.queue_id = 1;
    value.reason_errno = EIO;
    value.control_pending = true;
    value.cancel_known = true;
    value.cancel_result = -ENOENT;
    value.raw_published = 4;
    value.raw_retired = 1;
    Owner::publish(control, value);
    auto observed = control.snapshot();
    REQUIRE(observed.generation == 19);
    REQUIRE(observed.version > initial.version);
    REQUIRE(observed.version % 2 == 0);
    REQUIRE(observed.phase == StartupPhase::retained);
    REQUIRE(observed.control_pending);
    REQUIRE_FALSE(observed.original_known);
    REQUIRE(observed.cancel_known);
    REQUIRE(observed.cancel_result == -ENOENT);
    REQUIRE(observed.raw_published == 4);
    REQUIRE(observed.raw_retired == 1);

    const auto pending_version = observed.version;
    value.control_pending = false;
    value.original_known = true;
    value.original_result = -EINTR;
    value.kernel_debt_known = false;
    Owner::publish(control, value);
    observed = control.snapshot();
    REQUIRE(observed.version > pending_version);
    REQUIRE(observed.original_known);
    REQUIRE(observed.original_result == -EINTR);
    REQUIRE_FALSE(observed.control_pending);
    REQUIRE_FALSE(observed.kernel_debt_known);
    REQUIRE(observed.raw_retired < observed.raw_published);
    REQUIRE(observed.phase == StartupPhase::retained);
}

TEST_CASE("ublk: final pre-owner cleanup closes release admission",
          "[ublk-unit][startup-control]") {
    StartupControl control(19);
    REQUIRE(control.finish_before_start());
    REQUIRE(control.snapshot().phase == StartupPhase::released);
    REQUIRE(control.snapshot().release == StartupRelease::none);
    REQUIRE(control.request_release(19, 1) == ReleaseRequestResult::not_startup);
    REQUIRE(control.finish_before_start());
}

#ifdef OBD_TEST_HOOKS
TEST_CASE("ublk: pre-owner cleanup settles a release that wins admission",
          "[ublk-unit][startup-control]") {
    StartupControl control(19);
    struct Context {
        StartupControl& control;
        ReleaseRequestResult result = ReleaseRequestResult::stale;
    } context{control};
    Owner::before_close(control, [](void* opaque) noexcept {
        auto& state = *static_cast<Context*>(opaque);
        Owner::before_close(state.control, nullptr, nullptr);
        state.result = state.control.request_release(19, 7);
    }, &context);
    REQUIRE(control.finish_before_start());
    REQUIRE(context.result == ReleaseRequestResult::accepted);
    const auto terminal = control.snapshot();
    REQUIRE(terminal.phase == StartupPhase::released);
    REQUIRE(terminal.release == StartupRelease::released);
    REQUIRE(terminal.release_sequence == 7);
    REQUIRE(control.request_release(19, 8) == ReleaseRequestResult::not_startup);
}

TEST_CASE("ublk: pre-owner cleanup rejects a release that loses admission",
          "[ublk-unit][startup-control]") {
    StartupControl control(19);
    Owner::before_request(control, [](void* opaque) noexcept {
        (void)static_cast<StartupControl*>(opaque)->finish_before_start();
    }, &control);
    REQUIRE(control.request_release(19, 7) == ReleaseRequestResult::not_startup);
    REQUIRE(control.snapshot().phase == StartupPhase::released);
    REQUIRE(control.snapshot().release == StartupRelease::none);
}
#endif
