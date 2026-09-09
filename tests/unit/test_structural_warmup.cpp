// Unit tests: structural head/tail warm-up (ADR-0012's cold-start
// floor) — window computation (clamping, merging, disabling), the
// opportunistic populate driver, the `prefetch` config surface, and the
// `prefetch.enable` bring-up gate. See docs/testing.md.
#include "image/image_file.hpp"
#include "image/structural_warmup.hpp"

#include "common/errors.hpp"
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

/// A BlobSource of fixed size that records populate() calls (offset,
/// len) and can inject per-call delay, failure, or a throw.
class PopulateRecorder final : public source::BlobSource {
public:
    explicit PopulateRecorder(uint64_t size) : size_(size) {}

    std::vector<std::pair<uint64_t, size_t>> calls;
    ssize_t populate_result = 0;              // <0 injects a failure
    bool populate_throws = false;             // throws instead of -errno
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
        if (populate_throws) throw obd::error(EIO, "injected populate throw");
        co_return populate_result;
    }
    uint64_t size() const noexcept override { return size_; }
    std::string_view label() const noexcept override { return "recorder"; }

private:
    uint64_t size_;
};

}  // namespace

TEST_CASE("image: structural warm-up windows clamp and merge on small blobs",
          "[image]") {
    constexpr uint64_t MiB = uint64_t{1} << 20;

    // Normal case: two disjoint windows at the exact edges.
    {
        const auto w = image::structural_windows(4 * MiB, MiB, MiB);
        REQUIRE(w.size() == 2);
        REQUIRE(w[0].offset == 0);
        REQUIRE(w[0].len == MiB);
        REQUIRE(w[1].offset == 3 * MiB);
        REQUIRE(w[1].len == MiB);
    }
    // Blob smaller than head+tail: one merged full window — the overlap
    // region must not be populated twice.
    {
        const auto w = image::structural_windows(MiB + MiB / 2, MiB, MiB);
        REQUIRE(w.size() == 1);
        REQUIRE(w[0].offset == 0);
        REQUIRE(w[0].len == MiB + MiB / 2);
    }
    // head+tail exactly covering the blob also merges (adjacency).
    {
        const auto w = image::structural_windows(2 * MiB, MiB, MiB);
        REQUIRE(w.size() == 1);
        REQUIRE(w[0].offset == 0);
        REQUIRE(w[0].len == 2 * MiB);
    }
    // Each side clamped alone: head/tail larger than the whole blob.
    {
        const auto w = image::structural_windows(MiB / 2, MiB, 0);
        REQUIRE(w.size() == 1);
        REQUIRE(w[0].offset == 0);
        REQUIRE(w[0].len == MiB / 2);
        const auto t = image::structural_windows(MiB / 2, 0, MiB);
        REQUIRE(t.size() == 1);
        REQUIRE(t[0].offset == 0);
        REQUIRE(t[0].len == MiB / 2);
    }
    // 0 disables its side; both 0 disables warm-up entirely.
    {
        const auto w = image::structural_windows(4 * MiB, 0, MiB);
        REQUIRE(w.size() == 1);
        REQUIRE(w[0].offset == 3 * MiB);
        REQUIRE(w[0].len == MiB);
        REQUIRE(image::structural_windows(4 * MiB, MiB, 0).size() == 1);
        REQUIRE(image::structural_windows(4 * MiB, 0, 0).empty());
    }
    // An empty blob has no windows (and never errors).
    REQUIRE(image::structural_windows(0, MiB, MiB).empty());
    REQUIRE(image::structural_windows(0, 0, 0).empty());
}

TEST_CASE("image: structural warm-up populates head and tail windows opportunistically",
          "[image]") {
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        constexpr uint64_t MiB = uint64_t{1} << 20;
        // Two layers: head window before tail window per layer, layers in
        // order; a nullptr target is skipped without failing the pass.
        {
            PopulateRecorder a(4 * MiB), b(2 * MiB);
            std::vector<source::BlobSource*> targets{&a, nullptr, &b};
            const auto stats = co_await image::warmup_structural(targets);
            REQUIRE(stats.layers_total == 2);
            REQUIRE(stats.layers_warmed == 2);
            REQUIRE(stats.windows_populated == 3);
            REQUIRE(stats.windows_failed == 0);
            REQUIRE(stats.bytes_warmed == 4 * MiB);
            REQUIRE(!stats.budget_exhausted);
            REQUIRE(a.calls.size() == 2);
            REQUIRE((a.calls[0] == std::pair<uint64_t, size_t>{0, MiB}));
            REQUIRE((a.calls[1] ==
                     std::pair<uint64_t, size_t>{3 * MiB, MiB}));
            // The small blob merges into one full window.
            REQUIRE(b.calls.size() == 1);
            REQUIRE((b.calls[0] ==
                     std::pair<uint64_t, size_t>{0, 2 * MiB}));
        }
        // A failing populate (-errno) and a throwing one are logged and
        // skipped; the remaining windows and layers still warm, and the
        // pass never propagates the error.
        {
            PopulateRecorder failing(4 * MiB), throwing(4 * MiB), ok(4 * MiB);
            failing.populate_result = -EIO;
            throwing.populate_throws = true;
            std::vector<source::BlobSource*> targets{&failing, &throwing,
                                                     &ok};
            const auto stats = co_await image::warmup_structural(targets);
            REQUIRE(stats.windows_failed == 4);
            REQUIRE(stats.windows_populated == 2);
            REQUIRE(stats.layers_warmed == 1);
            REQUIRE(ok.calls.size() == 2);
        }
        // Both window sizes 0: no populate traffic at all.
        {
            PopulateRecorder a(4 * MiB);
            image::StructuralWarmupOptions opts;
            opts.head_bytes = 0;
            opts.tail_bytes = 0;
            std::vector<source::BlobSource*> targets{&a};
            const auto stats = co_await image::warmup_structural(targets,
                                                                 opts);
            REQUIRE(stats.windows_populated == 0);
            REQUIRE(a.calls.empty());
        }
        // Wall-time budget: with each populate sleeping ~250 ms and a
        // 550 ms budget, warm-up stops before the 8-window queue drains
        // (same proof shape as the trace replay budget test).
        {
            PopulateRecorder a(8 * MiB), b(8 * MiB), c(8 * MiB), d(8 * MiB);
            a.populate_delay = std::chrono::milliseconds(250);
            b.populate_delay = std::chrono::milliseconds(250);
            c.populate_delay = std::chrono::milliseconds(250);
            d.populate_delay = std::chrono::milliseconds(250);
            image::StructuralWarmupOptions opts;
            opts.max_wall_time = std::chrono::milliseconds(550);
            std::vector<source::BlobSource*> targets{&a, &b, &c, &d};
            const auto stats = co_await image::warmup_structural(targets,
                                                                 opts);
            REQUIRE(stats.windows_populated >= 1);
            REQUIRE(stats.windows_populated < 8);
            REQUIRE(stats.budget_exhausted);
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("image: prefetch config parses structural window knobs",
          "[image]") {
    // Defaults: absent section -> enabled, 1 MiB windows each side.
    {
        const auto g = image::GlobalConfig::from_json_text("{}");
        REQUIRE(g.prefetch_enable);
        REQUIRE(g.prefetch_head_kb == 1024);
        REQUIRE(g.prefetch_tail_kb == 1024);
    }
    // The honored subset parses; a 0 window size is kept (it disables
    // that side at bring-up).
    {
        const auto g = image::GlobalConfig::from_json_text(
            R"({"prefetch": {"enable": false, "head_kb": 0,
                              "tail_kb": 256}})");
        REQUIRE(!g.prefetch_enable);
        REQUIRE(g.prefetch_head_kb == 0);
        REQUIRE(g.prefetch_tail_kb == 256);
    }
    // Partial sections keep the documented defaults field by field.
    {
        const auto g = image::GlobalConfig::from_json_text(
            R"({"prefetch": {"head_kb": 2048}})");
        REQUIRE(g.prefetch_enable);
        REQUIRE(g.prefetch_head_kb == 2048);
        REQUIRE(g.prefetch_tail_kb == 1024);
    }
    // Out-of-range window sizes are rejected fail-loud: a negative value
    // would otherwise wrap to ~4 TiB through the uint32 conversion and
    // silently warm every layer whole at each bring-up.
    REQUIRE_THROWS(image::GlobalConfig::from_json_text(
        R"({"prefetch": {"head_kb": -1}})"));
    REQUIRE_THROWS(image::GlobalConfig::from_json_text(
        R"({"prefetch": {"tail_kb": -1024}})"));
    REQUIRE_THROWS(image::GlobalConfig::from_json_text(
        R"({"prefetch": {"tail_kb": 4294967296}})"));
    // The range edges are accepted (4294967295 = uint32 max).
    {
        const auto g = image::GlobalConfig::from_json_text(
            R"({"prefetch": {"head_kb": 4294967295}})");
        REQUIRE(g.prefetch_head_kb == 4294967295u);
    }
}

TEST_CASE("image: prefetch enable false skips structural warm-up",
          "[image]") {
    // The global `prefetch.enable` switch (ADR-0012) is the master gate
    // for BOTH warm-up kinds: with it false the structural head/tail
    // windows are not populated (stats zero) — the device still comes up
    // fully functional (same proof pattern as the trace-replay gate).
    TempDir dir;
    const auto raw = test::pattern_bytes(512 * 32, 43);
    const std::string l1 = dir / "layer.lsmt";
    {
        const std::string r1 = test::write_file(dir / "r.img", raw);
        const int fd = ::open(r1.c_str(), O_RDONLY);
        REQUIRE(fd >= 0);
        format::write_lsmt_single_layer(fd, raw.size(), l1, {});
        ::close(fd);
    }
    nlohmann::json cfgj;
    cfgj["lowers"] = nlohmann::json::array(
        {nlohmann::json{{"digest", "sha256:b"}, {"file", l1}}});
    const auto cfg = image::ImageConfig::from_json_text(cfgj.dump(), {});

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        // Enabled (default): the small local blob merges into one full
        // window, populated through the (no-op local) chain.
        {
            const image::GlobalConfig global;
            auto opened = co_await image::open_image(cfg, global);
            REQUIRE(opened.warmup.layers_total == 1);
            REQUIRE(opened.warmup.layers_warmed == 1);
            REQUIRE(opened.warmup.windows_populated == 1);
            REQUIRE(opened.warmup.windows_failed == 0);
            std::vector<uint8_t> buf(raw.size());
            const ssize_t r =
                co_await opened.root->pread(buf.data(), buf.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(raw.size()));
            REQUIRE(buf == raw);
        }
        // Disabled: no warm-up traffic, device fully functional.
        {
            image::GlobalConfig global;
            global.prefetch_enable = false;
            auto opened = co_await image::open_image(cfg, global);
            REQUIRE(opened.warmup.layers_total == 0);
            REQUIRE(opened.warmup.windows_populated == 0);
            REQUIRE(opened.warmup.bytes_warmed == 0);
            std::vector<uint8_t> buf(raw.size());
            const ssize_t r =
                co_await opened.root->pread(buf.data(), buf.size(), 0);
            REQUIRE(r == static_cast<ssize_t>(raw.size()));
            REQUIRE(buf == raw);
        }
        co_return 0;
    });
    REQUIRE(rc == 0);
}
