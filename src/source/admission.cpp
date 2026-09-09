// Read admission funnel. See admission.hpp for the contract, the class
// taxonomy, and the exact AIMD signals (ADR-0012).
#include "source/admission.hpp"

#include <algorithm>

namespace obd::source {

AdmissionFunnel::AdmissionFunnel() : AdmissionFunnel(Config{}) {}

AdmissionFunnel::AdmissionFunnel(Config cfg) : cfg_(cfg) {
    const uint32_t lo = std::max(cfg_.window_min, 1u);
    const uint32_t hi = std::max(cfg_.window_max, lo);
    const uint32_t init = std::min(std::max(cfg_.window_init, lo), hi);
    window_.store(init, std::memory_order_relaxed);
}

void AdmissionFunnel::apply_sample(std::chrono::nanoseconds sample) {
    const uint64_t s = static_cast<uint64_t>(
        std::max(sample, cfg_.baseline_floor).count());
    if (!baseline_valid_) {
        // The first sample anchors the baseline; it is not yet evidence
        // of flatness or rise.
        baseline_ns_ = s;
        baseline_valid_ = true;
        return;
    }
    const uint32_t lo = std::max(cfg_.window_min, 1u);
    const uint32_t hi = std::max(cfg_.window_max, lo);
    uint32_t w = window_.load(std::memory_order_relaxed);
    if (s > baseline_ns_ * cfg_.rise_percent / 100) {
        // Latency rise: multiplicative decrease (halving by default).
        w = std::max(lo, w * cfg_.md_percent / 100);
    } else {
        // Flat: additive increase into the idle capacity.
        w = std::min(hi, w + cfg_.ai_step);
    }
    window_.store(w, std::memory_order_relaxed);
    // EMA with alpha = 1/8: the baseline tracks slow drift of the
    // uncongested latency without chasing single spikes.
    baseline_ns_ = (7 * baseline_ns_ + s) / 8;
}

void AdmissionFunnel::admit_locked(
    std::vector<std::shared_ptr<Waiter>>& wake) {
    while (inflight_on_demand_.load(std::memory_order_relaxed) == 0 &&
           inflight_total_.load(std::memory_order_relaxed) <
               window_.load(std::memory_order_relaxed)) {
        // Two-level scavenger queue: trace replay (Prefetch) outranks
        // background fill (ADR-0012).
        std::shared_ptr<Waiter> w;
        if (!prefetch_q_.empty()) {
            w = prefetch_q_.front();
            prefetch_q_.pop_front();
        } else if (!fill_q_.empty()) {
            w = fill_q_.front();
            fill_q_.pop_front();
        } else {
            break;
        }
        // Reserve the slot now; the waiter adopts it on wake and must
        // not re-check the gate.
        inflight_total_.fetch_add(1, std::memory_order_relaxed);
        scavenger_admissions_.fetch_add(1, std::memory_order_relaxed);
        wake.push_back(std::move(w));
    }
}

elio::coro::task<AdmissionFunnel::Permit> AdmissionFunnel::acquire(
    ReadClass cls) {
    const auto start = std::chrono::steady_clock::now();
    if (cls == ReadClass::OnDemand) {
        // Unconditional, even past the window: a guest-visible miss is
        // never delayed to protect the window (ADR-0012). Under mu_ so
        // the scavenger gate check never races the on-demand count.
        std::lock_guard lk(mu_);
        inflight_on_demand_.fetch_add(1, std::memory_order_relaxed);
        inflight_total_.fetch_add(1, std::memory_order_relaxed);
        on_demand_admissions_.fetch_add(1, std::memory_order_relaxed);
        co_return Permit(this, cls, start);
    }
    {
        std::lock_guard lk(mu_);
        if (inflight_on_demand_.load(std::memory_order_relaxed) == 0 &&
            inflight_total_.load(std::memory_order_relaxed) <
                window_.load(std::memory_order_relaxed)) {
            inflight_total_.fetch_add(1, std::memory_order_relaxed);
            scavenger_admissions_.fetch_add(1, std::memory_order_relaxed);
            co_return Permit(this, cls, start);
        }
    }
    // Queue behind the class's FIFO and wait for a releaser to reserve a
    // slot for us. The event is one-shot per waiter; waiters on these
    // paths are never cancelled (see the header contract).
    scavenger_waits_.fetch_add(1, std::memory_order_relaxed);
    if (gap_hook_) gap_hook_();  // test-only: injects the lost-wakeup gap
    auto w = std::make_shared<Waiter>();
    std::vector<std::shared_ptr<Waiter>> wake;
    {
        std::lock_guard lk(mu_);
        (cls == ReadClass::Prefetch ? prefetch_q_ : fill_q_).push_back(w);
        // Push and re-admit in ONE critical section: a release that
        // opened the gate between the fast-path check above and this
        // push then pops our own waiter right here (FIFO order is
        // preserved — our push precedes the admit) instead of leaving
        // it queued with nobody left to wake it (lost wakeup).
        admit_locked(wake);
    }
    for (auto& wk : wake) wk->admitted.set();
    co_await w->admitted.wait();
    co_return Permit(this, cls, start);
}

void AdmissionFunnel::release(ReadClass cls,
                              std::chrono::nanoseconds latency) {
    std::vector<std::shared_ptr<Waiter>> wake;
    {
        std::lock_guard lk(mu_);
        inflight_total_.fetch_sub(1, std::memory_order_relaxed);
        if (cls == ReadClass::OnDemand) {
            inflight_on_demand_.fetch_sub(1, std::memory_order_relaxed);
            apply_sample(latency);
        }
        admit_locked(wake);
    }
    for (auto& w : wake) w->admitted.set();
}

void AdmissionFunnel::note_on_demand_latency(
    std::chrono::nanoseconds sample) {
    std::vector<std::shared_ptr<Waiter>> wake;
    {
        std::lock_guard lk(mu_);
        apply_sample(sample);
        admit_locked(wake);
    }
    for (auto& w : wake) w->admitted.set();
}

// ---------------------------------------------------------------------------
// AdmissionSource
// ---------------------------------------------------------------------------

AdmissionSource::AdmissionSource(BlobSourcePtr inner,
                                 AdmissionFunnelPtr funnel)
    : inner_(std::move(inner)), funnel_(std::move(funnel)) {}

elio::coro::task<ssize_t> AdmissionSource::pread(void* buf, size_t count,
                                                 uint64_t offset) {
    AdmissionFunnel::Permit permit =
        co_await funnel_->acquire(ReadClass::OnDemand);
    co_return co_await inner_->pread(buf, count, offset);
}

elio::coro::task<ssize_t> AdmissionSource::populate(uint64_t offset,
                                                    size_t len) {
    // Split at the scavenger size cap (ADR-0012: the funnel caps, the
    // caller splits).
    while (len > 0) {
        const size_t chunk = funnel_->cap_request(ReadClass::Prefetch, len);
        AdmissionFunnel::Permit permit =
            co_await funnel_->acquire(ReadClass::Prefetch);
        const ssize_t r = co_await inner_->populate(offset, chunk);
        if (r < 0) co_return r;
        offset += chunk;
        len -= chunk;
    }
    co_return 0;
}

}  // namespace obd::source
