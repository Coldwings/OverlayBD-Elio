// Unit tests: the ADR-0012 read admission funnel (src/source/admission.hpp)
// — class admission rules, the two-level scavenger queue, the AIMD window
// controller, the scavenger size cap, and the LayerStore class wiring.
//
// The AIMD samples here are fed through note_on_demand_latency (synthetic,
// deterministic); blocking behavior is observed with flags set by elio::go
// children plus bounded polling — no fixed-order assumptions beyond the
// funnel's documented guarantees.
//
// Convention: every co_await result is assigned to a local BEFORE the
// REQUIRE (a co_await inside Catch2's REQUIRE decomposition macro is not
// a single evaluation).
#include "source/admission.hpp"
#include "source/layer_store.hpp"

#include "common/sha256.hpp"

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <elio/runtime/spawn.hpp>
#include <elio/sync/event.hpp>
#include <elio/time/timer.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <vector>

using namespace obd;
using std::chrono::milliseconds;

namespace {

using source::AdmissionFunnel;
using source::ReadClass;

constexpr milliseconds kSample{100};

std::vector<uint8_t> slice(const std::vector<uint8_t>& v, size_t off,
                           size_t len) {
    return {v.begin() + static_cast<ptrdiff_t>(off),
            v.begin() + static_cast<ptrdiff_t>(off + len)};
}

/// Polls `pred` with 1 ms coroutine sleeps; bounded.
template <typename Pred>
elio::coro::task<bool> poll_until(Pred&& pred, int max_iters = 20000) {
    for (int i = 0; i < max_iters; ++i) {
        if (pred()) co_return true;
        co_await elio::time::sleep_for(milliseconds(1));
    }
    co_return pred();
}

/// A BlobSource recording populate() calls (offset/len pairs).
class RecordingSource final : public source::BlobSource {
public:
    elio::coro::task<ssize_t> pread(void*, size_t, uint64_t) override {
        co_return 0;
    }
    elio::coro::task<ssize_t> populate(uint64_t offset, size_t len) override {
        calls.emplace_back(offset, len);
        co_return 0;
    }
    uint64_t size() const noexcept override { return 1ULL << 40; }
    std::string_view label() const noexcept override { return "rec"; }

    std::vector<std::pair<uint64_t, size_t>> calls;
};

/// A source whose reads block until `gate` is set (the deterministic
/// "remote request in flight" fixture, same shape as the LayerStore
/// tests' GatedSource).
class GatedSource final : public source::BlobSource {
public:
    explicit GatedSource(std::vector<uint8_t> data)
        : data_(std::move(data)) {}

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override {
        co_await gate.wait();
        if (offset >= data_.size()) co_return 0;
        const size_t n =
            std::min(count, static_cast<size_t>(data_.size() - offset));
        std::memcpy(buf, data_.data() + offset, n);
        co_return static_cast<ssize_t>(n);
    }

    uint64_t size() const noexcept override { return data_.size(); }
    std::string_view label() const noexcept override { return "gated"; }

    elio::sync::event gate;

private:
    std::vector<uint8_t> data_;
};

}  // namespace

TEST_CASE("source: admission funnel admits on-demand unconditionally under a full window",
          "[source]") {
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        AdmissionFunnel funnel;  // window starts at 2
        // Fill the whole window with scavengers.
        auto s1 = co_await funnel.acquire(ReadClass::Prefetch);
        auto s2 = co_await funnel.acquire(ReadClass::Fill);
        REQUIRE(funnel.inflight_total() == 2);
        // On-demand is admitted immediately even past the window.
        auto o1 = co_await funnel.acquire(ReadClass::OnDemand);
        auto o2 = co_await funnel.acquire(ReadClass::OnDemand);
        auto o3 = co_await funnel.acquire(ReadClass::OnDemand);
        REQUIRE(funnel.inflight_on_demand() == 3);
        REQUIRE(funnel.inflight_total() == 5);
        REQUIRE(funnel.window() == 2);  // exceeded, not raised
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: admission funnel blocks scavengers while on-demand is in flight",
          "[source]") {
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        AdmissionFunnel funnel;
        auto od = co_await funnel.acquire(ReadClass::OnDemand);
        std::atomic<int> admitted{0};
        elio::go([&]() -> elio::coro::task<void> {
            auto p = co_await funnel.acquire(ReadClass::Prefetch);
            admitted.fetch_add(1, std::memory_order_relaxed);
        });
        elio::go([&]() -> elio::coro::task<void> {
            auto f = co_await funnel.acquire(ReadClass::Fill);
            admitted.fetch_add(1, std::memory_order_relaxed);
        });
        // While the on-demand request is in flight, neither scavenger
        // enters — even though the window (2) has room.
        co_await elio::time::sleep_for(milliseconds(50));
        REQUIRE(admitted.load() == 0);
        REQUIRE(funnel.scavenger_waits() == 2);
        // Releasing on-demand opens the gate; the window admits both.
        od.reset();
        const bool all = co_await poll_until(
            [&] { return admitted.load() == 2; });
        REQUIRE(all);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: admission funnel bounded scavenger acquire times out and dequeues",
          "[source]") {
    // Issue #35 primitive: a scavenger that cannot be admitted within
    // its timeout skips (nullopt) instead of waiting indefinitely, the
    // dequeued waiter leaves no ghost reservation behind (a leaked slot
    // would wedge the window), and an admission before the timeout
    // still succeeds.
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        AdmissionFunnel funnel;  // window starts at 2
        {
            // Gate open: immediate admission, like acquire().
            auto p = co_await funnel.acquire_scavenger_bounded(
                ReadClass::Prefetch, milliseconds(60));
            REQUIRE(p.has_value());
        }
        {
            // OnDemand is refused outright (real check, not an assert —
            // release builds compile asserts out): unconditional
            // admission is the OnDemand contract, and a bounded
            // on-demand call is always a caller bug. No slot is taken.
            auto bad = co_await funnel.acquire_scavenger_bounded(
                ReadClass::OnDemand, milliseconds(60));
            REQUIRE(!bad.has_value());
            REQUIRE(funnel.inflight_total() == 0);
        }
        // Hold the gate closed with an on-demand request.
        auto od = co_await funnel.acquire(ReadClass::OnDemand);
        const auto t0 = std::chrono::steady_clock::now();
        auto timed_out = co_await funnel.acquire_scavenger_bounded(
            ReadClass::Prefetch, milliseconds(60));
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        REQUIRE(!timed_out.has_value());
        REQUIRE(elapsed >= milliseconds(50));
        REQUIRE(elapsed < std::chrono::seconds(2));
        REQUIRE(funnel.scavenger_waits() == 1);
        // The timeout dequeued the waiter and reserved nothing: only the
        // held on-demand permit remains in flight.
        REQUIRE(funnel.inflight_total() == 1);
        // A bounded waiter queued while the gate is closed is still
        // admitted when the gate opens before its timeout.
        std::atomic<int> admitted{0};
        elio::go([&]() -> elio::coro::task<void> {
            auto p = co_await funnel.acquire_scavenger_bounded(
                ReadClass::Fill, std::chrono::seconds(5));
            if (p.has_value()) {
                admitted.fetch_add(1, std::memory_order_relaxed);
            }
        });
        co_await elio::time::sleep_for(milliseconds(30));
        REQUIRE(admitted.load() == 0);
        od.reset();  // opens the gate
        const bool got = co_await poll_until(
            [&] { return admitted.load() == 1; });
        REQUIRE(got);
        // And no ghost reservation lingers after everything settles:
        // the window is fully free again.
        const bool drained = co_await poll_until(
            [&] { return funnel.inflight_total() == 0; });
        REQUIRE(drained);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: layer store populate skips the extent when the funnel gate stays closed",
          "[source]") {
    // Issue #35 end of the plumbing: with populate_admit_timeout set,
    // a populate whose extent fetch cannot be admitted within the
    // bound fails that extent with -EAGAIN (warm-up skips the window)
    // instead of waiting at the gate indefinitely; once the gate opens
    // the same populate succeeds.
    test::TempDir dir;
    constexpr size_t kExtent = 64 * 1024;
    auto blob = test::pattern_bytes(2 * kExtent, 23);
    const std::string digest = common::Sha256::hex(blob.data(), blob.size());

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto funnel = std::make_shared<AdmissionFunnel>();
        auto* vec = new test::VectorSource(blob);
        source::LayerStore::Config lsc;
        lsc.funnel = funnel;
        lsc.populate_admit_timeout = milliseconds(60);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest, lsc);

        // Hold the gate closed with an on-demand permit (the storm).
        auto od = co_await funnel->acquire(ReadClass::OnDemand);
        const auto t0 = std::chrono::steady_clock::now();
        const ssize_t skipped = co_await store->populate(0, kExtent);
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        REQUIRE(skipped == -EAGAIN);
        REQUIRE(elapsed >= milliseconds(50));
        REQUIRE(elapsed < std::chrono::seconds(2));
        REQUIRE(vec->reads() == 0);  // nothing reached the remote

        // Gate open: the same populate fetches and persists.
        od.reset();
        const ssize_t warmed = co_await store->populate(0, kExtent);
        REQUIRE(warmed == 0);
        REQUIRE(vec->reads() == 1);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: on-demand bypasses queued prefetch with populate timeout",
          "[source]") {
    // Issue #22: a Prefetch waiting at the funnel gate must not publish an
    // in-flight extent that a later OnDemand miss joins. With a positive
    // populate timeout, the OnDemand read must complete with bytes before
    // the unrelated gate holder is released and must not inherit Prefetch's
    // local -EAGAIN skip.
    test::TempDir dir;
    constexpr size_t kExtent = 64 * 1024;
    auto blob = test::pattern_bytes(3 * kExtent, 41);
    const std::string digest = common::Sha256::hex(blob.data(), blob.size());

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto funnel = std::make_shared<AdmissionFunnel>();
        auto* vec = new test::VectorSource(blob);
        source::LayerStore::Config lsc;
        lsc.funnel = funnel;
        lsc.populate_admit_timeout = milliseconds(500);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest, lsc);

        auto held = co_await funnel->acquire(ReadClass::OnDemand);
        std::atomic<bool> populate_done{false};
        std::atomic<ssize_t> populate_rc{-999};
        elio::go([&]() -> elio::coro::task<void> {
            const ssize_t pr = co_await store->populate(kExtent, kExtent);
            populate_rc.store(pr, std::memory_order_relaxed);
            populate_done.store(true, std::memory_order_release);
        });

        const bool queued = co_await poll_until([&] {
            return funnel->scavenger_waits() == 1 &&
                   !populate_done.load(std::memory_order_acquire);
        });
        REQUIRE(queued);

        std::vector<uint8_t> buf(4096);
        std::atomic<bool> read_done{false};
        std::atomic<ssize_t> read_rc{-999};
        elio::go([&]() -> elio::coro::task<void> {
            const ssize_t r =
                co_await store->pread(buf.data(), buf.size(), kExtent);
            read_rc.store(r, std::memory_order_relaxed);
            read_done.store(true, std::memory_order_release);
        });

        const bool read_before_release = co_await poll_until(
            [&] { return read_done.load(std::memory_order_acquire); }, 100);
        if (!read_before_release) {
            held.reset();
            const bool drained = co_await poll_until([&] {
                return read_done.load(std::memory_order_acquire) &&
                       populate_done.load(std::memory_order_acquire);
            });
            REQUIRE(drained);
        }
        REQUIRE(read_before_release);
        REQUIRE(read_rc.load(std::memory_order_relaxed) ==
                static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == slice(blob, kExtent, buf.size()));
        REQUIRE(vec->reads() == 1);

        const bool populate_timed_out = co_await poll_until(
            [&] { return populate_done.load(std::memory_order_acquire); },
            2000);
        held.reset();
        if (!populate_timed_out) {
            const bool drained = co_await poll_until([&] {
                return populate_done.load(std::memory_order_acquire);
            });
            REQUIRE(drained);
        }
        REQUIRE(populate_timed_out);
        REQUIRE(populate_rc.load(std::memory_order_relaxed) == -EAGAIN);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: on-demand bypasses queued prefetch with unbounded populate wait",
          "[source]") {
    // Issue #22's default-timeout shape: an unbounded Prefetch waiter must
    // not make a later same-extent OnDemand read wait for an unrelated gate
    // holder to release.
    test::TempDir dir;
    constexpr size_t kExtent = 64 * 1024;
    auto blob = test::pattern_bytes(3 * kExtent, 43);
    const std::string digest = common::Sha256::hex(blob.data(), blob.size());

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto funnel = std::make_shared<AdmissionFunnel>();
        auto* vec = new test::VectorSource(blob);
        source::LayerStore::Config lsc;
        lsc.funnel = funnel;  // populate_admit_timeout remains 0: unbounded
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest, lsc);

        auto held = co_await funnel->acquire(ReadClass::OnDemand);
        std::atomic<bool> populate_done{false};
        std::atomic<ssize_t> populate_rc{-999};
        elio::go([&]() -> elio::coro::task<void> {
            const ssize_t pr = co_await store->populate(kExtent, kExtent);
            populate_rc.store(pr, std::memory_order_relaxed);
            populate_done.store(true, std::memory_order_release);
        });

        const bool queued = co_await poll_until([&] {
            return funnel->scavenger_waits() == 1 &&
                   !populate_done.load(std::memory_order_acquire);
        });
        REQUIRE(queued);

        std::vector<uint8_t> buf(4096);
        std::atomic<bool> read_done{false};
        std::atomic<ssize_t> read_rc{-999};
        elio::go([&]() -> elio::coro::task<void> {
            const ssize_t r =
                co_await store->pread(buf.data(), buf.size(), kExtent);
            read_rc.store(r, std::memory_order_relaxed);
            read_done.store(true, std::memory_order_release);
        });

        const bool read_before_release = co_await poll_until(
            [&] { return read_done.load(std::memory_order_acquire); }, 100);
        if (!read_before_release) {
            held.reset();
            const bool drained = co_await poll_until([&] {
                return read_done.load(std::memory_order_acquire) &&
                       populate_done.load(std::memory_order_acquire);
            });
            REQUIRE(drained);
        }
        REQUIRE(read_before_release);
        REQUIRE(read_rc.load(std::memory_order_relaxed) ==
                static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == slice(blob, kExtent, buf.size()));
        REQUIRE(vec->reads() == 1);
        REQUIRE(!populate_done.load(std::memory_order_acquire));

        held.reset();
        const bool populated = co_await poll_until(
            [&] { return populate_done.load(std::memory_order_acquire); });
        REQUIRE(populated);
        REQUIRE(populate_rc.load(std::memory_order_relaxed) == 0);
        REQUIRE(vec->reads() == 1);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: admission funnel grows additively on flat latency and halves on rise",
          "[source]") {
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        AdmissionFunnel::Config cfg;
        cfg.window_init = 2;
        cfg.window_min = 1;
        cfg.window_max = 8;
        AdmissionFunnel funnel(cfg);

        // The first sample only anchors the baseline.
        funnel.note_on_demand_latency(kSample);
        REQUIRE(funnel.window() == 2);
        // Flat samples (100 ms vs the 100 ms baseline): +1 each.
        funnel.note_on_demand_latency(kSample);
        funnel.note_on_demand_latency(kSample);
        funnel.note_on_demand_latency(kSample);
        REQUIRE(funnel.window() == 5);
        // A rise (400 ms > 150% of the ~100 ms baseline): halved.
        funnel.note_on_demand_latency(milliseconds(400));
        REQUIRE(funnel.window() == 2);
        // The baseline drifted toward the rise (EMA alpha 1/8):
        // (7*100+400)/8 = 137.5 ms, so 140 ms reads as flat again.
        funnel.note_on_demand_latency(milliseconds(140));
        REQUIRE(funnel.window() == 3);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: admission funnel respects window floor and ceiling",
          "[source]") {
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        AdmissionFunnel::Config cfg;
        cfg.window_init = 2;
        cfg.window_min = 2;
        cfg.window_max = 4;
        AdmissionFunnel funnel(cfg);

        funnel.note_on_demand_latency(kSample);  // baseline anchor
        for (int i = 0; i < 10; ++i) {
            funnel.note_on_demand_latency(kSample);
        }
        REQUIRE(funnel.window() == 4);  // ceiling holds
        // Samples that keep outrunning the EMA baseline (x10 each) read
        // as rises forever, driving the window to the floor — not below.
        for (int i = 0; i < 10; ++i) {
            funnel.note_on_demand_latency(milliseconds(1000) * (1 << i));
        }
        REQUIRE(funnel.window() == 2);  // floor holds
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: admission funnel admits prefetch before fill",
          "[source]") {
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        AdmissionFunnel::Config cfg;
        cfg.window_init = 1;  // room for exactly one scavenger
        cfg.window_min = 1;
        cfg.window_max = 1;
        AdmissionFunnel funnel(cfg);

        auto od = co_await funnel.acquire(ReadClass::OnDemand);
        std::atomic<bool> prefetch_done{false};
        std::atomic<bool> fill_done{false};
        std::optional<AdmissionFunnel::Permit> prefetch_permit;
        elio::go([&]() -> elio::coro::task<void> {
            prefetch_permit.emplace(
                co_await funnel.acquire(ReadClass::Prefetch));
            prefetch_done.store(true, std::memory_order_relaxed);
        });
        // Queue prefetch first, then fill (bounded sleeps order the
        // two queue insertions deterministically).
        co_await elio::time::sleep_for(milliseconds(20));
        elio::go([&]() -> elio::coro::task<void> {
            auto f = co_await funnel.acquire(ReadClass::Fill);
            fill_done.store(true, std::memory_order_relaxed);
        });
        co_await elio::time::sleep_for(milliseconds(20));
        REQUIRE(!prefetch_done.load());
        REQUIRE(!fill_done.load());

        // One free slot: the queued Prefetch wins it over Fill.
        od.reset();
        co_await elio::time::sleep_for(milliseconds(50));
        REQUIRE(prefetch_done.load());
        REQUIRE(!fill_done.load());
        // Releasing the prefetch permit admits the fill.
        prefetch_permit.reset();
        const bool filled = co_await poll_until(
            [&] { return fill_done.load(); });
        REQUIRE(filled);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: admission funnel caps scavenger request size",
          "[source]") {
    AdmissionFunnel funnel;  // default cap: 1 MiB
    constexpr size_t kMiB = 1024 * 1024;
    // On-demand is uncapped (bounded upstream by the registry client's
    // max_response_size and the extent granularity).
    REQUIRE(funnel.cap_request(ReadClass::OnDemand, 4 * kMiB) == 4 * kMiB);
    REQUIRE(funnel.cap_request(ReadClass::Prefetch, 4 * kMiB) == kMiB);
    REQUIRE(funnel.cap_request(ReadClass::Fill, 4 * kMiB) == kMiB);
    REQUIRE(funnel.cap_request(ReadClass::Fill, 512 * 1024) == 512 * 1024);

    AdmissionFunnel::Config cfg;
    cfg.scavenger_size_cap = 256 * 1024;
    AdmissionFunnel custom(cfg);
    REQUIRE(custom.cap_request(ReadClass::Prefetch, kMiB) == 256 * 1024);
}

TEST_CASE("source: admission source splits populate at the scavenger size cap",
          "[source]") {
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto funnel = std::make_shared<AdmissionFunnel>();
        auto* rec = new RecordingSource();
        source::AdmissionSource src(source::BlobSourcePtr(rec), funnel);

        constexpr size_t kMiB = 1024 * 1024;
        const ssize_t r = co_await src.populate(512, 2 * kMiB + 512 * 1024);
        REQUIRE(r == 0);
        // 2.5 MiB split at the 1 MiB cap: 1 MiB + 1 MiB + 512 KiB,
        // forwarded at successive offsets.
        REQUIRE(rec->calls.size() == 3);
        REQUIRE(rec->calls[0].first == 512);
        REQUIRE(rec->calls[0].second == kMiB);
        REQUIRE(rec->calls[1].first == 512 + kMiB);
        REQUIRE(rec->calls[1].second == kMiB);
        REQUIRE(rec->calls[2].first == 512 + 2 * kMiB);
        REQUIRE(rec->calls[2].second == 512 * 1024);
        // populate rides the Prefetch class: scavenger admissions.
        REQUIRE(funnel->scavenger_admissions() == 3);
        REQUIRE(funnel->on_demand_admissions() == 0);

        // pread is OnDemand.
        uint8_t b = 0;
        const ssize_t pr = co_await src.pread(&b, 1, 0);
        REQUIRE(pr == 0);
        REQUIRE(funnel->on_demand_admissions() == 1);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: layer store populate waits at the admission funnel while reads pass",
          "[source]") {
    test::TempDir dir;
    constexpr size_t kExtent = 64 * 1024;
    auto blob = test::pattern_bytes(2 * kExtent, 23);
    const std::string digest = common::Sha256::hex(blob.data(), blob.size());

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto funnel = std::make_shared<AdmissionFunnel>();  // window 2
        // Occupy the whole window with external scavenger permits.
        auto held1 = co_await funnel->acquire(ReadClass::Prefetch);
        auto held2 = co_await funnel->acquire(ReadClass::Fill);

        auto* vec = new test::VectorSource(blob);
        source::LayerStore::Config lsc;
        lsc.funnel = funnel;
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest, lsc);

        // An on-demand miss is admitted unconditionally (past the full
        // window) and returns correct bytes.
        std::vector<uint8_t> buf(4096);
        const ssize_t r = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == test::pattern_bytes(4096, 23));

        // A populate (Prefetch) queues behind the full window.
        std::atomic<bool> populated{false};
        std::atomic<ssize_t> populate_rc{-1};
        elio::go([&]() -> elio::coro::task<void> {
            const ssize_t pr = co_await store->populate(kExtent, kExtent);
            populate_rc.store(pr, std::memory_order_relaxed);
            populated.store(true, std::memory_order_relaxed);
        });
        co_await elio::time::sleep_for(milliseconds(50));
        REQUIRE(!populated.load());
        REQUIRE(funnel->scavenger_waits() == 1);

        // Freeing a window slot lets the warm-up fetch proceed.
        held1.reset();
        const bool done = co_await poll_until(
            [&] { return populated.load(); });
        REQUIRE(done);
        REQUIRE(populate_rc.load() == 0);
        REQUIRE(vec->reads() == 2);  // one miss + one warm fetch
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: admission funnel re-checks the gate when queueing a scavenger",
          "[source]") {
    // Regression: the scavenger slow path once had TWO critical sections
    // (gate check, then waiter push) with wakeups firing only from
    // release paths. A release landing in the gap — gate opened against
    // empty queues — left the about-to-be-queued waiter asleep forever
    // (lost wakeup). The gap is injected deterministically with the
    // test hook: inside it, the slot is freed with no queued waiters.
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        AdmissionFunnel::Config cfg;
        cfg.window_init = 1;  // room for exactly one request
        cfg.window_min = 1;
        cfg.window_max = 1;
        AdmissionFunnel funnel(cfg);

        auto held = co_await funnel.acquire(ReadClass::Fill);  // slot taken
        std::atomic<bool> hook_fired{false};
        funnel.set_gap_hook_for_test([&] {
            hook_fired.store(true, std::memory_order_relaxed);
            held.reset();  // gate opens against EMPTY queues
        });

        // This acquire takes the slow path (window full), the hook frees
        // the slot in the gap, and the push+re-admit must still admit.
        std::atomic<bool> admitted{false};
        std::optional<AdmissionFunnel::Permit> child_permit;
        elio::go([&]() -> elio::coro::task<void> {
            child_permit.emplace(
                co_await funnel.acquire(ReadClass::Prefetch));
            admitted.store(true, std::memory_order_relaxed);
        });
        const bool done = co_await poll_until(
            [&] { return admitted.load(); }, 2000);
        REQUIRE(done);  // without the re-check this never fires
        REQUIRE(hook_fired.load());
        REQUIRE(funnel.inflight_total() == 1);  // the permit holds the slot
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: layer store fill frees the funnel window before throttling",
          "[source]") {
    // Regression: fill's Fill-class permit once lived for the whole loop
    // iteration — including the write-behind back-pressure and the
    // max_mbps 1 s throttle sleep — so a queued Prefetch (documented to
    // strictly outrank Fill) waited behind fill-local delays. The permit
    // must cover the remote fetch alone. Deterministic shape: window 1,
    // the fill's first coalesced fetch held open at the source gate,
    // then a populate queues behind it; after the gate opens the
    // populate must complete long before fill's 1 s throttle sleep ends.
    test::TempDir dir;
    constexpr size_t kExtent = 64 * 1024;
    auto blob = test::pattern_bytes(32 * kExtent, 29);  // 2 MiB, 32 extents
    const std::string digest = common::Sha256::hex(blob.data(), blob.size());

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        AdmissionFunnel::Config fcfg;
        fcfg.window_init = 1;
        fcfg.window_min = 1;
        fcfg.window_max = 1;
        auto funnel = std::make_shared<AdmissionFunnel>(fcfg);

        auto* gated = new GatedSource(blob);
        source::LayerStore::Config lsc;
        lsc.funnel = funnel;
        lsc.fill.enable = true;
        lsc.fill.delay_sec = 0;
        lsc.fill.delay_extra_sec = 0;
        lsc.fill.max_mbps = 1;          // 1 MiB fetch -> 1 s throttle sleep
        lsc.fill.block_size = 1024 * 1024;
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(gated), dir.str(), digest, lsc);

        // The fill admits its first 1 MiB coalesced fetch (extents
        // 0-15) and parks at the closed gate, holding the only slot.
        const bool filling = co_await poll_until(
            [&] { return funnel->inflight_total() == 1; });
        REQUIRE(filling);

        // A populate of a later extent is a Prefetch: it queues behind
        // the full window.
        std::atomic<bool> populated{false};
        elio::go([&]() -> elio::coro::task<void> {
            const ssize_t pr =
                co_await store->populate(20 * kExtent, kExtent);
            if (pr == 0) populated.store(true, std::memory_order_relaxed);
        });
        co_await elio::time::sleep_for(milliseconds(50));
        REQUIRE(!populated.load());
        REQUIRE(funnel->scavenger_waits() == 1);

        // Open the gate: fill's fetch completes, its slot is released
        // IMMEDIATELY (not after the ~1 s throttle sleep), and the
        // queued Prefetch takes it. 700 ms << the 1 s throttle sleep.
        gated->gate.set();
        const bool done = co_await poll_until(
            [&] { return populated.load(); }, 700);
        REQUIRE(done);
        REQUIRE(funnel->scavenger_admissions() == 2);

        co_await store->park_fill(std::chrono::milliseconds(500));
        const auto fill_status = store->fill_status();
        REQUIRE((fill_status == source::LayerStore::FillStatus::kDone ||
                 fill_status == source::LayerStore::FillStatus::kStopped));
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: populate joining an in-flight fetch bypasses the funnel",
          "[source]") {
    // Pins the ADR-0012 dedup ordering: the in-flight join (under the
    // LayerStore's extent map) happens BEFORE the starter's funnel
    // acquire, so a populate for an extent already being fetched joins
    // that fetch and never consumes a scavenger admission — whatever
    // class started the fetch.
    test::TempDir dir;
    constexpr size_t kExtent = 64 * 1024;
    auto blob = test::pattern_bytes(4 * kExtent, 31);
    const std::string digest = common::Sha256::hex(blob.data(), blob.size());

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto funnel = std::make_shared<AdmissionFunnel>();
        auto* gated = new GatedSource(blob);  // gate closed
        source::LayerStore::Config lsc;
        lsc.funnel = funnel;
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(gated), dir.str(), digest, lsc);

        // An on-demand miss starts the fetch of extent 1 and parks at
        // the closed gate.
        std::vector<uint8_t> buf(4096);
        std::atomic<ssize_t> read_rc{-1};
        elio::go([&]() -> elio::coro::task<void> {
            const ssize_t r = co_await store->pread(
                buf.data(), buf.size(), kExtent);
            read_rc.store(r, std::memory_order_relaxed);
        });
        const bool fetching = co_await poll_until(
            [&] { return store->remote_fetches() == 1; });
        REQUIRE(fetching);
        REQUIRE(funnel->on_demand_admissions() == 1);

        // A populate of the SAME extent joins the in-flight fetch.
        std::atomic<bool> populated{false};
        elio::go([&]() -> elio::coro::task<void> {
            const ssize_t pr = co_await store->populate(kExtent, kExtent);
            if (pr == 0) populated.store(true, std::memory_order_relaxed);
        });
        const bool joined = co_await poll_until(
            [&] { return store->coalesced_joins() == 1; });
        REQUIRE(joined);
        // The join never reached the funnel: no scavenger admission,
        // no queue event — and the fetch itself is still the single
        // on-demand one.
        REQUIRE(funnel->scavenger_admissions() == 0);
        REQUIRE(funnel->scavenger_waits() == 0);

        gated->gate.set();
        const bool done = co_await poll_until(
            [&] { return populated.load() && read_rc.load() >= 0; });
        REQUIRE(done);
        REQUIRE(read_rc.load() == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == slice(blob, kExtent, buf.size()));
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: layer store fetch frees the funnel slot before retiring the fetch",
          "[source]") {
    // Regression: the starter's funnel permit once stayed alive through
    // the completion bookkeeping (result assignment, the suspending
    // inflight_mu_ lock, the in-flight map retire) instead of covering
    // exactly the remote fetch — inflating the OnDemand AIMD sample and
    // holding a scavenger slot past the fetch. The fetch-done hook
    // observes the slot count at the precise moment the fetch has
    // completed but the in-flight entry is not yet retired.
    test::TempDir dir;
    constexpr size_t kExtent = 64 * 1024;
    auto blob = test::pattern_bytes(4 * kExtent, 37);
    const std::string digest = common::Sha256::hex(blob.data(), blob.size());

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto funnel = std::make_shared<AdmissionFunnel>();
        auto* gated = new GatedSource(blob);  // gate closed
        source::LayerStore::Config lsc;
        lsc.funnel = funnel;
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(gated), dir.str(), digest, lsc);

        std::atomic<int64_t> slot_at_completion{-1};
        store->set_test_fetch_done_hook([&] {
            slot_at_completion.store(
                static_cast<int64_t>(funnel->inflight_total()),
                std::memory_order_relaxed);
        });

        std::vector<uint8_t> buf(4096);
        std::atomic<ssize_t> read_rc{-1};
        elio::go([&]() -> elio::coro::task<void> {
            const ssize_t r = co_await store->pread(
                buf.data(), buf.size(), 2 * kExtent);
            read_rc.store(r, std::memory_order_relaxed);
        });
        const bool fetching = co_await poll_until(
            [&] { return store->remote_fetches() == 1; });
        REQUIRE(fetching);
        // The fetch is parked at the source gate, holding its slot.
        REQUIRE(funnel->inflight_total() == 1);

        gated->gate.set();
        const bool done = co_await poll_until(
            [&] { return read_rc.load() >= 0; });
        REQUIRE(done);
        REQUIRE(read_rc.load() == static_cast<ssize_t>(buf.size()));
        // The hook ran after the fetch completed and BEFORE the
        // completion protocol — the permit must already be released
        // (0, not the 1 a fetch-scoped permit would still show).
        REQUIRE(slot_at_completion.load() == 0);
        REQUIRE(buf == slice(blob, 2 * kExtent, buf.size()));
        co_return 0;
    });
    REQUIRE(rc == 0);
}
