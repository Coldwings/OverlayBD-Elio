// Unit tests: LayerStore sparse-file persistence (ADR-0011).
//
// The fixtures are deterministic patterned bytes generated in-test; the
// expected sha256 digests come from the project's OpenSSL-backed helper
// (the same primitive upstream overlaybd verifies against), not from any
// writer under test.
//
// Convention: every co_await result is assigned to a local BEFORE the
// REQUIRE (the same style as the other unit tests) — a co_await inside
// Catch2's REQUIRE decomposition macro is not a single evaluation.
#include "source/layer_store.hpp"

#include "common/bytes.hpp"
#include "common/errors.hpp"
#include "common/sha256.hpp"

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <elio/runtime/spawn.hpp>
#include <elio/time/timer.hpp>

#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace obd;
using obd::test::VectorSource;

namespace {

constexpr size_t kExtent = 64 * 1024;

/// Polls `pred` with 1 ms coroutine sleeps; bounded, no fixed sleeps.
template <typename Pred>
elio::coro::task<bool> poll_until(Pred&& pred, int max_iters = 20000) {
    for (int i = 0; i < max_iters; ++i) {
        if (pred()) co_return true;
        co_await elio::time::sleep_for(std::chrono::milliseconds(1));
    }
    co_return pred();
}

/// Lists dir entries whose name starts with `prefix` (plain test-thread IO).
std::vector<std::string> names_with_prefix(const std::string& dir,
                                           const std::string& prefix) {
    std::vector<std::string> out;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.compare(0, prefix.size(), prefix) == 0) {
            out.push_back(entry.path().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// Overwrites bytes in an existing file (plain test-thread IO).
void overwrite_file(const std::string& path, uint64_t offset,
                    const std::vector<uint8_t>& data) {
    const int fd = ::open(path.c_str(), O_WRONLY);
    REQUIRE(fd >= 0);
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t w = ::pwrite(fd, data.data() + done, data.size() - done,
                                   static_cast<off_t>(offset + done));
        REQUIRE(w > 0);
        done += static_cast<size_t>(w);
    }
    ::close(fd);
}

std::string digest_of(const std::vector<uint8_t>& blob) {
    return common::Sha256::hex(blob.data(), blob.size());
}

std::vector<uint8_t> slice(const std::vector<uint8_t>& v, size_t off,
                           size_t len) {
    return {v.begin() + static_cast<ptrdiff_t>(off),
            v.begin() + static_cast<ptrdiff_t>(off + len)};
}

/// Writes one 8-byte sidecar record {crc32, flags} for `extent_id`
/// (plain test-thread IO; the on-disk layout documented in layer_store.hpp).
void write_sidecar_record(const std::string& sidecar_path, uint64_t extent_id,
                          uint32_t crc, uint32_t flags) {
    std::vector<uint8_t> rec(8);
    bytes::store_u32_le(rec.data(), crc);
    bytes::store_u32_le(rec.data() + 4, flags);
    overwrite_file(sidecar_path, 80 + extent_id * 8, rec);
}

uint32_t crc32_of(const std::vector<uint8_t>& v) {
    return static_cast<uint32_t>(::crc32(
        0L, reinterpret_cast<const Bytef*>(v.data()),
        static_cast<uInt>(v.size())));
}

/// A source whose reads block until `gate` is set, delegating to a
/// VectorSource afterwards — deterministic in-flight coalescing tests.
class GatedSource final : public source::BlobSource {
public:
    explicit GatedSource(std::vector<uint8_t> data)
        : inner_(std::move(data)) {}

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override {
        co_await gate.wait();
        co_return co_await inner_.pread(buf, count, offset);
    }

    uint64_t size() const noexcept override { return inner_.size(); }
    std::string_view label() const noexcept override {
        return inner_.label();
    }
    uint64_t reads() const { return inner_.reads(); }

    elio::sync::event gate;

private:
    VectorSource inner_;
};

}  // namespace

TEST_CASE("source: layer store cold read persists and reopen serves locally",
          "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(4 * kExtent, 7);
    const std::string digest = digest_of(blob);

    int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        REQUIRE(store->state() == source::LayerStore::State::Filling);
        REQUIRE(store->extents_total() == 4);
        std::vector<uint8_t> buf(kExtent);
        const ssize_t r1 = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r1 == static_cast<ssize_t>(kExtent));
        REQUIRE(buf == slice(blob, 0, kExtent));
        const ssize_t r2 =
            co_await store->pread(buf.data(), buf.size(), kExtent);
        REQUIRE(r2 == static_cast<ssize_t>(kExtent));
        REQUIRE(vec->reads() == 2);
        const bool warm = co_await poll_until(
            [&] { return store->extents_present() == 2; });
        REQUIRE(warm);
        co_return 0;  // store destroyed here, scheduler still running
    });
    REQUIRE(rc == 0);

    // Reopen against the same dir with a fresh remote: the two persisted
    // extents must be served locally with zero remote reads.
    rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        REQUIRE(store->state() == source::LayerStore::State::Filling);
        REQUIRE(store->extents_present() == 2);
        std::vector<uint8_t> buf(kExtent);
        const ssize_t r1 = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r1 == static_cast<ssize_t>(kExtent));
        REQUIRE(buf == slice(blob, 0, kExtent));
        const ssize_t r2 =
            co_await store->pread(buf.data(), buf.size(), kExtent);
        REQUIRE(r2 == static_cast<ssize_t>(kExtent));
        REQUIRE(buf == slice(blob, kExtent, kExtent));
        REQUIRE(vec->reads() == 0);  // all local
        // A cold extent still fetches from the remote.
        const ssize_t r3 =
            co_await store->pread(buf.data(), buf.size(), 2 * kExtent);
        REQUIRE(r3 == static_cast<ssize_t>(kExtent));
        REQUIRE(vec->reads() == 1);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: layer store detects corrupted staging via crc and refetches",
          "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(2 * kExtent, 8);
    const std::string digest = digest_of(blob);

    int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        std::vector<uint8_t> buf(kExtent);
        const ssize_t r = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(kExtent));
        const bool warm = co_await poll_until(
            [&] { return store->extents_present() == 1; });
        REQUIRE(warm);
        co_return 0;
    });
    REQUIRE(rc == 0);

    // Corrupt extent 0 inside the staging file between runs.
    const auto stagings = names_with_prefix(dir.str(), ".download.");
    REQUIRE(stagings.size() == 1);
    overwrite_file(stagings.front(), 0, std::vector<uint8_t>(kExtent, 0xFF));

    rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        REQUIRE(store->extents_present() == 1);  // sidecar still says present
        std::vector<uint8_t> buf(kExtent);
        const ssize_t r = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(kExtent));
        REQUIRE(buf == slice(blob, 0, kExtent));  // re-fetched, correct
        REQUIRE(vec->reads() == 1);
        REQUIRE(store->crc_failures() == 1);
        // The demoted extent is re-persisted with the good bytes.
        const bool warm = co_await poll_until(
            [&] { return store->extents_present() == 1; });
        REQUIRE(warm);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: layer store deletes stale sidecar and restarts fresh",
          "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(2 * kExtent, 9);
    const std::string digest = digest_of(blob);

    int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        std::vector<uint8_t> buf(kExtent);
        const ssize_t r = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(kExtent));
        const bool warm = co_await poll_until(
            [&] { return store->extents_present() == 1; });
        REQUIRE(warm);
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(names_with_prefix(dir.str(), ".download.").size() == 1);
    REQUIRE(names_with_prefix(dir.str(), ".bitmap.").size() == 1);

    // Tamper A: rename the sidecar to a wrong nonce name -> both unpaired.
    {
        const auto sidecars = names_with_prefix(dir.str(), ".bitmap.");
        std::filesystem::rename(sidecars.front(),
                                dir.str() + "/.bitmap.0123456789abcdef");
    }
    rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        REQUIRE(store->extents_present() == 0);  // fresh start
        std::vector<uint8_t> buf(kExtent);
        const ssize_t r = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(kExtent));
        REQUIRE(vec->reads() == 1);  // re-fetched remotely
        const bool warm = co_await poll_until(
            [&] { return store->extents_present() == 1; });
        REQUIRE(warm);
        co_return 0;
    });
    REQUIRE(rc == 0);
    // The unpaired staging file and renamed sidecar were deleted; exactly
    // one fresh pair exists now.
    REQUIRE(names_with_prefix(dir.str(), ".download.").size() == 1);
    REQUIRE(names_with_prefix(dir.str(), ".bitmap.").size() == 1);
    REQUIRE(!std::filesystem::exists(dir.str() +
                                     "/.bitmap.0123456789abcdef"));

    // Tamper B: clobber the sidecar magic -> header invalid.
    {
        const auto sidecars = names_with_prefix(dir.str(), ".bitmap.");
        REQUIRE(sidecars.size() == 1);
        overwrite_file(sidecars.front(), 0, {'B', 'A', 'D', 'M',
                                             'A', 'G', 'I', 'C'});
    }
    rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        REQUIRE(store->extents_present() == 0);  // fresh again
        std::vector<uint8_t> buf(kExtent);
        const ssize_t r = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(kExtent));
        REQUIRE(vec->reads() == 1);
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(names_with_prefix(dir.str(), ".download.").size() == 1);
    REQUIRE(names_with_prefix(dir.str(), ".bitmap.").size() == 1);
}

TEST_CASE("source: layer store drops writes when the queue is full",
          "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(8 * kExtent, 10);
    const std::string digest = digest_of(blob);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        source::LayerStore::Config cfg;
        cfg.queue_max_bytes = kExtent;  // room for exactly one queued extent
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest, cfg);

        // Stall the writer inside the first job until released. Bounded so
        // a failing assertion can never hang teardown (the destructor joins
        // the writer thread).
        std::atomic<bool> first{true};
        std::atomic<bool> entered{false};
        std::atomic<bool> release{false};
        store->set_test_write_hook([&](uint64_t) -> int {
            if (!first.exchange(false)) return 0;
            entered.store(true);
            for (int i = 0; i < 30000 && !release.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return 0;
        });

        std::vector<uint8_t> buf(kExtent);
        const ssize_t r0 = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r0 == static_cast<ssize_t>(kExtent));
        const bool blocked = co_await poll_until(
            [&] { return entered.load(); });
        REQUIRE(blocked);
        // Writer holds job 0; extent 1 fits the queue; 2..5 must drop.
        for (uint64_t e = 1; e <= 5; ++e) {
            const ssize_t r =
                co_await store->pread(buf.data(), buf.size(), e * kExtent);
            REQUIRE(r == static_cast<ssize_t>(kExtent));
            REQUIRE(buf == slice(blob, e * kExtent, kExtent));
        }
        const bool dropped = co_await poll_until(
            [&] { return store->dropped_writes() == 4; });
        REQUIRE(dropped);
        release.store(true);
        const bool warm = co_await poll_until(
            [&] { return store->extents_present() == 2; });
        REQUIRE(warm);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: layer store enters bypass on write failure", "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(2 * kExtent, 11);
    const std::string digest = digest_of(blob);

    int injected = 0;
    SECTION("enospc") { injected = ENOSPC; }
    SECTION("eio") { injected = EIO; }

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        store->set_test_write_hook(
            [&injected](uint64_t) -> int { return injected; });

        std::vector<uint8_t> buf(kExtent);
        const ssize_t r0 = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r0 == static_cast<ssize_t>(kExtent));
        const bool bypassed = co_await poll_until([&] {
            return store->state() == source::LayerStore::State::Bypass;
        });
        REQUIRE(bypassed);
        // The writer must not persist anything after bypass, even if the
        // "disk" recovers (hook cleared): reads keep working remotely and
        // nothing is enqueued.
        store->set_test_write_hook([](uint64_t) -> int { return 0; });
        const ssize_t r =
            co_await store->pread(buf.data(), buf.size(), kExtent);
        REQUIRE(r == static_cast<ssize_t>(kExtent));
        REQUIRE(buf == slice(blob, kExtent, kExtent));
        REQUIRE(vec->reads() == 2);
        // populate is a no-op in bypass.
        const ssize_t p = co_await store->populate(0, kExtent);
        REQUIRE(p == 0);
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));
        REQUIRE(store->state() == source::LayerStore::State::Bypass);
        REQUIRE(store->extents_present() == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: layer store stays filling after a non-fatal write error",
          "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(2 * kExtent, 17);
    const std::string digest = digest_of(blob);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        // Extent 0 writes always fail with a non-ENOSPC/EIO errno: the
        // entry is dropped with a warning but the store keeps filling.
        store->set_test_write_hook(
            [](uint64_t eid) -> int { return eid == 0 ? EACCES : 0; });

        std::vector<uint8_t> buf(kExtent);
        const ssize_t r0 = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r0 == static_cast<ssize_t>(kExtent));
        const ssize_t r1 =
            co_await store->pread(buf.data(), buf.size(), kExtent);
        REQUIRE(r1 == static_cast<ssize_t>(kExtent));
        // Extent 1 persists; extent 0's entry is dropped; state stays.
        const bool warm = co_await poll_until(
            [&] { return store->extents_present() == 1; });
        REQUIRE(warm);
        co_await elio::time::sleep_for(std::chrono::milliseconds(20));
        REQUIRE(store->state() == source::LayerStore::State::Filling);
        REQUIRE(store->extents_present() == 1);
        // Extent 0 is still a hole: reading it re-fetches remotely.
        const ssize_t r2 = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r2 == static_cast<ssize_t>(kExtent));
        REQUIRE(buf == slice(blob, 0, kExtent));
        REQUIRE(vec->reads() == 3);
        REQUIRE(store->state() == source::LayerStore::State::Filling);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: layer store completes to overlaybd.commit and reopens read-only",
          "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(3 * kExtent + 100, 12);
    const std::string digest = digest_of(blob);

    int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        const ssize_t p = co_await store->populate(0, blob.size());
        REQUIRE(p == 0);
        const bool done = co_await poll_until([&] {
            return store->state() == source::LayerStore::State::Complete;
        });
        REQUIRE(done);
        std::vector<uint8_t> buf(blob.size());
        const ssize_t r = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(blob.size()));
        REQUIRE(buf == blob);
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(std::filesystem::exists(dir.str() + "/overlaybd.commit"));
    REQUIRE(std::filesystem::file_size(dir.str() + "/overlaybd.commit") ==
            blob.size());
    REQUIRE(names_with_prefix(dir.str(), ".download.").empty());
    REQUIRE(names_with_prefix(dir.str(), ".bitmap.").empty());

    // Reopen: binds the commit file read-only, zero remote reads.
    rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        REQUIRE(store->state() == source::LayerStore::State::Complete);
        std::vector<uint8_t> buf(4096);
        const ssize_t r = co_await store->pread(buf.data(), buf.size(), 77);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == slice(blob, 77, buf.size()));
        REQUIRE(vec->reads() == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: layer store restarts on sha mismatch within try count",
          "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(2 * kExtent, 13);
    const std::string wrong_digest =
        digest_of(test::pattern_bytes(2 * kExtent, 99));

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        source::LayerStore::Config cfg;
        cfg.try_count = 2;
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), wrong_digest, cfg);

        const auto first_name = [&] {
            const auto v = names_with_prefix(dir.str(), ".download.");
            return v.empty() ? std::string() : v.front();
        };
        const std::string nonce1 = first_name();
        REQUIRE(!nonce1.empty());

        // Attempt 1 fills, verification fails, a fresh pair (new nonce)
        // appears. (The poll must wait for a non-empty new name: there is
        // an unlink/create gap during the restart.)
        const ssize_t p1 = co_await store->populate(0, blob.size());
        REQUIRE(p1 == 0);
        const bool restarted = co_await poll_until([&] {
            const std::string n = first_name();
            return !n.empty() && n != nonce1;
        });
        REQUIRE(restarted);
        REQUIRE(store->state() == source::LayerStore::State::Filling);
        const std::string nonce2 = first_name();
        REQUIRE(nonce2 != nonce1);

        // Attempt 2 exhausts try_count: bypass, no third restart.
        const ssize_t p2 = co_await store->populate(0, blob.size());
        REQUIRE(p2 == 0);
        const bool bypassed = co_await poll_until([&] {
            return store->state() == source::LayerStore::State::Bypass;
        });
        REQUIRE(bypassed);
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));
        REQUIRE(store->state() == source::LayerStore::State::Bypass);
        REQUIRE(first_name() == nonce2);

        // Reads keep working remotely with correct data.
        std::vector<uint8_t> buf(kExtent);
        const ssize_t r = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(kExtent));
        REQUIRE(buf == slice(blob, 0, kExtent));
        REQUIRE(vec->reads() > 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: layer store populate warms extents without serving data",
          "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(4 * kExtent, 14);
    const std::string digest = digest_of(blob);

    int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        const ssize_t p = co_await store->populate(0, 2 * kExtent);
        REQUIRE(p == 0);
        REQUIRE(vec->reads() == 2);
        const bool warm = co_await poll_until(
            [&] { return store->extents_present() == 2; });
        REQUIRE(warm);
        co_return 0;
    });
    REQUIRE(rc == 0);

    // The warmed extents survive a reopen and are served locally.
    rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        REQUIRE(store->extents_present() == 2);
        std::vector<uint8_t> buf(2 * kExtent);
        const ssize_t r = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == slice(blob, 0, buf.size()));
        REQUIRE(vec->reads() == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: layer store coalesces concurrent fetches of one extent",
          "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(2 * kExtent, 15);
    const std::string digest = digest_of(blob);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* gated = new GatedSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(gated), dir.str(), digest);

        constexpr int kN = 6;
        std::array<std::vector<uint8_t>, kN> bufs;
        std::array<ssize_t, kN> results{};
        std::atomic<int> done{0};
        for (int i = 0; i < kN; ++i) {
            bufs[i].resize(4096);
            elio::go([&store, &bufs, &results, &done,
                      i]() -> elio::coro::task<void> {
                results[i] =
                    co_await store->pread(bufs[i].data(), bufs[i].size(), 128);
                done.fetch_add(1, std::memory_order_relaxed);
            });
        }
        // One starter fetches; the other N-1 join the in-flight fetch.
        const bool joined = co_await poll_until(
            [&] { return store->coalesced_joins() == kN - 1; });
        REQUIRE(joined);
        gated->gate.set();
        const bool finished = co_await poll_until(
            [&] { return done.load() == kN; });
        REQUIRE(finished);
        for (int i = 0; i < kN; ++i) {
            REQUIRE(results[i] == static_cast<ssize_t>(bufs[i].size()));
            REQUIRE(bufs[i] == slice(blob, 128, bufs[i].size()));
        }
        REQUIRE(store->remote_fetches() == 1);
        REQUIRE(gated->reads() == 1);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: layer store handles a tail extent at eof", "[source]") {
    test::TempDir dir;
    // 3 extents; the tail extent is 1000 bytes. Only extents 0 and 2 are
    // persisted in phase 1, so the store stays Filling for the reopen.
    const size_t blob_size = 2 * kExtent + 1000;
    auto blob = test::pattern_bytes(blob_size, 16);
    const std::string digest = digest_of(blob);

    int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        REQUIRE(store->extents_total() == 3);
        std::vector<uint8_t> buf(kExtent);
        // Whole-extent read of extent 0.
        const ssize_t r0 = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r0 == static_cast<ssize_t>(kExtent));
        // Tail extent read (extent 2, 1000 bytes).
        const ssize_t r1 =
            co_await store->pread(buf.data(), 1000, 2 * kExtent);
        REQUIRE(r1 == 1000);
        REQUIRE(slice(buf, 0, 1000) == slice(blob, 2 * kExtent, 1000));
        REQUIRE(vec->reads() == 2);  // one fetch per extent
        // Clamped at EOF.
        const ssize_t r2 =
            co_await store->pread(buf.data(), 1000, 2 * kExtent + 500);
        REQUIRE(r2 == 500);
        // At/after EOF reads return 0.
        const ssize_t eof = co_await store->pread(buf.data(), 1000, blob_size);
        REQUIRE(eof == 0);
        const bool warm = co_await poll_until(
            [&] { return store->extents_present() == 2; });
        REQUIRE(warm);
        REQUIRE(store->state() == source::LayerStore::State::Filling);
        co_return 0;
    });
    REQUIRE(rc == 0);

    // Reopen: the tail extent is served locally and CRC-verified over its
    // actual (short) length; a read crossing into it from the cold middle
    // extent mixes local and remote bytes correctly.
    rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        REQUIRE(store->extents_present() == 2);
        std::vector<uint8_t> buf(1500);
        const ssize_t r =
            co_await store->pread(buf.data(), buf.size(), 2 * kExtent - 500);
        REQUIRE(r == 1500);
        REQUIRE(buf == slice(blob, 2 * kExtent - 500, 1500));
        REQUIRE(vec->reads() == 1);  // only the cold middle extent fetched
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: layer store completes a fully-filled pair on reopen",
          "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(2 * kExtent, 18);
    const std::string digest = digest_of(blob);

    // Phase 1: persist only extent 0, simulating a run that dies after the
    // last extent's data+record but before the completion rename.
    int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        const ssize_t p = co_await store->populate(0, kExtent);
        REQUIRE(p == 0);
        const bool warm = co_await poll_until(
            [&] { return store->extents_present() == 1; });
        REQUIRE(warm);
        co_return 0;
    });
    REQUIRE(rc == 0);

    // Test-thread: persist the last extent (data + sidecar record) exactly
    // as the writer thread would have — no rename afterwards.
    const auto stagings = names_with_prefix(dir.str(), ".download.");
    const auto sidecars = names_with_prefix(dir.str(), ".bitmap.");
    REQUIRE(stagings.size() == 1);
    REQUIRE(sidecars.size() == 1);
    const auto tail = slice(blob, kExtent, kExtent);
    overwrite_file(stagings.front(), kExtent, tail);
    write_sidecar_record(sidecars.front(), 1, crc32_of(tail), 1);

    // Reopen: every record is present, so the store completes immediately
    // (verify + rename) without any further reads.
    rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        REQUIRE(store->extents_present() == 2);
        const bool done = co_await poll_until([&] {
            return store->state() == source::LayerStore::State::Complete;
        });
        REQUIRE(done);
        std::vector<uint8_t> buf(blob.size());
        const ssize_t r = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(blob.size()));
        REQUIRE(buf == blob);
        REQUIRE(vec->reads() == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(std::filesystem::exists(dir.str() + "/overlaybd.commit"));
    REQUIRE(names_with_prefix(dir.str(), ".download.").empty());
    REQUIRE(names_with_prefix(dir.str(), ".bitmap.").empty());
}

TEST_CASE("source: layer store accepts digest forms and rejects malformed",
          "[source]") {
    auto blob = test::pattern_bytes(2 * kExtent, 19);
    const std::string digest = digest_of(blob);

    // The "sha256:" prefix form of the image config is accepted.
    test::TempDir dir1;
    int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir1.str(), "sha256:" + digest);
        const ssize_t p = co_await store->populate(0, blob.size());
        REQUIRE(p == 0);
        const bool done = co_await poll_until([&] {
            return store->state() == source::LayerStore::State::Complete;
        });
        REQUIRE(done);
        co_return 0;
    });
    REQUIRE(rc == 0);

    // Uppercase hex is accepted (normalized before comparison).
    test::TempDir dir2;
    std::string upper = digest;
    for (auto& c : upper) c = static_cast<char>(::toupper(c));
    rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir2.str(), upper);
        const ssize_t p = co_await store->populate(0, blob.size());
        REQUIRE(p == 0);
        const bool done = co_await poll_until([&] {
            return store->state() == source::LayerStore::State::Complete;
        });
        REQUIRE(done);
        co_return 0;
    });
    REQUIRE(rc == 0);

    // Malformed digests fail open with EINVAL.
    for (const std::string& bad : {std::string("not-a-digest"),
                                   std::string("sha256:abcd")}) {
        test::TempDir dir3;
        rc = test::run_coro([&]() -> elio::coro::task<int> {
            try {
                auto store = co_await source::LayerStore::open(
                    std::make_unique<VectorSource>(blob), dir3.str(), bad);
                co_return 0;
            } catch (const error& e) {
                co_return e.errno_value();
            }
        });
        REQUIRE(rc == EINVAL);
    }
}

TEST_CASE("source: layer store completes without verification when digest is empty",
          "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(2 * kExtent, 20);

    // Phase 1: warm extent 0 and destroy — the zero-filled digest in the
    // sidecar header must match on resume.
    int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), "");
        const ssize_t p = co_await store->populate(0, kExtent);
        REQUIRE(p == 0);
        const bool warm = co_await poll_until(
            [&] { return store->extents_present() == 1; });
        REQUIRE(warm);
        co_return 0;
    });
    REQUIRE(rc == 0);

    // Phase 2: resume, fill the rest, complete without verification.
    rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), "");
        REQUIRE(store->extents_present() == 1);  // resumed
        const ssize_t p = co_await store->populate(0, blob.size());
        REQUIRE(p == 0);
        const bool done = co_await poll_until([&] {
            return store->state() == source::LayerStore::State::Complete;
        });
        REQUIRE(done);
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(std::filesystem::exists(dir.str() + "/overlaybd.commit"));

    // Reopen binds the commit; reads are local with zero remote reads.
    rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), "");
        REQUIRE(store->state() == source::LayerStore::State::Complete);
        std::vector<uint8_t> buf(1024);
        const ssize_t r = co_await store->pread(buf.data(), buf.size(), 9);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == slice(blob, 9, buf.size()));
        REQUIRE(vec->reads() == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: layer store completes an empty layer", "[source]") {
    test::TempDir dir;
    const std::vector<uint8_t> blob;
    const std::string digest = digest_of(blob);

    int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto store = co_await source::LayerStore::open(
            std::make_unique<VectorSource>(blob), dir.str(), digest);
        const bool done = co_await poll_until([&] {
            return store->state() == source::LayerStore::State::Complete;
        });
        REQUIRE(done);
        REQUIRE(store->extents_total() == 0);
        REQUIRE(store->extents_present() == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(std::filesystem::exists(dir.str() + "/overlaybd.commit"));
    REQUIRE(std::filesystem::file_size(dir.str() + "/overlaybd.commit") == 0);
}

TEST_CASE("source: layer store fill completes a partially warmed store",
          "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(4 * kExtent, 22);
    const std::string digest = digest_of(blob);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        source::LayerStore::Config cfg;
        cfg.fill.enable = true;
        cfg.fill.delay_sec = 0;
        cfg.fill.delay_extra_sec = 0;
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest, cfg);
        // Warm one extent read-through; fill must warm the rest and drive
        // the store to the sha256-verified commit without further reads.
        std::vector<uint8_t> buf(kExtent);
        const ssize_t r0 = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r0 == static_cast<ssize_t>(kExtent));
        const bool done = co_await poll_until([&] {
            return store->state() == source::LayerStore::State::Complete;
        });
        REQUIRE(done);
        // Completion lands on the writer thread; the fill walk notices a
        // beat later — poll for its terminal status.
        const bool fill_done = co_await poll_until([&] {
            return store->fill_status() ==
                   source::LayerStore::FillStatus::kDone;
        });
        REQUIRE(fill_done);
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(std::filesystem::exists(dir.str() + "/overlaybd.commit"));

    // Reopen binds the commit: byte-exact, zero remote reads.
    const int rc2 = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        REQUIRE(store->state() == source::LayerStore::State::Complete);
        std::vector<uint8_t> buf(blob.size());
        const ssize_t r = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(blob.size()));
        REQUIRE(buf == blob);
        REQUIRE(vec->reads() == 0);
        co_return 0;
    });
    REQUIRE(rc2 == 0);
}

TEST_CASE("source: layer store fill resumes from the sidecar across a restart",
          "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(6 * kExtent, 23);
    const std::string digest = digest_of(blob);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        uint64_t persisted_phase1 = 0;
        {
            auto* vec = new VectorSource(blob);
            source::LayerStore::Config cfg;
            cfg.fill.enable = true;
            cfg.fill.delay_sec = 0;
            cfg.fill.delay_extra_sec = 0;
            cfg.queue_max_bytes = kExtent;  // fill back-pressures quickly
            auto store = co_await source::LayerStore::open(
                source::BlobSourcePtr(vec), dir.str(), digest, cfg);
            // Slow disk: fill stays busy while a few extents persist.
            store->set_test_write_hook([](uint64_t) -> int {
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(15);
                while (std::chrono::steady_clock::now() < deadline) {
                }
                return 0;
            });
            const bool warm = co_await poll_until(
                [&] { return store->extents_present() >= 1; });
            REQUIRE(warm);
            store->stop_fill();
            const bool parked = co_await poll_until([&] {
                return store->fill_status() ==
                       source::LayerStore::FillStatus::kStopped;
            });
            REQUIRE(parked);
            store->set_test_write_hook(nullptr);
            persisted_phase1 = store->extents_present();
            REQUIRE(persisted_phase1 >= 1);
            REQUIRE(persisted_phase1 < 6);  // genuinely mid-fill
        }  // store destroyed with the fill parked (lifetime contract)

        // Reopen with fill: the persisted extents resume from the sidecar
        // (not refetched); fill finishes the rest and completes.
        auto* vec2 = new VectorSource(blob);
        source::LayerStore::Config cfg2;
        cfg2.fill.enable = true;
        cfg2.fill.delay_sec = 0;
        cfg2.fill.delay_extra_sec = 0;
        auto store2 = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec2), dir.str(), digest, cfg2);
        // The sidecar resumes at least what phase 1 persisted (the writer
        // may legitimately have landed one more extent before teardown).
        REQUIRE(store2->extents_present() >= persisted_phase1);
        const bool done = co_await poll_until([&] {
            return store2->state() == source::LayerStore::State::Complete;
        });
        REQUIRE(done);
        // Resumed extents were not refetched: fill's coalesced runs touch
        // at most the extents phase 1 never persisted.
        REQUIRE(vec2->reads() <= 6 - persisted_phase1);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: layer store fill honors the throughput throttle",
          "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(3 * 1024 * 1024, 24);
    const std::string digest = digest_of(blob);

    const auto t0 = std::chrono::steady_clock::now();
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        source::LayerStore::Config cfg;
        cfg.fill.enable = true;
        cfg.fill.delay_sec = 0;
        cfg.fill.delay_extra_sec = 0;
        cfg.fill.max_mbps = 1;  // 1 MiB/s: 3 MiB takes >= 2 window waits
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest, cfg);
        const bool done = co_await poll_until(
            [&] {
                return store->state() == source::LayerStore::State::Complete;
            },
            60000);
        REQUIRE(done);
        co_return 0;
    });
    REQUIRE(rc == 0);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    // Unthrottled this completes in milliseconds; the 1 MiB/s budget
    // forces ~2s. 1.5s leaves generous margin on slow machines.
    REQUIRE(elapsed >= std::chrono::milliseconds(1500));
}

TEST_CASE("source: layer store fill stays off in bypass", "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(4 * kExtent, 25);
    const std::string digest = digest_of(blob);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        // Gated remote: the hook is installed before any fill traffic.
        auto* gated = new GatedSource(blob);
        source::LayerStore::Config cfg;
        cfg.fill.enable = true;
        cfg.fill.delay_sec = 0;
        cfg.fill.delay_extra_sec = 0;
        cfg.queue_max_bytes = kExtent;
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(gated), dir.str(), digest, cfg);
        store->set_test_write_hook([](uint64_t) -> int { return ENOSPC; });
        gated->gate.set();

        const bool bypassed = co_await poll_until([&] {
            return store->state() == source::LayerStore::State::Bypass;
        });
        REQUIRE(bypassed);
        const bool parked = co_await poll_until([&] {
            return store->fill_status() ==
                   source::LayerStore::FillStatus::kStopped;
        });
        REQUIRE(parked);
        // Reads keep working remotely; nothing persists, no commit.
        std::vector<uint8_t> buf(kExtent);
        const ssize_t r = co_await store->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(kExtent));
        REQUIRE(buf == slice(blob, 0, kExtent));
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));
        REQUIRE(store->extents_present() == 0);
        REQUIRE(store->state() == source::LayerStore::State::Bypass);
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(!std::filesystem::exists(dir.str() + "/overlaybd.commit"));
}

TEST_CASE("source: layer store fill is disabled without download.enable",
          "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(2 * kExtent, 26);
    const std::string digest = digest_of(blob);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest);
        co_await elio::time::sleep_for(std::chrono::milliseconds(50));
        REQUIRE(store->fill_status() ==
                source::LayerStore::FillStatus::kDisabled);
        REQUIRE(store->extents_present() == 0);
        REQUIRE(vec->reads() == 0);  // no background traffic at all
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: layer store sweeps stale pairs when the commit binds",
          "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(2 * kExtent, 27);
    // The commit from a previous completed run, plus a leftover pair that
    // can never win the probe (disk leak to sweep).
    test::write_file(dir.str() + "/overlaybd.commit", blob);
    test::write_file(dir.str() + "/.download.deadbeefdeadbeef",
                     slice(blob, 0, kExtent));
    test::write_file(dir.str() + "/.bitmap.deadbeefdeadbeef",
                     std::vector<uint8_t>(80, 0));
    test::write_file(dir.str() + "/.download.0123456789abcdef",
                     std::vector<uint8_t>(1, 0));

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest_of(blob));
        REQUIRE(store->state() == source::LayerStore::State::Complete);
        std::vector<uint8_t> buf(1024);
        const ssize_t r = co_await store->pread(buf.data(), buf.size(), 5);
        REQUIRE(r == static_cast<ssize_t>(buf.size()));
        REQUIRE(buf == slice(blob, 5, buf.size()));
        REQUIRE(vec->reads() == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(names_with_prefix(dir.str(), ".download.").empty());
    REQUIRE(names_with_prefix(dir.str(), ".bitmap.").empty());
}

TEST_CASE("source: layer store fill does not starve readers", "[source]") {
    test::TempDir dir;
    auto blob = test::pattern_bytes(16 * kExtent, 28);
    const std::string digest = digest_of(blob);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto* vec = new VectorSource(blob);
        source::LayerStore::Config cfg;
        cfg.fill.enable = true;
        cfg.fill.delay_sec = 0;
        cfg.fill.delay_extra_sec = 0;
        cfg.queue_max_bytes = 2 * kExtent;  // fill sits in back-pressure
        auto store = co_await source::LayerStore::open(
            source::BlobSourcePtr(vec), dir.str(), digest, cfg);
        // Slow disk: fill stays active (queue-full waits) for the whole
        // measurement, contending with the readers below.
        store->set_test_write_hook([](uint64_t) -> int {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(20);
            while (std::chrono::steady_clock::now() < deadline) {
            }
            return 0;
        });
        const bool active = co_await poll_until([&] {
            return store->fill_status() ==
                   source::LayerStore::FillStatus::kFilling;
        });
        REQUIRE(active);
        // Scavenger claim, pinned: with fill mid-walk, cold-reader
        // latency stays bounded (fill never holds the fetch-coalescing
        // lock over a wait, and reader writes are droppable, never
        // blocking). 1s is two orders above a local in-memory fetch.
        std::vector<uint8_t> buf(kExtent);
        for (uint64_t e = 0; e < 8; ++e) {
            const auto a = std::chrono::steady_clock::now();
            const ssize_t r = co_await store->pread(buf.data(), buf.size(),
                                                    e * kExtent);
            const auto b = std::chrono::steady_clock::now();
            REQUIRE(r == static_cast<ssize_t>(kExtent));
            REQUIRE(buf == slice(blob, e * kExtent, kExtent));
            REQUIRE(b - a < std::chrono::seconds(1));
        }
        store->stop_fill();
        const bool parked = co_await poll_until([&] {
            return store->fill_status() ==
                       source::LayerStore::FillStatus::kStopped ||
                   store->fill_status() ==
                       source::LayerStore::FillStatus::kDone;
        });
        REQUIRE(parked);
        store->set_test_write_hook(nullptr);
        co_return 0;
    });
    REQUIRE(rc == 0);
}
