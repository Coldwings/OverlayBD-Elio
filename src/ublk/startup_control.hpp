#pragma once

#include "ublk/startup_test_hooks.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>

namespace obd::ublk {

enum class StartupPhase : uint8_t {
    preparing, control_pending, retained, releasing, ready, released,
};
enum class StartupOperation : uint8_t { none, start, end, stop, remove };
enum class StartupRelease : uint8_t { none, pending, refused, released };
enum class ReleaseRequestResult : uint8_t { accepted, stale, busy, not_startup };

/// Non-owning facts. No strings, callbacks, allocations, or resource handles.
struct StartupSnapshot {
    uint64_t generation = 0;
    uint64_t version = 0;
    StartupPhase phase = StartupPhase::preparing;
    StartupOperation operation = StartupOperation::none;
    uint32_t device_id = UINT32_MAX;
    uint16_t queue_id = UINT16_MAX;
    int reason_errno = 0;
    int original_result = 0;
    int cancel_result = 0;
    bool original_known = false;
    bool cancel_known = false;
    bool control_pending = false;
    bool kernel_debt_known = true;
    uint64_t raw_published = 0;
    uint64_t raw_retired = 0;
    uint64_t release_sequence = 0;
    StartupRelease release = StartupRelease::none;
    int release_errno = 0;
};

/// Observer/mailbox, never a resource owner. Its lifetime covers the awaited
/// startup operation, including retained failure and explicit release. Once a
/// ready Device is returned, its normal owner handles subsequent stop/release.
/// Destroying a retained startup task/runtime violates that operation's
/// lifetime contract; cancellation of an observer does not cancel its loans.
class StartupControl {
public:
    explicit StartupControl(uint64_t generation)
        : generation_(generation) {
        if (generation == 0) throw std::invalid_argument("zero startup generation");
        fields_[device].store(UINT32_MAX);
        fields_[queue].store(UINT16_MAX);
        fields_[debt_known].store(1);
    }
    StartupControl(const StartupControl&) = delete;
    StartupControl& operator=(const StartupControl&) = delete;
#ifdef OBD_TEST_HOOKS
    void set_test_hooks(detail::StartupTestHooks* hooks) noexcept { test_hooks_ = hooks; }
#endif

    StartupSnapshot snapshot() const noexcept {
        std::array<uint64_t, count> values{};
        uint64_t version, request, acknowledged;
        do {
            version = version_.load();
            if (version & 1) continue;
            for (size_t i = 0; i < count; ++i) values[i] = fields_[i].load();
            request = requested_.load();
            acknowledged = acknowledged_.load();
            if (version == version_.load()) break;
        } while (true);
        StartupSnapshot s;
        s.generation = generation_;
        s.version = version;
        s.phase = static_cast<StartupPhase>(values[phase]);
        s.operation = static_cast<StartupOperation>(values[operation]);
        s.device_id = static_cast<uint32_t>(values[device]);
        s.queue_id = static_cast<uint16_t>(values[queue]);
        s.reason_errno = static_cast<int>(static_cast<int64_t>(values[reason]));
        s.original_result = static_cast<int>(static_cast<int64_t>(values[original]));
        s.cancel_result = static_cast<int>(static_cast<int64_t>(values[cancel]));
        s.original_known = values[original_known] != 0;
        s.cancel_known = values[cancel_known] != 0;
        s.control_pending = values[control_pending] != 0;
        s.kernel_debt_known = values[debt_known] != 0;
        s.raw_published = values[published];
        s.raw_retired = values[retired];
        s.release_sequence = values[release_sequence];
        s.release = static_cast<StartupRelease>(values[release]);
        s.release_errno = static_cast<int>(static_cast<int64_t>(values[release_error]));
        // request_release() is the acceptance linearization point and may run
        // while the Device owner is suspended in caller cleanup. Project the
        // outstanding mailbox request at read time so status/list cannot deny
        // that accepted attempt merely because the owner has not resumed to
        // persist the same facts yet. Only the Device owner writes fields_.
        if ((request & closed_bit) == 0 && request != acknowledged) {
            s.phase = StartupPhase::releasing;
            s.release_sequence = request;
            s.release = StartupRelease::pending;
            s.release_errno = 0;
        }
        return s;
    }

    /// One outstanding request. The caller chooses a strictly increasing,
    /// nonzero sequence within this immutable device-owner generation.
    ReleaseRequestResult request_release(uint64_t generation,
                                         uint64_t sequence) noexcept {
        if (generation != generation_ || sequence == 0 || sequence >= closed_bit)
            return ReleaseRequestResult::stale;
        auto current = requested_.load();
        if (current & closed_bit) return ReleaseRequestResult::not_startup;
        if (sequence <= current) return ReleaseRequestResult::stale;
        if (current != acknowledged_.load()) return ReleaseRequestResult::busy;
#ifdef OBD_TEST_HOOKS
        if (before_request_cas_) before_request_cas_(test_context_);
#endif
        if (!requested_.compare_exchange_strong(current, sequence))
            return current & closed_bit ? ReleaseRequestResult::not_startup
                                        : ReleaseRequestResult::busy;
#ifdef OBD_TEST_HOOKS
        if (test_hooks_) test_hooks_->observe({.kind = detail::StartupTestKind::release_admitted,
            .value = generation_, .release_sequence = sequence});
#endif
        return ReleaseRequestResult::accepted;
    }

    /// Owner-only path before invoking Device::create/attach or publishing any
    /// operation. Completes an explicit release without granting startup merely
    /// to make its cancellation serviceable. The caller must leave this phase
    /// permanently on true; it must not create a device afterwards.
    bool release_before_start() noexcept {
        if (owner_entered_.load()) return false;
        const auto sequence = release_request();
        if (sequence == 0) return false;
        finish_release_admission(sequence);
        StartupSnapshot value;
        value.phase = StartupPhase::released;
        value.release_sequence = sequence;
        value.release = StartupRelease::released;
        publish(value);
        return true;
    }
    /// Owner-only finalization when setup failed before Device acquired any
    /// resources. Close admission even without a request: a command arriving
    /// during caller cleanup must not receive a promise nobody will service.
    /// Competing release admission either wins and is settled, or observes the
    /// closed bit. Does not replace release_before_start while awaiting grant.
    bool finish_before_start() noexcept {
        if (owner_entered_.load()) return false;
        auto current = requested_.load();
        for (;;) {
            if (current & closed_bit) return true;
#ifdef OBD_TEST_HOOKS
            if (before_close_cas_) before_close_cas_(test_context_);
#endif
            if (requested_.compare_exchange_strong(current, current | closed_bit)) break;
        }
        const bool accepted = current != acknowledged_.load();
        acknowledged_.store(current);
        StartupSnapshot value;
        value.phase = StartupPhase::released;
        if (accepted) {
            value.release_sequence = current;
            value.release = StartupRelease::released;
        }
        publish(value);
        return true;
    }
    /// True only once the Device coroutine body has accepted responsibility;
    /// task allocation and direct-await setup do not constitute this handoff.
    bool owner_entered() const noexcept { return owner_entered_.load(); }

private:
    friend class Device;
    friend struct StartupControlTestAccess;
    enum Field : size_t {
        phase, operation, device, queue, reason, original, cancel,
        original_known, cancel_known, control_pending, debt_known,
        published, retired, release_sequence, release, release_error, count,
    };
    void publish(const StartupSnapshot& s) noexcept {
        // Cold single-writer path. All fields are atomic and seq_cst, so the
        // version check cannot turn an ordinary-field data race into UB.
        version_.fetch_add(1);
        const std::array<uint64_t, count> values{
            static_cast<uint64_t>(s.phase), static_cast<uint64_t>(s.operation),
            s.device_id, s.queue_id, static_cast<uint64_t>(s.reason_errno),
            static_cast<uint64_t>(s.original_result),
            static_cast<uint64_t>(s.cancel_result), s.original_known,
            s.cancel_known, s.control_pending, s.kernel_debt_known,
            s.raw_published, s.raw_retired, s.release_sequence,
            static_cast<uint64_t>(s.release), static_cast<uint64_t>(s.release_errno),
        };
        for (size_t i = 0; i < count; ++i) fields_[i].store(values[i]);
        version_.fetch_add(1);
    }
    uint64_t release_request() const noexcept {
        const auto sequence = requested_.load();
        if (sequence & closed_bit) return 0;
        return sequence != acknowledged_.load() ? sequence : 0;
    }
    bool close_release_admission() noexcept {
        auto current = requested_.load();
        if (current & closed_bit) return true;
        if (current != acknowledged_.load()) return false;
#ifdef OBD_TEST_HOOKS
        if (before_close_cas_) before_close_cas_(test_context_);
#endif
        return requested_.compare_exchange_strong(current, current | closed_bit);
    }
    void acknowledge_release(uint64_t sequence) noexcept {
        acknowledged_.store(sequence);
    }
    void finish_release_admission(uint64_t sequence) noexcept {
        // While this request is outstanding no subsequent request can enter.
        // Close before ack, so completion cannot orphan a newly accepted one.
        auto expected = sequence;
        (void)requested_.compare_exchange_strong(expected, sequence | closed_bit);
        acknowledged_.store(sequence);
    }
    const uint64_t generation_;
    static constexpr uint64_t closed_bit = uint64_t{1} << 63;
    std::atomic<uint64_t> version_{0};
    std::array<std::atomic<uint64_t>, count> fields_{};
    std::atomic<uint64_t> requested_{0};
    std::atomic<uint64_t> acknowledged_{0};
    std::atomic<bool> owner_entered_{false};
#ifdef OBD_TEST_HOOKS
    detail::StartupTestHooks* test_hooks_ = nullptr;
    // Deterministic interleavings at the two competing admission CAS sites.
    void (*before_request_cas_)(void*) noexcept = nullptr;
    void (*before_close_cas_)(void*) noexcept = nullptr;
    void* test_context_ = nullptr;
#endif
};

/// Terminal indication after explicit release, carrying no resource owner.
class StartupReleased final : public std::exception {
public:
    const char* what() const noexcept override {
        return "ublk startup owner explicitly released";
    }
};

} // namespace obd::ublk
