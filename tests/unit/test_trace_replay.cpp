// Unit tests: trace replay (ADR-0013, proposed) — populate translation in
// recorded order, upstream-parity skip rules, replay budgets, and local
// trace-layer assembly. Trace blobs are built with the spec-pinned codec
// (src/format/trace.hpp; its golden bytes are pinned in test_trace.cpp).
#include "image/image_file.hpp"
#include "image/trace_replay.hpp"

#include "common/bytes.hpp"
#include "common/crc32c.hpp"
#include "format/trace.hpp"
#include "format/writer.hpp"

#include "../support.hpp"

#include <elio/time/timer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <fcntl.h>

#include <filesystem>

using namespace obd;
using obd::test::TempDir;

namespace {

/// A BlobSource that records populate() calls (order, offset, count) and
/// can inject per-call delay or failure.
class PopulateRecorder final : public source::BlobSource {
public:
    std::vector<std::pair<uint64_t, size_t>> calls;
    ssize_t populate_result = 0;              // <0 injects a failure
    std::chrono::milliseconds populate_delay{0};

    elio::coro::task<ssize_t> pread(void*, size_t, uint64_t) override {
        co_return 0;
    }
    elio::coro::task<ssize_t> populate(uint64_t offset,
                                       size_t len) override {
        if (populate_delay.count() > 0) {
            co_await elio::time::sleep_for(populate_delay);
        }
        calls.emplace_back(offset, len);
        co_return populate_result;
    }
    uint64_t size() const noexcept override { return uint64_t{1} << 40; }
    std::string_view label() const noexcept override { return "recorder"; }
};

// Hand-serializes one record (zero padding) — needed for records the
// conforming writer refuses to emit ('W' ops, oversized counts), which
// the parser accepts and the REPLAY must skip (trace-format.md §7/§8).
void put_record(std::vector<uint8_t>& out, char op, uint32_t layer_index,
                uint64_t count, int64_t offset) {
    const size_t at = out.size();
    out.resize(at + format::trace::kRecordSize, 0);
    uint8_t* p = out.data() + at;
    p[0] = static_cast<uint8_t>(op);
    bytes::store_u32_le(p + 4, layer_index);
    bytes::store_u64_le(p + 8, count);
    bytes::store_u64_le(p + 16, static_cast<uint64_t>(offset));
}

std::vector<uint8_t> make_blob(const std::vector<uint8_t>& records) {
    std::vector<uint8_t> blob(format::trace::kHeaderSize, 0);
    bytes::store_u32_le(blob.data(), format::trace::kMagic);
    bytes::store_u64_le(blob.data() + 8, records.size());
    bytes::store_u32_le(
        blob.data() + 16,
        crc32::crc32c_extend(records.data(), records.size(), /*crc=*/0));
    blob.insert(blob.end(), records.begin(), records.end());
    return blob;
}

/// Valid trace blob via the conforming writer.
std::vector<uint8_t> writer_blob(
    const std::vector<format::trace::TraceRecord>& records) {
    format::trace::TraceWriter w;
    for (const auto& rec : records) REQUIRE(w.append(rec));
    const auto blob = w.finalize();
    return {blob.begin(), blob.end()};
}

}  // namespace

TEST_CASE("image: trace replay populates traced extents in recorded order",
          "[image]") {
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        PopulateRecorder a, b;
        // Interleaved layers; replay must preserve the trace's order and
        // address the right target per record.
        const auto blob = writer_blob({
            {'R', 1, 4096, 8192},
            {'R', 0, 1024, 0},
            {'R', 1, 2048, 16384},
        });
        // Named target list: GCC 12 rejects a brace-init temporary as a
        // coroutine argument ("array used as initializer").
        std::vector<source::BlobSource*> targets{&a, &b};
        const auto stats = co_await image::replay_trace(blob, targets);
        REQUIRE(stats.trace_present);
        REQUIRE(stats.records_total == 3);
        REQUIRE(stats.records_replayed == 3);
        REQUIRE(stats.records_skipped == 0);
        REQUIRE(stats.bytes_warmed == 4096 + 1024 + 2048);
        REQUIRE(!stats.budget_exhausted);
        // Layer 1's records in recorded order; layer 0's between them.
        REQUIRE(b.calls.size() == 2);
        REQUIRE((b.calls[0] == std::pair<uint64_t, size_t>{8192, 4096}));
        REQUIRE((b.calls[1] == std::pair<uint64_t, size_t>{16384, 2048}));
        REQUIRE(a.calls.size() == 1);
        REQUIRE((a.calls[0] == std::pair<uint64_t, size_t>{0, 1024}));
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("image: trace replay skips unknown ops, layers and bad records",
          "[image]") {
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        PopulateRecorder a;
        std::vector<uint8_t> records;
        put_record(records, 'R', 0, 4096, 0);          // replayed
        put_record(records, 'W', 0, 4096, 4096);       // op not READ: skip
        put_record(records, '\x7F', 0, 4096, 0);       // unknown op: skip
        put_record(records, 'R', 7, 4096, 0);          // unknown layer: skip
        put_record(records, 'R', 0, 0, 0);             // count 0: skip
        put_record(records, 'R', 0, 1048577, 0);       // > 1 MiB: skip
        put_record(records, 'R', 0, 4096, -1);         // negative offset
        put_record(records, 'R', 0, 8192, 1048576);    // replayed
        std::vector<source::BlobSource*> targets{&a, nullptr};
        const auto stats =
            co_await image::replay_trace(make_blob(records), targets);
        REQUIRE(stats.trace_present);
        REQUIRE(stats.records_total == 8);
        REQUIRE(stats.records_replayed == 2);
        REQUIRE(stats.records_skipped == 6);
        // Skipped records issue no populate; order preserved.
        REQUIRE(a.calls.size() == 2);
        REQUIRE((a.calls[0] == std::pair<uint64_t, size_t>{0, 4096}));
        REQUIRE((a.calls[1] == std::pair<uint64_t, size_t>{1048576, 8192}));

        // A failing populate is logged and skipped, never fatal.
        PopulateRecorder failing;
        failing.populate_result = -EIO;
        const auto blob = writer_blob({{'R', 0, 4096, 0}, {'R', 0, 4096, 4096}});
        std::vector<source::BlobSource*> failing_targets{&failing};
        const auto s2 =
            co_await image::replay_trace(blob, failing_targets);
        REQUIRE(s2.trace_present);
        REQUIRE(s2.records_replayed == 0);
        REQUIRE(s2.records_skipped == 2);

        // A malformed blob degrades to "no prefetch", never an error.
        const std::vector<uint8_t> garbage{0xDE, 0xAD, 0xBE, 0xEF};
        std::vector<source::BlobSource*> one{&a};
        const auto s3 = co_await image::replay_trace(garbage, one);
        REQUIRE(!s3.trace_present);
        REQUIRE(s3.records_replayed == 0);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("image: trace replay enforces record, byte and time budgets",
          "[image]") {
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        // Record cap: processing stops at max_records.
        {
            PopulateRecorder a;
            std::vector<format::trace::TraceRecord> recs(
                10, {'R', 0, 4096, 0});
            image::TraceReplayOptions opts;
            opts.max_records = 3;
            std::vector<source::BlobSource*> targets{&a};
            const auto stats = co_await image::replay_trace(
                writer_blob(recs), targets, opts);
            REQUIRE(stats.records_replayed == 3);
            REQUIRE(stats.budget_exhausted);
        }
        // Byte cap: warm-up stops once max_bytes is reached.
        {
            PopulateRecorder a;
            std::vector<format::trace::TraceRecord> recs(
                10, {'R', 0, 512 * 1024, 0});
            image::TraceReplayOptions opts;
            opts.max_bytes = 1024 * 1024;
            std::vector<source::BlobSource*> targets{&a};
            const auto stats = co_await image::replay_trace(
                writer_blob(recs), targets, opts);
            REQUIRE(stats.records_replayed == 2);
            REQUIRE(stats.bytes_warmed == 1024 * 1024);
            REQUIRE(stats.budget_exhausted);
        }
        // Wall-time budget: with each populate sleeping ~250 ms and a
        // 550 ms budget, replay stops well before the 10-record queue
        // drains (sleep overshoot only makes the stop earlier-or-equal
        // per check, never later than one extra record).
        {
            PopulateRecorder a;
            a.populate_delay = std::chrono::milliseconds(250);
            std::vector<format::trace::TraceRecord> recs(
                10, {'R', 0, 4096, 0});
            image::TraceReplayOptions opts;
            opts.max_wall_time = std::chrono::milliseconds(550);
            std::vector<source::BlobSource*> targets{&a};
            const auto stats = co_await image::replay_trace(
                writer_blob(recs), targets, opts);
            REQUIRE(stats.records_replayed >= 1);
            REQUIRE(stats.records_replayed < 10);
            REQUIRE(stats.budget_exhausted);
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("image: local trace layer is set aside and replayed at open",
          "[image]") {
    TempDir dir;
    const auto bottom_raw = test::pattern_bytes(512 * 32, 31);
    const auto top_raw = test::pattern_bytes(512 * 32, 33);
    const std::string l1 = dir / "bottom.lsmt";
    const std::string l2 = dir / "top.lsmt";
    {
        const std::string r1 = test::write_file(dir / "b.img", bottom_raw);
        const int fd = ::open(r1.c_str(), O_RDONLY);
        REQUIRE(fd >= 0);
        format::write_lsmt_single_layer(fd, bottom_raw.size(), l1, {});
        ::close(fd);
        const std::string r2 = test::write_file(dir / "t.img", top_raw);
        const int fd2 = ::open(r2.c_str(), O_RDONLY);
        REQUIRE(fd2 >= 0);
        format::write_lsmt_single_layer(fd2, top_raw.size(), l2, {});
        ::close(fd2);
    }
    // The acceleration layer: a directory whose `trace` member is a valid
    // trace blob touching both data lowers (the upstream lookup name,
    // trace-format.md §6 step 4).
    const std::string accel_dir = dir / "accel";
    {
        REQUIRE(std::filesystem::create_directories(accel_dir));
        const auto blob = writer_blob({
            {'R', 0, 4096, 0},
            {'R', 1, 4096, 8192},
        });
        test::write_file(accel_dir + "/trace", blob);
    }
    nlohmann::json cfgj;
    cfgj["repoBlobUrl"] = "";
    cfgj["accelerationLayer"] = true;
    cfgj["lowers"] = nlohmann::json::array(
        {nlohmann::json{{"digest", "sha256:b"}, {"file", l1}},
         nlohmann::json{{"digest", "sha256:t"}, {"file", l2}},
         nlohmann::json{{"digest", "sha256:a"}, {"dir", accel_dir}}});
    const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});
    REQUIRE(cfg.acceleration_layer);
    REQUIRE(cfg.lowers.size() == 3);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        const image::GlobalConfig global;
        auto opened = co_await image::open_image(cfg, global);
        // The trace layer is not a data layer: excluded from the merge.
        REQUIRE(opened.layer_count == 2);
        REQUIRE(opened.virtual_size == top_raw.size());
        // The trace was recognized and replayed in full (populate on the
        // local sources is a no-op success, so every record replays).
        REQUIRE(opened.trace.trace_present);
        REQUIRE(opened.trace.records_total == 2);
        REQUIRE(opened.trace.records_replayed == 2);
        // Device reads never see trace bytes: the merged view is exactly
        // the two data layers.
        std::vector<uint8_t> buf(top_raw.size());
        const ssize_t r = co_await opened.root->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(top_raw.size()));
        REQUIRE(buf == top_raw);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("image: garbage trace layer never fails assembly", "[image]") {
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 32, 41);
    const std::string l1 = dir / "layer.lsmt";
    {
        const std::string r1 = test::write_file(dir / "r.img", raw);
        const int fd = ::open(r1.c_str(), O_RDONLY);
        REQUIRE(fd >= 0);
        format::write_lsmt_single_layer(fd, raw.size(), l1, {});
        ::close(fd);
    }
    const std::string accel_dir = dir / "accel";
    REQUIRE(std::filesystem::create_directories(accel_dir));
    // Garbage where the trace blob should be: bad magic, no valid header.
    test::write_file(accel_dir + "/trace", test::pattern_bytes(4096, 7));

    nlohmann::json cfgj;
    cfgj["repoBlobUrl"] = "";
    cfgj["accelerationLayer"] = true;
    cfgj["lowers"] = nlohmann::json::array(
        {nlohmann::json{{"digest", "sha256:b"}, {"file", l1}},
         nlohmann::json{{"digest", "sha256:a"}, {"dir", accel_dir}}});
    const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        const image::GlobalConfig global;
        // Replay is opportunistic: the device comes up normally.
        auto opened = co_await image::open_image(cfg, global);
        REQUIRE(opened.layer_count == 1);
        REQUIRE(opened.virtual_size == raw.size());
        REQUIRE(!opened.trace.trace_present);
        REQUIRE(opened.trace.records_replayed == 0);
        std::vector<uint8_t> buf(raw.size());
        const ssize_t r = co_await opened.root->pread(buf.data(), buf.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(raw.size()));
        REQUIRE(buf == raw);
        co_return 0;
    });
    REQUIRE(rc == 0);
}
