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
#include <elio/time/timer.hpp>

#include <atomic>
#include <chrono>
#include <vector>

using namespace obd;
using std::chrono::milliseconds;

namespace {

using source::AdmissionFunnel;
using source::ReadClass;

constexpr milliseconds kSample{100};

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
