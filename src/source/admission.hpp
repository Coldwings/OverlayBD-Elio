// Read admission funnel (ADR-0012): every remote range request of a
// device passes one shared funnel before reaching the source client
// (registry or DART). The funnel enforces client-side priority over a
// QoS-less source:
//
//   * OnDemand — a ublk miss blocking a guest: admitted immediately and
//     unconditionally, even if that momentarily exceeds the concurrency
//     window. Delaying a guest-visible miss to protect a window is never
//     correct.
//   * Prefetch (warm-up: structural head/tail prefetch and trace
//     replay) and Fill (background layer fill) — the
//     scavenger class: admitted only when no on-demand request is in
//     flight AND total in-flight requests are below the AIMD window.
//     Within the class, Prefetch outranks Fill (two FIFO queues drained
//     prefetch-first): traced data is needed soon, fill has the whole
//     runtime.
//   * Scavenger requests are size-capped near 1 MiB (cap_request), which
//     bounds the head-of-line delay an arriving on-demand read can
//     suffer behind an already-issued scavenger request. Callers split
//     larger warm requests; LayerStore's populate/fill already coalesce
//     contiguous missing 64 KiB extents up to this cap (ADR-0011 bulk
//     path).
//
// The window is NOT operator-configured; it is AIMD-managed from
// observed on-demand latency (LEDBAT-style scavenger: consume spare
// capacity, yield on the first congestion signal). Exact signals:
//
//   * sample: the acquire-to-release wall-clock latency of each OnDemand
//     request (it brackets exactly one remote fetch);
//   * baseline: an EMA (alpha = 1/8) of past samples, floored at 1 ms —
//     local-speed samples carry no congestion signal;
//   * flat (sample <= baseline * 3/2): additive increase, window += 1
//     (capped at window_max, default 32);
//   * rise (sample > baseline * 3/2): multiplicative decrease, window
//     halved (floored at window_min, default 1);
//   * window starts at window_init (default 2).
//
// Extent-granular dedup is NOT the funnel's job: it lives in the
// LayerStore's in-flight map (ADR-0011) below — a scavenger request for
// an extent already being fetched joins that fetch and never reaches the
// funnel.
//
// Wiring: image assembly creates ONE funnel per device open and threads
// it through LayerStore::Config::funnel (misses = OnDemand, populate =
// Prefetch, background fill = Fill) and through AdmissionSource for the
// remote-only paths (no per-layer dir, ADR-0016 degrade, the trace blob
// fetch). Registry/DART clients stay class-agnostic: the funnel governs
// admission, never source selection (ADR-0005 fallback untouched).
//
// Fill's own per-second max_mbps bandwidth throttle is a DIFFERENT
// mechanism and stays: the throttle bounds fill's byte rate, the funnel
// governs WHEN fill's requests may enter the source at all.
//
// Lifetime/concurrency: the funnel is shared_ptr-held by every source
// that admits through it and is safe to call from any Elio worker
// thread. acquire() waiters must not be cancelled (no cancel tokens on
// these paths — the same precedent as LayerStore's in-flight joins): a
// waiter cancelled after admit_locked reserved its slot would leak the
// slot. The BOUNDED variant (acquire_scavenger_bounded) is the sole
// exception — it reconciles the reservation before returning (see its
// contract), making it safe for callers that must not wait indefinitely
// (warm-up's skip semantics, issue #35).
#pragma once

#include "source/blob_source.hpp"

#include <elio/coro/task.hpp>
#include <elio/sync/event.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace obd::source {

/// The traffic class of one remote range request (ADR-0012).
enum class ReadClass : int {
    OnDemand = 0,  // guest-blocking miss: unconditional admission
    Prefetch = 1,  // warm-up (structural, trace replay): scavenger, outranks Fill
    Fill = 2,      // background layer fill: scavenger, lowest priority
};

class AdmissionFunnel final {
public:
    struct Config {
        uint32_t window_init = 2;  // small start; grows into idle capacity
        uint32_t window_min = 1;   // floor: never fully starve scavengers
        uint32_t window_max = 32;  // ceiling: bounds worst-case queueing
        uint32_t ai_step = 1;      // additive increase per flat sample
        uint32_t md_percent = 50;  // window -> 50% on a latency rise
        uint32_t rise_percent = 150;  // sample > baseline * 150% = a rise
        std::chrono::nanoseconds baseline_floor{1000 * 1000};  // 1 ms
        size_t scavenger_size_cap = 1024 * 1024;  // ~1 MiB (ADR-0012)
    };

    // (The default constructor is out-of-line: a nested Config with
    // default member initializers cannot be aggregate-initialized in a
    // default argument inside the class body on GCC.)
    AdmissionFunnel();
    explicit AdmissionFunnel(Config cfg);
    ~AdmissionFunnel() = default;
    AdmissionFunnel(const AdmissionFunnel&) = delete;
    AdmissionFunnel& operator=(const AdmissionFunnel&) = delete;

    /// One admitted request. Move-only RAII: destruction releases the
    /// slot and, for OnDemand, feeds the measured acquire-to-release
    /// latency into the AIMD controller. The destructor may wake queued
    /// scavengers; it never blocks.
    class Permit {
    public:
        Permit() = default;
        ~Permit() { reset(); }
        Permit(Permit&& o) noexcept { *this = std::move(o); }
        Permit& operator=(Permit&& o) noexcept {
            if (this != &o) {
                reset();
                funnel_ = o.funnel_;
                cls_ = o.cls_;
                start_ = o.start_;
                o.funnel_ = nullptr;
            }
            return *this;
        }
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;

        /// Releases the slot early (idempotent); also runs at scope exit.
        void reset() {
            if (funnel_ == nullptr) return;
            AdmissionFunnel* f = funnel_;
            funnel_ = nullptr;
            f->release(cls_, std::chrono::steady_clock::now() - start_);
        }

    private:
        friend class AdmissionFunnel;
        Permit(AdmissionFunnel* f, ReadClass cls,
               std::chrono::steady_clock::time_point start)
            : funnel_(f), cls_(cls), start_(start) {}

        AdmissionFunnel* funnel_ = nullptr;
        ReadClass cls_ = ReadClass::OnDemand;
        std::chrono::steady_clock::time_point start_{};
    };

    /// Admits one request of `cls`. OnDemand never suspends. A scavenger
    /// suspends until no on-demand request is in flight and the window
    /// has room; the returned Permit holds the reserved slot. The
    /// caller's remote fetch must happen while holding the Permit — its
    /// lifetime is the latency sample.
    elio::coro::task<Permit> acquire(ReadClass cls);

    /// Scavenger-only bounded acquire (Prefetch or Fill): like
    /// acquire(), but if the gate has not opened within `timeout` the
    /// waiter dequeues itself and the result is std::nullopt — the
    /// caller SKIPS the request (warm-up's opportunism contract: a
    /// window that cannot start promptly is skipped, never awaited,
    /// issue #35). Cancel-safe where acquire() is not: a waiter whose
    /// slot was already reserved by admit_locked when the timeout fires
    /// ADOPTS the slot (returns a Permit) instead of leaking it; a
    /// waiter still queued is removed under mu_ before returning. The
    /// AIMD/latency accounting is untouched (no Permit, no sample).
    /// OnDemand is refused outright (`std::nullopt`, logged) —
    /// unconditional admission is its contract, so a bounded on-demand
    /// call is always a caller bug.
    elio::coro::task<std::optional<Permit>> acquire_scavenger_bounded(
        ReadClass cls, std::chrono::milliseconds timeout);

    /// Clamps a request size to the class's cap: scavenger requests are
    /// capped near 1 MiB (the caller splits larger warm requests);
    /// OnDemand is uncapped (its sizes are bounded upstream by the
    /// registry client's max_response_size and the extent granularity).
    size_t cap_request(ReadClass cls, size_t bytes) const {
        return cls == ReadClass::OnDemand
                   ? bytes
                   : std::min(bytes, cfg_.scavenger_size_cap);
    }

    /// Feeds an on-demand latency sample into the AIMD controller
    /// directly (tests); may wake queued scavengers when the window
    /// grows. Production samples flow through Permit.
    void note_on_demand_latency(std::chrono::nanoseconds sample);

    // Observability (relaxed snapshots, safe from any thread).
    uint32_t window() const {
        return window_.load(std::memory_order_relaxed);
    }
    uint64_t inflight_on_demand() const {
        return inflight_on_demand_.load(std::memory_order_relaxed);
    }
    uint64_t inflight_total() const {
        return inflight_total_.load(std::memory_order_relaxed);
    }
    uint64_t on_demand_admissions() const {
        return on_demand_admissions_.load(std::memory_order_relaxed);
    }
    uint64_t scavenger_admissions() const {
        return scavenger_admissions_.load(std::memory_order_relaxed);
    }
    /// Times a scavenger request had to queue instead of entering
    /// immediately — the observable "the funnel is throttling" signal.
    uint64_t scavenger_waits() const {
        return scavenger_waits_.load(std::memory_order_relaxed);
    }

    /// Test-only hook invoked on the scavenger slow path between the
    /// fast-path gate check and the waiter queue push — the window in
    /// which a concurrent release could otherwise be lost. Never set in
    /// production (same precedent as LayerStore's write_hook_).
    void set_gap_hook_for_test(std::function<void()> hook) {
        gap_hook_ = std::move(hook);
    }

private:
    struct Waiter {
        elio::sync::event admitted;
        /// Set under mu_ when admit_locked pops the waiter and reserves
        /// its slot; once true the slot is the waiter's to adopt even if
        /// its bounded wait has already timed out (the reconciliation
        /// that makes acquire_scavenger_bounded cancel-safe). Atomic:
        /// the bounded acquire reads it again under mu_ after the timed
        /// wait, but also once lock-free between its own admit_locked
        /// and that wait.
        std::atomic<bool> reserved{false};
    };

    void release(ReadClass cls, std::chrono::nanoseconds latency);
    void apply_sample(std::chrono::nanoseconds sample);
    // Admits queued scavengers (Prefetch first) while the gate is open.
    // Call with mu_ held; the returned waiters must be woken AFTER
    // releasing mu_ (their slots are already reserved).
    void admit_locked(std::vector<std::shared_ptr<Waiter>>& wake);

    const Config cfg_;

    std::mutex mu_;
    std::deque<std::shared_ptr<Waiter>> prefetch_q_;  // drained first
    std::deque<std::shared_ptr<Waiter>> fill_q_;
    uint64_t baseline_ns_ = 0;  // EMA of on-demand samples (under mu_)
    bool baseline_valid_ = false;

    std::atomic<uint32_t> window_;
    std::atomic<uint64_t> inflight_on_demand_{0};
    std::atomic<uint64_t> inflight_total_{0};
    std::atomic<uint64_t> on_demand_admissions_{0};
    std::atomic<uint64_t> scavenger_admissions_{0};
    std::atomic<uint64_t> scavenger_waits_{0};
    std::function<void()> gap_hook_;  // test-only, see set_gap_hook_for_test
};

using AdmissionFunnelPtr = std::shared_ptr<AdmissionFunnel>;

/// AdmissionSource — a BlobSource decorator that routes every remote
/// request of a source without its own funnel wiring through a shared
/// funnel: pread = OnDemand, populate = Prefetch (split at the
/// scavenger size cap). Image assembly wraps the remote-only paths
/// (a lower without a dir, the ADR-0016 degrade path, the trace blob
/// fetch) so that ALL remote reads of a device pass the funnel —
/// grep-auditable as the only pread/populate forwarding below the
/// format layers besides LayerStore's.
class AdmissionSource final : public BlobSource {
public:
    AdmissionSource(BlobSourcePtr inner, AdmissionFunnelPtr funnel);

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;
    elio::coro::task<ssize_t> populate(uint64_t offset,
                                       size_t len) override;

    uint64_t size() const noexcept override { return inner_->size(); }
    std::string_view label() const noexcept override {
        return inner_->label();
    }

private:
    BlobSourcePtr inner_;
    AdmissionFunnelPtr funnel_;
};

}  // namespace obd::source
