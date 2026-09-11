// Unit tests for trace recording (ADR-0013, record path): the
// TraceRecordSource tap + TraceRecorder sink from src/image/trace_record.hpp.
// Every produced blob is validated with the SAME codec reader the replay
// side and upstream consumers use (src/format/trace.hpp), and against the
// conforming-writer MUSTs of docs/trace-format.md §10.
//
// STYLE NOTE: each test runs its whole recording lifecycle (start, reads,
// stop) inside ONE run_coro: stop is the recorder's timer completion
// barrier, so the coroutine that starts the duration timer also drains it
// before the scheduler exits (see trace_record.hpp). No Catch2 macros
// between start and stop: a REQUIRE throw would skip cleanup; read
// results are collected and asserted after the stop.
#include "common/sha256.hpp"
#include "image/trace_record.hpp"
#include "source/layer_store.hpp"

#include "../support.hpp"

#include <elio/runtime/spawn.hpp>
#include <elio/time/timer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace obd;
using namespace std::chrono_literals;

namespace {

/// One read through a source (coroutine context). No assertions here.
elio::coro::task<ssize_t> read_at(source::BlobSource& src, uint64_t offset,
                                  size_t count) {
    std::vector<uint8_t> buf(count);
    const ssize_t r = co_await src.pread(buf.data(), buf.size(), offset);
    co_return r;
}

elio::coro::task<image::TraceRecorder::FinalizeResult> marked_stop(
    std::shared_ptr<image::TraceRecorder> rec, std::atomic<bool>* entered,
    std::string reason) {
    entered->store(true, std::memory_order_release);
    co_return co_await rec->stop(std::move(reason));
}

elio::coro::task<image::TraceRecorder::FinalizeResult> await_stop_handle(
    elio::coro::join_handle<image::TraceRecorder::FinalizeResult>& handle) {
    auto result = co_await handle;
    while (!handle.is_destroyed()) {
        co_await elio::time::sleep_for(1ms);
    }
    co_return result;
}

elio::coro::task<bool> await_bool_handle(
    elio::coro::join_handle<bool>& handle) {
    const bool result = co_await handle;
    while (!handle.is_destroyed()) {
        co_await elio::time::sleep_for(1ms);
    }
    co_return result;
}

template <typename Pred>
elio::coro::task<bool> wait_until(Pred pred, int attempts = 5000) {
    for (int i = 0; i < attempts; ++i) {
        if (pred()) co_return true;
        co_await elio::time::sleep_for(1ms);
    }
    co_return pred();
}

/// Starts the recorder, runs `body` (a coroutine lambda), and ALWAYS
/// stops — even when the body throws — so the duration timer never
/// leaks into scheduler teardown. Returns the stop result.
template <typename F>
elio::coro::task<image::TraceRecorder::FinalizeResult> with_recording(
    image::TraceRecorder& rec, const std::string& out, F&& body) {
    std::string error;
    const bool started =
        co_await rec.start(out, 300,
                           [](const image::TraceRecorder::FinalizeResult&) {},
                           error);
    if (!started) {
        throw std::runtime_error("recorder start failed: " + error);
    }
    // co_await is not permitted inside a catch handler: capture, clean
    // up after the block, then rethrow.
    std::exception_ptr err;
    try {
        co_await body();
    } catch (...) {
        err = std::current_exception();
    }
    if (err) {
        const auto ignored = co_await rec.stop("shutdown");
        (void)ignored;
        std::rethrow_exception(err);
    }
    co_return co_await rec.stop("stopped");
}

std::vector<uint8_t> read_file_bytes(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    struct stat st {};
    REQUIRE(::fstat(fd, &st) == 0);
    std::vector<uint8_t> blob(static_cast<size_t>(st.st_size));
    size_t done = 0;
    while (done < blob.size()) {
        const ssize_t r = ::read(fd, blob.data() + done, blob.size() - done);
        REQUIRE(r > 0);
        done += static_cast<size_t>(r);
    }
    ::close(fd);
    return blob;
}

std::string file_sha256(const std::string& path) {
    const auto blob = read_file_bytes(path);
    return common::Sha256::hex(blob.data(), blob.size());
}

std::vector<format::trace::TraceRecord> parse_file(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    struct stat st {};
    REQUIRE(::fstat(fd, &st) == 0);
    std::vector<uint8_t> blob(static_cast<size_t>(st.st_size));
    // ::read may return short even for regular files; loop for the full
    // contents (a single-read assumption is a real flake source).
    size_t got = 0;
    while (got < blob.size()) {
        const ssize_t n = ::read(fd, blob.data() + got, blob.size() - got);
        if (n <= 0) {
            ::close(fd);
            FAIL("short read of trace file");
        }
        got += static_cast<size_t>(n);
    }
    ::close(fd);
    auto parsed = format::trace::parse(blob);
    REQUIRE(parsed.has_value());
    return parsed.value();
}

}  // namespace

TEST_CASE("image: trace recording round-trips through the codec reader",
          "[image]") {
    test::TempDir dir;
    const auto data = test::pattern_bytes(512 * 1024, 61);
    const std::string out = dir / "out.trace";
    image::TraceRecorder::FinalizeResult res;
    std::vector<ssize_t> reads;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        image::TraceRecordSource tap(
            std::make_unique<test::VectorSource>(data), rec, 0);
        res = co_await with_recording(
            *rec, out, [&]() -> elio::coro::task<void> {
                reads.push_back(co_await read_at(tap, 0, 4096));
                reads.push_back(co_await read_at(tap, 65536, 8192));
                reads.push_back(co_await read_at(tap, 4096, 2048));
            });
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(reads == std::vector<ssize_t>{4096, 8192, 2048});

    REQUIRE(res.ok);
    REQUIRE(res.reason == "stopped");
    REQUIRE(res.records == 3);
    REQUIRE(res.dropped == 0);
    REQUIRE(res.size == format::trace::kHeaderSize +
                            3 * format::trace::kRecordSize);
    REQUIRE(res.sha256.size() == 64);
    // The reported digest is the real file's digest, not a length check.
    REQUIRE(res.sha256 == file_sha256(out));
    const auto records = parse_file(out);
    REQUIRE(records.size() == 3);
    REQUIRE(records[0] == format::trace::TraceRecord{'R', 0, 4096, 0});
    REQUIRE(records[1] == format::trace::TraceRecord{'R', 0, 8192, 65536});
    REQUIRE(records[2] == format::trace::TraceRecord{'R', 0, 2048, 4096});
}

TEST_CASE("image: trace recording coalesces adjacent records and preserves order",
          "[image]") {
    test::TempDir dir;
    const auto data = test::pattern_bytes(512 * 1024, 63);
    const std::string out = dir / "out.trace";
    image::TraceRecorder::FinalizeResult res;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        image::TraceRecordSource tap0(
            std::make_unique<test::VectorSource>(data), rec, 0);
        image::TraceRecordSource tap1(
            std::make_unique<test::VectorSource>(data), rec, 1);
        res = co_await with_recording(
            *rec, out, [&]() -> elio::coro::task<void> {
                // Adjacent same-layer reads merge (window: contiguity +
                // 1 MiB cap).
                co_await read_at(tap0, 0, 4096);
                co_await read_at(tap0, 4096, 4096);
                co_await read_at(tap0, 8192, 4096);
                // An interleaved other-layer record must NOT merge but
                // keeps order.
                co_await read_at(tap1, 1024, 512);
                // Non-adjacent same-layer: no merge.
                co_await read_at(tap0, 65536, 4096);
            });
        co_return 0;
    });
    REQUIRE(rc == 0);

    REQUIRE(res.ok);
    REQUIRE(res.records == 3);  // 3 merged into 1, +1 +1
    const auto records = parse_file(out);
    REQUIRE(records.size() == 3);
    REQUIRE(records[0] == format::trace::TraceRecord{'R', 0, 12288, 0});
    REQUIRE(records[1] == format::trace::TraceRecord{'R', 1, 512, 1024});
    REQUIRE(records[2] == format::trace::TraceRecord{'R', 0, 4096, 65536});
}

TEST_CASE("image: trace recording splits reads beyond the conforming count cap",
          "[image]") {
    test::TempDir dir;
    const auto data = test::pattern_bytes(3 * 1024 * 1024, 65);
    const std::string out = dir / "out.trace";
    image::TraceRecorder::FinalizeResult res;
    ssize_t big_read = -1;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        image::TraceRecordSource tap(
            std::make_unique<test::VectorSource>(data), rec, 0);
        res = co_await with_recording(
            *rec, out, [&]() -> elio::coro::task<void> {
                // 2.5 MiB in one pread: the writer contract caps a
                // record at 1 MiB, so the tap splits; the adjacent
                // pieces cannot merge (the merged count would exceed
                // the cap).
                big_read =
                    co_await read_at(tap, 0, 2 * 1024 * 1024 + 512 * 1024);
            });
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(big_read == 2 * 1024 * 1024 + 512 * 1024);

    REQUIRE(res.ok);
    REQUIRE(res.records == 3);
    const auto records = parse_file(out);
    REQUIRE(records.size() == 3);
    REQUIRE(records[0] == format::trace::TraceRecord{'R', 0, 1024 * 1024, 0});
    REQUIRE(records[1] ==
            format::trace::TraceRecord{'R', 0, 1024 * 1024, 1024 * 1024});
    REQUIRE(records[2] ==
            format::trace::TraceRecord{'R', 0, 512 * 1024, 2 * 1024 * 1024});
}

TEST_CASE("image: trace recording drops and counts records when the buffer fills",
          "[image]") {
    test::TempDir dir;
    const auto data = test::pattern_bytes(1024 * 1024, 67);
    const std::string out = dir / "out.trace";
    image::TraceRecorder::FinalizeResult res;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>(
            /*max_pending=*/4);
        image::TraceRecordSource tap(
            std::make_unique<test::VectorSource>(data), rec, 0);
        res = co_await with_recording(
            *rec, out, [&]() -> elio::coro::task<void> {
                // 10 non-adjacent reads against a 4-slot buffer: 4
                // kept, 6 dropped.
                for (int i = 0; i < 10; ++i) {
                    co_await read_at(tap, static_cast<uint64_t>(i) * 65536,
                                     4096);
                }
            });
        co_return 0;
    });
    REQUIRE(rc == 0);

    REQUIRE(res.ok);
    REQUIRE(res.records == 4);
    REQUIRE(res.dropped == 6);
    // A dropped-record trace is still a valid, parseable blob.
    const auto records = parse_file(out);
    REQUIRE(records.size() == 4);
    REQUIRE(records.front() == format::trace::TraceRecord{'R', 0, 4096, 0});
}

TEST_CASE("image: trace recording skips partial and failed reads",
          "[image]") {
    test::TempDir dir;
    const auto data = test::pattern_bytes(64 * 1024, 69);
    const std::string out = dir / "out.trace";
    image::TraceRecorder::FinalizeResult res;
    ssize_t short_read = -1;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        image::TraceRecordSource tap(
            std::make_unique<test::VectorSource>(data), rec, 0);
        res = co_await with_recording(
            *rec, out, [&]() -> elio::coro::task<void> {
                // A read crossing EOF is only partially satisfied: no
                // record.
                std::vector<uint8_t> buf(8192);
                short_read = co_await tap.pread(buf.data(), buf.size(),
                                                64 * 1024 - 4096);
                co_await read_at(tap, 0, 4096);  // good read, contrast
            });
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(short_read == 4096);  // the short read happened, unsatisfied

    REQUIRE(res.ok);
    REQUIRE(res.records == 1);
    const auto records = parse_file(out);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0] == format::trace::TraceRecord{'R', 0, 4096, 0});
}

TEST_CASE("image: trace recording is pass-through and error-clean when idle",
          "[image]") {
    test::TempDir dir;
    const auto data = test::pattern_bytes(64 * 1024, 71);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        image::TraceRecordSource tap(
            std::make_unique<test::VectorSource>(data), rec, 0);

        // Reads work with the recorder idle; nothing is queued.
        const ssize_t idle_read = co_await read_at(tap, 0, 4096);
        if (idle_read != 4096) co_return 1;
        // Stop with no recording ever started is a clean error.
        const auto res = co_await rec->stop("stopped");
        if (res.ok) co_return 2;
        if (res.error.find("no trace recording in progress") ==
            std::string::npos) {
            co_return 3;
        }
        co_return 0;
    });
    REQUIRE(rc == 0);

    // Start validates the duration bound and the output path (a FAILED
    // start spawns no timer, so these are safe outside the guard).
    const int rc2 = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        std::string error;
        const bool bad_dur =
            co_await rec->start(dir / "x.trace", 0,
                                [](const image::TraceRecorder::
                                       FinalizeResult&) {},
                                error);
        if (bad_dur || error.find("duration_sec") == std::string::npos) {
            co_return 1;
        }
        const bool bad_path =
            co_await rec->start("relative.trace", 60,
                                [](const image::TraceRecorder::
                                       FinalizeResult&) {},
                                error);
        if (bad_path || error.find("absolute") == std::string::npos) {
            co_return 2;
        }
        co_return 0;
    });
    REQUIRE(rc2 == 0);
}

TEST_CASE("image: trace recording stop is idempotent and reports expiry stats",
          "[image]") {
    test::TempDir dir;
    const auto data = test::pattern_bytes(64 * 1024, 73);
    const std::string out = dir / "out.trace";
    std::optional<image::TraceRecorder::FinalizeResult> expired;
    image::TraceRecorder::FinalizeResult again;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        image::TraceRecordSource tap(
            std::make_unique<test::VectorSource>(data), rec, 0);
        // Duration-1s recording, stopped by the timer with NO client
        // action — then an explicit stop must return the SAME finalized
        // stats (idempotent), which is what a CLI racing the expiry
        // needs.
        std::string error;
        const bool started = co_await rec->start(
            out, 1,
            [&](const image::TraceRecorder::FinalizeResult& r) {
                expired = r;
            },
            error);
        if (!started) co_return 1;
        const ssize_t r1 = co_await read_at(tap, 0, 4096);
        if (r1 != 4096) {
            const auto ignored = co_await rec->stop("shutdown");
            (void)ignored;
            co_return 2;
        }
        for (int i = 0; i < 100 && rec->recording(); ++i) {
            co_await elio::time::sleep_for(50ms);
        }
        again = co_await rec->stop("stopped");
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(expired.has_value());  // the timer fired with no client call

    REQUIRE(expired->ok);
    REQUIRE(expired->reason == "expired");
    REQUIRE(expired->records == 1);
    REQUIRE(again.ok);
    REQUIRE(again.reason == "expired");
    REQUIRE(again.records == expired->records);
    REQUIRE(again.sha256 == expired->sha256);
    const auto records = parse_file(out);
    REQUIRE(records.size() == 1);
}

TEST_CASE("image: trace recording stop drains an awakened duration timer",
          "[image]") {
    test::TempDir dir;
    const auto data = test::pattern_bytes(64 * 1024, 74);
    const std::string out = dir / "out.trace";
    std::atomic<bool> timer_awake{false};
    std::atomic<bool> timer_release{false};
    std::atomic<bool> stop_entered{false};
    std::optional<image::TraceRecorder::FinalizeResult> stopped;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        rec->set_timer_awake_hook_for_test(
            [&]() -> elio::coro::task<void> {
                timer_awake.store(true, std::memory_order_release);
                while (!timer_release.load(std::memory_order_acquire)) {
                    co_await elio::time::sleep_for(1ms);
                }
            });
        image::TraceRecordSource tap(
            std::make_unique<test::VectorSource>(data), rec, 0);
        std::string error;
        const bool started = co_await rec->start(
            out, 1,
            [](const image::TraceRecorder::FinalizeResult&) {}, error);
        if (!started) co_return 1;
        const ssize_t r1 = co_await read_at(tap, 0, 4096);
        if (r1 != 4096) {
            timer_release.store(true, std::memory_order_release);
            const auto ignored = co_await rec->stop("cleanup");
            (void)ignored;
            co_return 2;
        }
        const bool awake = co_await wait_until([&] {
            return timer_awake.load(std::memory_order_acquire);
        });
        if (!awake) {
            timer_release.store(true, std::memory_order_release);
            stopped = co_await rec->stop("cleanup");
            co_return 3;
        }

        auto stop_task = elio::spawn(marked_stop, rec, &stop_entered,
                                    std::string("shutdown"));
        const bool entered = co_await wait_until([&] {
            return stop_entered.load(std::memory_order_acquire);
        });
        if (!entered) {
            timer_release.store(true, std::memory_order_release);
            stopped = co_await await_stop_handle(stop_task);
            co_return 4;
        }
        const bool finalized = co_await wait_until([&] {
            return rec->last_result().has_value() || stop_task.is_ready();
        });
        if (!finalized) {
            timer_release.store(true, std::memory_order_release);
            stopped = co_await await_stop_handle(stop_task);
            co_return 5;
        }
        if (stop_task.is_ready() || stop_task.is_destroyed()) {
            timer_release.store(true, std::memory_order_release);
            stopped = co_await await_stop_handle(stop_task);
            co_return 6;
        }
        timer_release.store(true, std::memory_order_release);
        stopped = co_await await_stop_handle(stop_task);
        rec->set_timer_awake_hook_for_test(nullptr);
        co_return 0;
    });
    REQUIRE(rc == 0);

    REQUIRE(stopped.has_value());
    REQUIRE(stopped->ok);
    REQUIRE(stopped->reason == "shutdown");
    REQUIRE(stopped->records == 1);
    const auto records = parse_file(out);
    REQUIRE(records.size() == 1);
}

TEST_CASE("image: trace recording shutdown joins expiry finalization",
          "[image]") {
    test::TempDir dir;
    const auto data = test::pattern_bytes(64 * 1024, 76);
    const std::string out = dir / "out.trace";
    std::atomic<bool> finalize_entered{false};
    std::atomic<bool> finalize_release{false};
    std::atomic<bool> shutdown_entered{false};
    bool recording_during_finalize = true;
    std::optional<image::TraceRecorder::FinalizeResult> shutdown_stop;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        rec->set_finalize_hook_for_test(
            [&]() -> elio::coro::task<void> {
                finalize_entered.store(true, std::memory_order_release);
                while (!finalize_release.load(std::memory_order_acquire)) {
                    co_await elio::time::sleep_for(1ms);
                }
            });
        image::TraceRecordSource tap(
            std::make_unique<test::VectorSource>(data), rec, 0);
        std::string error;
        const bool started = co_await rec->start(
            out, 1,
            [](const image::TraceRecorder::FinalizeResult&) {}, error);
        if (!started) co_return 1;
        const ssize_t r1 = co_await read_at(tap, 0, 4096);
        if (r1 != 4096) {
            finalize_release.store(true, std::memory_order_release);
            const auto ignored = co_await rec->stop("cleanup");
            (void)ignored;
            co_return 2;
        }
        const bool entered = co_await wait_until([&] {
            return finalize_entered.load(std::memory_order_acquire);
        });
        if (!entered) {
            finalize_release.store(true, std::memory_order_release);
            shutdown_stop = co_await rec->stop("cleanup");
            co_return 3;
        }
        recording_during_finalize = rec->recording();

        auto stop_task = elio::spawn(marked_stop, rec, &shutdown_entered,
                                    std::string("shutdown"));
        const bool stop_entered = co_await wait_until([&] {
            return shutdown_entered.load(std::memory_order_acquire);
        });
        if (!stop_entered) {
            finalize_release.store(true, std::memory_order_release);
            shutdown_stop = co_await await_stop_handle(stop_task);
            co_return 4;
        }
        co_await elio::time::sleep_for(20ms);
        if (stop_task.is_ready() || stop_task.is_destroyed()) {
            finalize_release.store(true, std::memory_order_release);
            shutdown_stop = co_await await_stop_handle(stop_task);
            co_return 5;
        }
        finalize_release.store(true, std::memory_order_release);
        shutdown_stop = co_await await_stop_handle(stop_task);
        rec->set_finalize_hook_for_test(nullptr);
        co_return 0;
    });
    REQUIRE(rc == 0);

    REQUIRE_FALSE(recording_during_finalize);
    REQUIRE(shutdown_stop.has_value());
    REQUIRE(shutdown_stop->ok);
    REQUIRE(shutdown_stop->reason == "expired");
    REQUIRE(shutdown_stop->records == 1);
    const auto records = parse_file(out);
    REQUIRE(records.size() == 1);
}

TEST_CASE("image: trace recording late stop waits for expiry callback completion",
          "[image]") {
    test::TempDir dir;
    const auto data = test::pattern_bytes(64 * 1024, 78);
    const std::string out = dir / "out.trace";
    std::atomic<bool> callback_seen{false};
    std::atomic<bool> timer_exit_entered{false};
    std::atomic<bool> timer_exit_release{false};
    std::atomic<bool> shutdown_entered{false};
    std::optional<image::TraceRecorder::FinalizeResult> shutdown_stop;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        rec->set_timer_exit_hook_for_test(
            [&]() -> elio::coro::task<void> {
                timer_exit_entered.store(true, std::memory_order_release);
                while (!timer_exit_release.load(std::memory_order_acquire)) {
                    co_await elio::time::sleep_for(1ms);
                }
            });
        image::TraceRecordSource tap(
            std::make_unique<test::VectorSource>(data), rec, 0);
        std::string error;
        const bool started = co_await rec->start(
            out, 1,
            [&](const image::TraceRecorder::FinalizeResult&) {
                callback_seen.store(true, std::memory_order_release);
            },
            error);
        if (!started) co_return 1;
        const ssize_t r1 = co_await read_at(tap, 0, 4096);
        if (r1 != 4096) {
            timer_exit_release.store(true, std::memory_order_release);
            const auto ignored = co_await rec->stop("cleanup");
            (void)ignored;
            co_return 2;
        }
        const bool exit_held = co_await wait_until([&] {
            return timer_exit_entered.load(std::memory_order_acquire);
        });
        if (!exit_held) {
            timer_exit_release.store(true, std::memory_order_release);
            shutdown_stop = co_await rec->stop("cleanup");
            co_return 3;
        }

        auto stop_task = elio::spawn(marked_stop, rec, &shutdown_entered,
                                    std::string("shutdown"));
        const bool stop_entered = co_await wait_until([&] {
            return shutdown_entered.load(std::memory_order_acquire);
        });
        if (!stop_entered) {
            timer_exit_release.store(true, std::memory_order_release);
            shutdown_stop = co_await await_stop_handle(stop_task);
            co_return 4;
        }
        co_await elio::time::sleep_for(20ms);
        if (stop_task.is_ready() || stop_task.is_destroyed()) {
            timer_exit_release.store(true, std::memory_order_release);
            shutdown_stop = co_await await_stop_handle(stop_task);
            co_return 5;
        }
        timer_exit_release.store(true, std::memory_order_release);
        shutdown_stop = co_await await_stop_handle(stop_task);
        rec->set_timer_exit_hook_for_test(nullptr);
        co_return 0;
    });
    REQUIRE(rc == 0);

    REQUIRE(callback_seen.load(std::memory_order_acquire));
    REQUIRE(shutdown_stop.has_value());
    REQUIRE(shutdown_stop->ok);
    REQUIRE(shutdown_stop->reason == "expired");
    REQUIRE(shutdown_stop->records == 1);
    const auto records = parse_file(out);
    REQUIRE(records.size() == 1);
}

TEST_CASE("image: trace recording expiry callback stop never joins itself",
          "[image]") {
    test::TempDir dir;
    const auto data = test::pattern_bytes(64 * 1024, 80);
    const std::string out = dir / "out.trace";
    const std::string next_out = dir / "next.trace";
    std::atomic<bool> timer_exit_entered{false};
    std::atomic<bool> timer_exit_release{false};
    bool callback_made_stop_task = false;
    std::optional<elio::coro::task<image::TraceRecorder::FinalizeResult>>
        callback_stop_task;
    std::optional<elio::coro::task<image::TraceRecorder::FinalizeResult>>
        self_join_stop_task;
    std::optional<elio::coro::task<bool>> callback_start_task;
    std::optional<image::TraceRecorder::FinalizeResult> callback_stop;
    std::optional<image::TraceRecorder::FinalizeResult> self_join_stop;
    std::optional<image::TraceRecorder::FinalizeResult> cleanup_stop;
    bool callback_start_ok = true;
    std::string callback_start_error;
    bool restarted = false;
    std::string restart_error;
    bool recording_after_callback_stop = false;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        rec->set_timer_exit_hook_for_test(
            [&]() -> elio::coro::task<void> {
                timer_exit_entered.store(true, std::memory_order_release);
                while (!timer_exit_release.load(std::memory_order_acquire)) {
                    co_await elio::time::sleep_for(1ms);
                }
            });
        image::TraceRecordSource tap(
            std::make_unique<test::VectorSource>(data), rec, 0);
        std::string error;
        const bool started = co_await rec->start(
            out, 1,
            [&](const image::TraceRecorder::FinalizeResult&) {
                self_join_stop_task.emplace(rec->stop("self-join"));
                callback_stop_task.emplace(rec->stop("callback"));
                callback_start_task.emplace(rec->start(
                    next_out, 300,
                    [](const image::TraceRecorder::FinalizeResult&) {},
                    callback_start_error));
                callback_made_stop_task = true;
            },
            error);
        if (!started) co_return 1;
        const ssize_t r1 = co_await read_at(tap, 0, 4096);
        if (r1 != 4096) {
            timer_exit_release.store(true, std::memory_order_release);
            const auto ignored = co_await rec->stop("cleanup");
            (void)ignored;
            co_return 2;
        }
        const bool exit_held = co_await wait_until([&] {
            return timer_exit_entered.load(std::memory_order_acquire);
        });
        if (!exit_held || !callback_made_stop_task || !self_join_stop_task ||
            !callback_stop_task || !callback_start_task) {
            timer_exit_release.store(true, std::memory_order_release);
            cleanup_stop = co_await rec->stop("cleanup");
            co_return 3;
        }

        auto self_join_stop_handle =
            elio::spawn(std::move(*self_join_stop_task));
        const bool self_join_ready = co_await wait_until([&] {
            return self_join_stop_handle.is_ready() ||
                   self_join_stop_handle.is_destroyed();
        }, 200);
        if (!self_join_ready) {
            timer_exit_release.store(true, std::memory_order_release);
            self_join_stop = co_await await_stop_handle(self_join_stop_handle);
            cleanup_stop = co_await rec->stop("cleanup");
            co_return 4;
        }
        self_join_stop = co_await await_stop_handle(self_join_stop_handle);

        timer_exit_release.store(true, std::memory_order_release);
        rec->set_timer_exit_hook_for_test(nullptr);
        restarted = co_await rec->start(
            next_out, 300,
            [](const image::TraceRecorder::FinalizeResult&) {},
            restart_error);
        if (!restarted) {
            cleanup_stop = co_await rec->stop("cleanup");
            co_return 5;
        }

        auto reentrant_stop = elio::spawn(std::move(*callback_stop_task));
        const bool reentrant_ready = co_await wait_until([&] {
            return reentrant_stop.is_ready() || reentrant_stop.is_destroyed();
        }, 200);
        if (!reentrant_ready) {
            callback_stop = co_await await_stop_handle(reentrant_stop);
            cleanup_stop = co_await rec->stop("cleanup");
            co_return 6;
        }
        callback_stop = co_await await_stop_handle(reentrant_stop);
        recording_after_callback_stop = rec->recording();

        auto reentrant_start = elio::spawn(std::move(*callback_start_task));
        const bool start_ready = co_await wait_until([&] {
            return reentrant_start.is_ready() || reentrant_start.is_destroyed();
        }, 200);
        if (!start_ready) {
            callback_start_ok = co_await await_bool_handle(reentrant_start);
            cleanup_stop = co_await rec->stop("cleanup");
            co_return 7;
        }
        callback_start_ok = co_await await_bool_handle(reentrant_start);
        cleanup_stop = co_await rec->stop("cleanup");
        co_return 0;
    });
    REQUIRE(rc == 0);

    REQUIRE(restarted);
    REQUIRE(recording_after_callback_stop);
    REQUIRE(self_join_stop.has_value());
    REQUIRE(self_join_stop->ok);
    REQUIRE(self_join_stop->reason == "expired");
    REQUIRE(callback_stop.has_value());
    REQUIRE(callback_stop->ok);
    REQUIRE(callback_stop->reason == "expired");
    REQUIRE_FALSE(callback_start_ok);
    REQUIRE(callback_start_error.find("expiry callback") !=
            std::string::npos);
    REQUIRE(cleanup_stop.has_value());
    REQUIRE(cleanup_stop->ok);
    REQUIRE(cleanup_stop->reason == "cleanup");
    REQUIRE(parse_file(out).size() == 1);
    REQUIRE(parse_file(next_out).empty());
}

TEST_CASE("image: trace recording stale stop never drains a restarted timer",
          "[image]") {
    test::TempDir dir;
    const auto data = test::pattern_bytes(64 * 1024, 79);
    const std::string first_out = dir / "first.trace";
    const std::string second_out = dir / "second.trace";
    std::atomic<bool> timer_exit_entered{false};
    std::atomic<bool> timer_exit_release{false};
    std::atomic<bool> drain_claimed{false};
    std::atomic<bool> drain_claim_release{false};
    std::atomic<bool> drain_waiting{false};
    std::atomic<bool> drain_wait_release{false};
    std::atomic<bool> first_stop_entered{false};
    std::atomic<bool> stale_stop_entered{false};
    std::optional<image::TraceRecorder::FinalizeResult> first_stop;
    std::optional<image::TraceRecorder::FinalizeResult> stale_stop;
    std::optional<image::TraceRecorder::FinalizeResult> cleanup_stop;
    bool second_started = false;
    bool recording_after_stale_stop = false;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        rec->set_timer_exit_hook_for_test(
            [&]() -> elio::coro::task<void> {
                timer_exit_entered.store(true, std::memory_order_release);
                while (!timer_exit_release.load(std::memory_order_acquire)) {
                    co_await elio::time::sleep_for(1ms);
                }
            });
        rec->set_timer_drain_claim_hook_for_test(
            [&]() -> elio::coro::task<void> {
                drain_claimed.store(true, std::memory_order_release);
                while (!drain_claim_release.load(std::memory_order_acquire)) {
                    co_await elio::time::sleep_for(1ms);
                }
            });
        rec->set_timer_drain_wait_hook_for_test(
            [&]() -> elio::coro::task<void> {
                drain_waiting.store(true, std::memory_order_release);
                while (!drain_wait_release.load(std::memory_order_acquire)) {
                    co_await elio::time::sleep_for(1ms);
                }
            });
        auto release_all = [&] {
            timer_exit_release.store(true, std::memory_order_release);
            drain_claim_release.store(true, std::memory_order_release);
            drain_wait_release.store(true, std::memory_order_release);
        };

        image::TraceRecordSource first_tap(
            std::make_unique<test::VectorSource>(data), rec, 0);
        std::string error;
        const bool started = co_await rec->start(
            first_out, 1,
            [](const image::TraceRecorder::FinalizeResult&) {}, error);
        if (!started) co_return 1;
        const ssize_t r1 = co_await read_at(first_tap, 0, 4096);
        if (r1 != 4096) {
            release_all();
            const auto ignored = co_await rec->stop("cleanup");
            (void)ignored;
            co_return 2;
        }
        const bool exit_held = co_await wait_until([&] {
            return timer_exit_entered.load(std::memory_order_acquire);
        });
        if (!exit_held) {
            release_all();
            stale_stop = co_await rec->stop("cleanup");
            co_return 3;
        }

        auto first_task = elio::spawn(marked_stop, rec, &first_stop_entered,
                                     std::string("shutdown"));
        const bool claimed = co_await wait_until([&] {
            return first_stop_entered.load(std::memory_order_acquire) &&
                   drain_claimed.load(std::memory_order_acquire);
        });
        if (!claimed) {
            release_all();
            first_stop = co_await await_stop_handle(first_task);
            co_return 4;
        }

        auto stale_task = elio::spawn(marked_stop, rec, &stale_stop_entered,
                                     std::string("shutdown"));
        const bool waiting = co_await wait_until([&] {
            return stale_stop_entered.load(std::memory_order_acquire) &&
                   drain_waiting.load(std::memory_order_acquire);
        });
        if (!waiting) {
            release_all();
            first_stop = co_await await_stop_handle(first_task);
            stale_stop = co_await await_stop_handle(stale_task);
            co_return 5;
        }

        timer_exit_release.store(true, std::memory_order_release);
        drain_claim_release.store(true, std::memory_order_release);
        first_stop = co_await await_stop_handle(first_task);
        rec->set_timer_exit_hook_for_test(nullptr);
        rec->set_timer_drain_claim_hook_for_test(nullptr);

        image::TraceRecordSource second_tap(
            std::make_unique<test::VectorSource>(data), rec, 0);
        second_started = co_await rec->start(
            second_out, 300,
            [](const image::TraceRecorder::FinalizeResult&) {}, error);
        if (!second_started) {
            release_all();
            stale_stop = co_await await_stop_handle(stale_task);
            co_return 6;
        }

        drain_wait_release.store(true, std::memory_order_release);
        const bool stale_ready = co_await wait_until([&] {
            return stale_task.is_ready() || stale_task.is_destroyed();
        }, 200);
        if (!stale_ready) {
            cleanup_stop = co_await rec->stop("cleanup");
            stale_stop = co_await await_stop_handle(stale_task);
            rec->set_timer_drain_wait_hook_for_test(nullptr);
            co_return 7;
        }
        stale_stop = co_await await_stop_handle(stale_task);
        recording_after_stale_stop = rec->recording();
        cleanup_stop = co_await rec->stop("cleanup");
        rec->set_timer_drain_wait_hook_for_test(nullptr);
        co_return recording_after_stale_stop ? 0 : 8;
    });
    REQUIRE(rc == 0);

    REQUIRE(second_started);
    REQUIRE(recording_after_stale_stop);
    REQUIRE(first_stop.has_value());
    REQUIRE(first_stop->ok);
    REQUIRE(first_stop->reason == "expired");
    REQUIRE(stale_stop.has_value());
    REQUIRE(stale_stop->ok);
    REQUIRE(stale_stop->reason == "expired");
    REQUIRE(cleanup_stop.has_value());
    REQUIRE(cleanup_stop->ok);
    REQUIRE(cleanup_stop->reason == "cleanup");
    REQUIRE(parse_file(first_out).size() == 1);
    REQUIRE(parse_file(second_out).empty());
}

TEST_CASE("image: concurrent trace stops join the winning finalize",
          "[image]") {
    test::TempDir dir;
    const auto data = test::pattern_bytes(64 * 1024, 74);
    const std::string out = dir / "out.trace";
    std::atomic<int> stop_claim_entries{0};
    std::atomic<bool> first_stop_claim_release{false};
    std::atomic<bool> second_stop_claim_release{false};
    std::atomic<bool> finalize_entered{false};
    std::atomic<bool> finalize_release{false};
    std::atomic<bool> explicit_stop_entered{false};
    std::optional<image::TraceRecorder::FinalizeResult> expiry_stop;
    std::optional<image::TraceRecorder::FinalizeResult> explicit_stop;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        rec->set_stop_claim_hook_for_test(
            [&]() -> elio::coro::task<void> {
                const int entry =
                    stop_claim_entries.fetch_add(
                        1, std::memory_order_acq_rel) +
                    1;
                auto& release = entry == 1 ? first_stop_claim_release
                                           : second_stop_claim_release;
                while (!release.load(std::memory_order_acquire)) {
                    co_await elio::time::sleep_for(1ms);
                }
            });
        rec->set_finalize_hook_for_test(
            [&]() -> elio::coro::task<void> {
                finalize_entered.store(true, std::memory_order_release);
                while (!finalize_release.load(std::memory_order_acquire)) {
                    co_await elio::time::sleep_for(1ms);
                }
            });
        auto release_all = [&] {
            first_stop_claim_release.store(true, std::memory_order_release);
            second_stop_claim_release.store(true, std::memory_order_release);
            finalize_release.store(true, std::memory_order_release);
            rec->set_stop_claim_hook_for_test(nullptr);
            rec->set_finalize_hook_for_test(nullptr);
        };
        image::TraceRecordSource tap(
            std::make_unique<test::VectorSource>(data), rec, 0);
        std::string error;
        const bool started = co_await rec->start(
            out, 300,
            [](const image::TraceRecorder::FinalizeResult&) {}, error);
        if (!started) co_return 1;
        const ssize_t r1 = co_await read_at(tap, 0, 4096);
        if (r1 != 4096) {
            release_all();
            const auto ignored = co_await rec->stop("cleanup");
            (void)ignored;
            co_return 2;
        }

        // Both stop callers are held after observing Recording but before
        // finalizer ownership is selected. The old split-state stop() let
        // both callers pass its first check; the loser then saw the winner's
        // Recording -> Finalizing transition as "not Recording" and returned
        // a spurious no-recording error. The fixed stop() selects or joins
        // under one state decision and binds joiners to that finalize result.
        auto expiry_task = elio::spawn(rec->stop("expired"));
        for (int i = 0;
             i < 5000 &&
             stop_claim_entries.load(std::memory_order_acquire) < 1; ++i) {
            co_await elio::time::sleep_for(1ms);
        }
        if (stop_claim_entries.load(std::memory_order_acquire) < 1) {
            release_all();
            expiry_stop = co_await await_stop_handle(expiry_task);
            co_return 3;
        }

        auto explicit_task = elio::spawn(marked_stop, rec,
                                        &explicit_stop_entered,
                                        std::string("stopped"));
        for (int i = 0;
             i < 5000 &&
             (stop_claim_entries.load(std::memory_order_acquire) < 2 ||
              !explicit_stop_entered.load(std::memory_order_acquire)); ++i) {
            co_await elio::time::sleep_for(1ms);
        }
        if (stop_claim_entries.load(std::memory_order_acquire) < 2 ||
            !explicit_stop_entered.load(std::memory_order_acquire)) {
            release_all();
            expiry_stop = co_await await_stop_handle(expiry_task);
            explicit_stop = co_await await_stop_handle(explicit_task);
            co_return 4;
        }

        // Let the first stop (the simulated expiry) claim finalization,
        // then hold it in the finalize hook before releasing the explicit
        // stop to join that exact completion.
        first_stop_claim_release.store(true, std::memory_order_release);
        for (int i = 0;
             i < 5000 &&
             !finalize_entered.load(std::memory_order_acquire); ++i) {
            co_await elio::time::sleep_for(1ms);
        }
        if (!finalize_entered.load(std::memory_order_acquire)) {
            release_all();
            expiry_stop = co_await await_stop_handle(expiry_task);
            explicit_stop = co_await await_stop_handle(explicit_task);
            co_return 5;
        }
        // Give the loser a chance to hit the join path while finalization is
        // still held. The old split-state implementation could return a
        // spurious no-recording error from this window.
        second_stop_claim_release.store(true, std::memory_order_release);
        co_await elio::time::sleep_for(10ms);
        release_all();
        expiry_stop = co_await await_stop_handle(expiry_task);
        explicit_stop = co_await await_stop_handle(explicit_task);
        co_return 0;
    });
    REQUIRE(rc == 0);

    REQUIRE(expiry_stop->ok);
    REQUIRE(explicit_stop->ok);
    REQUIRE(expiry_stop->reason == "expired");
    REQUIRE(explicit_stop->reason == "expired");
    REQUIRE(explicit_stop->path == expiry_stop->path);
    REQUIRE(explicit_stop->records == expiry_stop->records);
    REQUIRE(explicit_stop->dropped == expiry_stop->dropped);
    REQUIRE(explicit_stop->size == expiry_stop->size);
    REQUIRE(explicit_stop->sha256 == expiry_stop->sha256);
    const auto records = parse_file(out);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0] == format::trace::TraceRecord{'R', 0, 4096, 0});
}

TEST_CASE("image: trace recording captures only remote fetches through the layer store",
          "[image]") {
    // The tap sits at the remote source below the LayerStore: a second
    // read of a persisted extent is a LOCAL hit and must produce NO new
    // record (ADR-0013: one record per fully-satisfied REMOTE pread).
    test::TempDir dir;
    const auto data = test::pattern_bytes(512 * 1024, 75);
    const std::string layer_dir = dir / "layer";
    REQUIRE(std::filesystem::create_directories(layer_dir));
    const std::string out = dir / "out.trace";
    image::TraceRecorder::FinalizeResult res;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        auto tap = std::make_unique<image::TraceRecordSource>(
            std::make_unique<test::VectorSource>(data), rec, 0);
        auto store = co_await source::LayerStore::open(std::move(tap),
                                                       layer_dir, "");

        // Park/destroy inside this scheduler (LayerStore lifetime).
        auto park = [&]() -> elio::coro::task<void> {
            store->stop_fill();
            using FillStatus = source::LayerStore::FillStatus;
            for (int i = 0; i < 5000; ++i) {
                const FillStatus s = store->fill_status();
                if (s == FillStatus::kDisabled || s == FillStatus::kDone ||
                    s == FillStatus::kStopped) {
                    break;
                }
                co_await elio::time::sleep_for(1ms);
            }
            store.reset();
        };
        std::exception_ptr err;
        try {
            res = co_await with_recording(
                *rec, out, [&]() -> elio::coro::task<void> {
                    // First read: extent-0 miss -> one 64 KiB remote
                    // fetch recorded. Wait for the write-behind to
                    // PERSIST the extent (an earlier re-read would miss
                    // the still-in-flight staging write and re-fetch
                    // remotely — a legitimate second record); after
                    // that, reads of the extent are local hits with NO
                    // new record.
                    co_await read_at(*store, 0, 4096);
                    for (int i = 0; i < 5000 &&
                                    store->extents_present() < 1; ++i) {
                        co_await elio::time::sleep_for(1ms);
                    }
                    co_await read_at(*store, 0, 4096);
                    co_await read_at(*store, 1024, 4096);
                    // Extent 3 (non-adjacent, so the documented
                    // coalescing window does not merge it into the
                    // extent-0 record): one more fetch.
                    co_await read_at(*store, 3 * 65536, 4096);
                });
        } catch (...) {
            err = std::current_exception();
        }
        co_await park();
        if (err) std::rethrow_exception(err);
        co_return 0;
    });
    REQUIRE(rc == 0);

    REQUIRE(res.ok);
    REQUIRE(res.records == 2);
    const auto records = parse_file(out);
    REQUIRE(records.size() == 2);
    // No tar wrapper here (base 0): records are extent reads verbatim.
    REQUIRE(records[0] == format::trace::TraceRecord{'R', 0, 65536, 0});
    REQUIRE(records[1] ==
            format::trace::TraceRecord{'R', 0, 65536, 3 * 65536});
}

TEST_CASE("image: trace recording translates offsets out of the tar wrapper",
          "[image]") {
    // The tar base is subtracted so recorded offsets address the payload
    // space replay consumes; a read spanning the header clamps to the
    // payload overlap (extent fetches include the 512-byte header).
    test::TempDir dir;
    const auto data = test::pattern_bytes(512 * 1024, 77);
    const std::string out = dir / "out.trace";
    image::TraceRecorder::FinalizeResult res;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        image::TraceRecordSource tap(
            std::make_unique<test::VectorSource>(data), rec, 0);
        tap.set_base(512);  // what assembly does after the tar probe

        res = co_await with_recording(
            *rec, out, [&]() -> elio::coro::task<void> {
                // Raw [0, 65536) = header + payload [0, 65024): the
                // header overlap is clamped away, the record covers
                // [0, 65024).
                co_await read_at(tap, 0, 65536);
                // Raw [262144, +65536) = payload [261632, 327168).
                co_await read_at(tap, 262144, 65536);
            });
        co_return 0;
    });
    REQUIRE(rc == 0);

    REQUIRE(res.ok);
    const auto records = parse_file(out);
    REQUIRE(records.size() == 2);
    REQUIRE(records[0] ==
            format::trace::TraceRecord{'R', 0, 65536 - 512, 0});
    REQUIRE(records[1] ==
            format::trace::TraceRecord{'R', 0, 65536, 262144 - 512});
}

TEST_CASE("image: trace recording finalizes an empty window to a valid header-only blob",
          "[image]") {
    // Stop before any record: the queue drains empty, the codec writer
    // still performs the header checksum rewrite, and the C2 reader
    // accepts the 24-byte header-only blob.
    test::TempDir dir;
    const std::string out = dir / "out.trace";
    image::TraceRecorder::FinalizeResult res;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        image::TraceRecorder rec;
        res = co_await with_recording(
            rec, out, []() -> elio::coro::task<void> { co_return; });
        co_return 0;
    });
    REQUIRE(rc == 0);

    REQUIRE(res.ok);
    REQUIRE(res.records == 0);
    REQUIRE(res.dropped == 0);
    REQUIRE(res.size == format::trace::kHeaderSize);
    REQUIRE(res.sha256 == file_sha256(out));
    REQUIRE(parse_file(out).empty());
}

TEST_CASE("image: trace recording restarts cleanly after a stop",
          "[image]") {
    // start -> stop -> start: the second window opens fresh (queue and
    // drop counter reset), records only its own reads, and the first
    // blob stays intact.
    test::TempDir dir;
    const auto data = test::pattern_bytes(512 * 1024, 91);
    const std::string out1 = dir / "one.trace";
    const std::string out2 = dir / "two.trace";
    image::TraceRecorder::FinalizeResult res1, res2;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        image::TraceRecordSource tap(
            std::make_unique<test::VectorSource>(data), rec, 0);
        res1 = co_await with_recording(
            *rec, out1, [&]() -> elio::coro::task<void> {
                co_await read_at(tap, 0, 4096);
            });
        res2 = co_await with_recording(
            *rec, out2, [&]() -> elio::coro::task<void> {
                co_await read_at(tap, 65536, 4096);
                co_await read_at(tap, 131072, 4096);
            });
        co_return 0;
    });
    REQUIRE(rc == 0);

    REQUIRE(res1.ok);
    REQUIRE(res2.ok);
    REQUIRE(res1.records == 1);
    REQUIRE(res2.records == 2);
    REQUIRE(res2.dropped == 0);  // counters reset per window
    const auto first = parse_file(out1);
    REQUIRE(first.size() == 1);
    REQUIRE(first[0] == format::trace::TraceRecord{'R', 0, 4096, 0});
    const auto second = parse_file(out2);
    REQUIRE(second.size() == 2);
    REQUIRE(second[0] == format::trace::TraceRecord{'R', 0, 4096, 65536});
    REQUIRE(second[1] == format::trace::TraceRecord{'R', 0, 4096, 131072});
}

TEST_CASE("image: trace recording rejects start while a finalize is in flight",
          "[image]") {
    // M1 regression: a start slipping into an in-flight finalize would
    // wipe the draining queue and corrupt its stats. The test-only
    // finalize hook holds the finalize open (state Finalizing) so the
    // second start deterministically meets it and is rejected with the
    // clean "already in progress" error; the in-flight finalize then
    // completes with its records and stats intact.
    test::TempDir dir;
    const auto data = test::pattern_bytes(512 * 1024, 93);
    const std::string out1 = dir / "one.trace";
    const std::string out2 = dir / "two.trace";
    std::atomic<bool> finalize_entered{false};
    std::atomic<bool> finalize_release{false};
    bool second_start_ok = true;
    std::string second_start_error;
    std::optional<image::TraceRecorder::FinalizeResult> stop_res;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        rec->set_finalize_hook_for_test(
            [&]() -> elio::coro::task<void> {
                finalize_entered.store(true, std::memory_order_release);
                while (!finalize_release.load(std::memory_order_acquire)) {
                    co_await elio::time::sleep_for(1ms);
                }
            });
        image::TraceRecordSource tap(
            std::make_unique<test::VectorSource>(data), rec, 0);
        std::string error;
        const bool started = co_await rec->start(
            out1, 300,
            [](const image::TraceRecorder::FinalizeResult&) {}, error);
        if (!started) co_return 1;
        co_await read_at(tap, 0, 4096);
        // Stop concurrently: the finalize reaches the hook and parks.
        elio::go([&]() -> elio::coro::task<void> {
            stop_res = co_await rec->stop("stopped");
        });
        for (int i = 0;
             i < 5000 &&
             !finalize_entered.load(std::memory_order_acquire); ++i) {
            co_await elio::time::sleep_for(1ms);
        }
        if (!finalize_entered.load(std::memory_order_acquire)) co_return 2;
        // The finalize is held open: a start now MUST be rejected.
        second_start_ok = co_await rec->start(
            out2, 300,
            [](const image::TraceRecorder::FinalizeResult&) {},
            second_start_error);
        finalize_release.store(true, std::memory_order_release);
        for (int i = 0; i < 5000 && !stop_res.has_value(); ++i) {
            co_await elio::time::sleep_for(1ms);
        }
        rec->set_finalize_hook_for_test(nullptr);
        co_return stop_res.has_value() ? 0 : 3;
    });
    REQUIRE(rc == 0);

    REQUIRE(!second_start_ok);
    REQUIRE(second_start_error.find("already in progress") !=
            std::string::npos);
    // The held finalize completed untouched: its record and digest.
    REQUIRE(stop_res->ok);
    REQUIRE(stop_res->records == 1);
    REQUIRE(stop_res->dropped == 0);
    REQUIRE(stop_res->sha256 == file_sha256(out1));
    const auto records = parse_file(out1);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0] == format::trace::TraceRecord{'R', 0, 4096, 0});
}

TEST_CASE("image: trace recording rejected start never truncates existing files",
          "[image]") {
    // Regression (PR #34 review): start() once opened its output with
    // O_TRUNC BEFORE the state gate, so a REJECTED start truncated
    // whatever path it was given — including a previous recording's
    // valid finalized blob. Now the gate runs before any open: the
    // first blob's bytes must survive the rejected start untouched,
    // and the active recording's own finalize must produce its
    // complete valid blob.
    test::TempDir dir;
    const auto data = test::pattern_bytes(512 * 1024, 95);
    const std::string out1 = dir / "one.trace";
    const std::string out2 = dir / "two.trace";
    image::TraceRecorder::FinalizeResult res1, res2;
    bool second_ok = true, third_ok = true;
    std::string second_err, third_err;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        image::TraceRecordSource tap(
            std::make_unique<test::VectorSource>(data), rec, 0);
        // Window 1: a complete valid blob at out1.
        res1 = co_await with_recording(
            *rec, out1, [&]() -> elio::coro::task<void> {
                co_await read_at(tap, 0, 4096);
            });
        // Window 2 active at out2; a rejected start aimed at out1 must
        // not touch it, one aimed at out2 must not break the window.
        std::string error;
        const bool started = co_await rec->start(
            out2, 300,
            [](const image::TraceRecorder::FinalizeResult&) {}, error);
        if (!started) co_return 1;
        co_await read_at(tap, 65536, 4096);
        second_ok = co_await rec->start(
            out1, 300,
            [](const image::TraceRecorder::FinalizeResult&) {},
            second_err);
        third_ok = co_await rec->start(
            out2, 300,
            [](const image::TraceRecorder::FinalizeResult&) {}, third_err);
        res2 = co_await rec->stop("stopped");
        co_return 0;
    });
    REQUIRE(rc == 0);

    REQUIRE(res1.ok);
    REQUIRE(!second_ok);
    REQUIRE(!third_ok);
    REQUIRE(second_err.find("already in progress") != std::string::npos);
    REQUIRE(third_err.find("already in progress") != std::string::npos);
    // out1 survived both rejections byte-identically.
    REQUIRE(res1.sha256 == file_sha256(out1));
    const auto first = parse_file(out1);
    REQUIRE(first.size() == 1);
    REQUIRE(first[0] == format::trace::TraceRecord{'R', 0, 4096, 0});
    // The active window finalized its complete valid blob.
    REQUIRE(res2.ok);
    REQUIRE(res2.records == 1);
    REQUIRE(res2.sha256 == file_sha256(out2));
    const auto second = parse_file(out2);
    REQUIRE(second.size() == 1);
    REQUIRE(second[0] == format::trace::TraceRecord{'R', 0, 4096, 65536});
}

TEST_CASE("image: trace recording start race truncates the output exactly once",
          "[image]") {
    // Regression (PR #34 review): two concurrent starts on the SAME
    // path both passed the first gate and both opened O_TRUNC — the
    // loser's open wiped the winner's file behind its clean "already
    // in progress" rejection. Now the open carries no O_TRUNC and only
    // the state winner ftruncates, under the lock. The test-only start
    // hook holds start A between its open and the locked re-check so
    // start B deterministically wins; A must be rejected WITHOUT
    // touching the file, and B's finalize must produce a complete
    // valid blob (truncated exactly once, before the taps armed).
    test::TempDir dir;
    const auto data = test::pattern_bytes(512 * 1024, 97);
    const std::string out = dir / "race.trace";
    // A pre-existing file stand-in (e.g. a previous blob).
    {
        const int fd = ::open(out.c_str(), O_WRONLY | O_CREAT, 0644);
        REQUIRE(fd >= 0);
        REQUIRE(::write(fd, data.data(), 4096) == 4096);
        ::close(fd);
    }
    std::atomic<bool> a_in_hook{false};
    std::atomic<bool> release{false};
    std::optional<bool> a_ok;
    std::string a_err, b_err;
    bool b_ok = false;
    image::TraceRecorder::FinalizeResult res;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        image::TraceRecordSource tap(
            std::make_unique<test::VectorSource>(data), rec, 0);
        // The hook is shared by every start on this recorder: fire it
        // once (A parks; B must pass straight through or both deadlock).
        std::atomic<bool> hook_used{false};
        rec->set_start_hook_for_test([&]() -> elio::coro::task<void> {
            if (hook_used.exchange(true, std::memory_order_acq_rel)) {
                co_return;
            }
            a_in_hook.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                co_await elio::time::sleep_for(1ms);
            }
        });
        auto cb = [](const image::TraceRecorder::FinalizeResult&) {};
        elio::go([&]() -> elio::coro::task<void> {
            a_ok = co_await rec->start(out, 300, cb, a_err);
        });
        for (int i = 0;
             i < 5000 && !a_in_hook.load(std::memory_order_acquire); ++i) {
            co_await elio::time::sleep_for(1ms);
        }
        if (!a_in_hook.load(std::memory_order_acquire)) co_return 2;
        // B races A while A is parked between open and lock: B wins.
        b_ok = co_await rec->start(out, 300, cb, b_err);
        release.store(true, std::memory_order_release);
        for (int i = 0; i < 5000 && !a_ok.has_value(); ++i) {
            co_await elio::time::sleep_for(1ms);
        }
        rec->set_start_hook_for_test(nullptr);
        if (!a_ok.has_value()) co_return 3;
        co_await read_at(tap, 0, 4096);
        co_await read_at(tap, 65536, 8192);
        res = co_await rec->stop("stopped");
        co_return 0;
    });
    REQUIRE(rc == 0);

    REQUIRE(b_ok);                       // B won the race
    REQUIRE(a_ok.has_value());
    REQUIRE(!a_ok.value());              // A rejected cleanly
    REQUIRE(a_err.find("already in progress") != std::string::npos);
    // Exactly one truncation, before arming: the winner's blob is
    // complete and the pre-existing bytes are gone.
    REQUIRE(res.ok);
    REQUIRE(res.records == 2);
    REQUIRE(res.sha256 == file_sha256(out));
    const auto records = parse_file(out);
    REQUIRE(records.size() == 2);
    REQUIRE(records[0] == format::trace::TraceRecord{'R', 0, 4096, 0});
    REQUIRE(records[1] == format::trace::TraceRecord{'R', 0, 8192, 65536});
}

TEST_CASE("image: trace recording drops out-of-range offsets instead of corrupting",
          "[image]") {
    // The wire record's offset is int64_t: an offset past INT64_MAX
    // would silently cast into a negative blob offset. record() drops
    // + counts such a range instead (same policy as buffer overflow).
    test::TempDir dir;
    const auto data = test::pattern_bytes(512 * 1024, 99);
    const std::string out = dir / "out.trace";
    image::TraceRecorder::FinalizeResult res;

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto rec = std::make_shared<image::TraceRecorder>();
        image::TraceRecordSource tap(
            std::make_unique<test::VectorSource>(data), rec, 0);
        res = co_await with_recording(
            *rec, out, [&]() -> elio::coro::task<void> {
                // Direct bad-caller injection (the tap's offsets come
                // from real blob sizes and can never reach this).
                rec->record(0, static_cast<uint64_t>(INT64_MAX) + 1,
                            4096);
                rec->record(0, static_cast<uint64_t>(INT64_MAX) - 100,
                            4096);  // offset+count overflows int64 too
                co_await read_at(tap, 0, 4096);  // legitimate read
            });
        co_return 0;
    });
    REQUIRE(rc == 0);

    REQUIRE(res.ok);
    REQUIRE(res.records == 1);   // only the legitimate read
    REQUIRE(res.dropped == 2);   // both out-of-range calls counted
    const auto records = parse_file(out);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0] == format::trace::TraceRecord{'R', 0, 4096, 0});
}
