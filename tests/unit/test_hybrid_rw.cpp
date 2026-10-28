#include "format/hybrid_rw.hpp"
#include "common/sha256.hpp"
#include <sys/stat.h>
#include "format/merged_writable.hpp"
#include "source/local_file.hpp"
#include "../support.hpp"
#include <elio/coro/task_group.hpp>
#include <elio/runtime/spawn.hpp>
#include <array>
#include <elio/runtime/spawn_blocking.hpp>
#include <elio/sync/event.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace obd;
namespace {
std::vector<uint8_t> read_fixture(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
// Hold an actual Hybrid physical read before IO. Count entries into the actual
// upper write, distinguishing a queued merged mutation from a merely slow IO.
class HeldHybrid final : public format::WritableLayer {
public:
    explicit HeldHybrid(std::unique_ptr<format::HybridRwLayer> upper)
        : upper_(std::move(upper)), view(upper_->data_source()) {}
    class HeldView final : public source::BlobSource {
    public:
        explicit HeldView(source::BlobSource& source) : source_(source) {}
        elio::coro::task<ssize_t> pread(void* b, size_t n, uint64_t o) override {
            ++entries;
            entered.set();
            co_await release.wait();
            if (fail) throw std::runtime_error("held read failure");
            co_return co_await source_.pread(b, n, o);
        }
        uint64_t size() const noexcept override { return source_.size(); }
        std::string_view label() const noexcept override { return "held-hybrid"; }
        elio::sync::event entered, release;
        unsigned entries = 0; // Single-worker directed tests.
        bool fail = false;
    private:
        source::BlobSource& source_;
    };
    elio::coro::task<ssize_t> pread(void* b, size_t n, uint64_t o) override {
        co_return co_await upper_->pread(b, n, o);
    }
    elio::coro::task<ssize_t> pwrite(const void* b, size_t n, uint64_t o) override {
        ++mutation_entries;
        co_return co_await upper_->pwrite(b, n, o);
    }
    elio::coro::task<int> discard(uint64_t o, uint64_t n) override {
        ++mutation_entries;
        co_return co_await upper_->discard(o, n);
    }
    elio::coro::task<int> flush() override { co_return co_await upper_->flush(); }
    elio::coro::task<int> checkpoint() override { co_return co_await upper_->checkpoint(); }
    int grow(uint64_t n) override { return upper_->grow(n); }
    uint64_t virtual_size() const override { return upper_->virtual_size(); }
    const std::vector<bytes::segment_mapping>& segments() const override { return upper_->segments(); }
    std::vector<bytes::segment_mapping> segments_snapshot() const override { return upper_->segments_snapshot(); }
    source::BlobSource& data_source() override { return view; }
    bool serialize_merged_reads() const noexcept override { return upper_->serialize_merged_reads(); }
private:
    std::unique_ptr<format::HybridRwLayer> upper_;
public:
    HeldView view;
    unsigned mutation_entries = 0; // Single-worker test below.
};
}

TEST_CASE("format: hybrid writable adapter preserves merged lower and ZERO ownership",
          "[format][hybrid]") {
    const auto fixtures = std::filesystem::path(__FILE__).parent_path().parent_path() /
                          "fixtures/hybrid/overlaybd";
    test::TempDir temporary;
    const auto directory = temporary / "pair";
    auto expected = read_fixture(fixtures / "expected.raw");
    const auto lower_path = fixtures / "sealed.data";
    const auto lower_before = read_fixture(lower_path);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto lower_source = co_await source::LocalFileSource::open(lower_path.string());
        auto lower = co_await format::LsmtLayer::open(std::move(lower_source));
        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
        layers.push_back(std::move(lower));
        auto upper = co_await format::HybridRwLayer::create(directory, expected.size());
        auto merged = co_await format::MergedWritable::open(std::move(layers), std::move(upper));
        std::vector<uint8_t> bytes(512, 0x51), actual(expected.size());
        const auto first = co_await merged->pread(actual.data(), actual.size(), 0);
        REQUIRE(first == static_cast<ssize_t>(expected.size()));
        REQUIRE(actual == expected);
        const int discarded = co_await merged->discard(512, 512);
        REQUIRE(discarded == 0);
        std::fill_n(expected.begin() + 512, 512, 0);
        const auto wrote = co_await merged->pwrite(bytes.data(), bytes.size(), 2 * 512);
        REQUIRE(wrote == 512);
        std::fill_n(expected.begin() + 2 * 512, 512, 0x51);
        const auto changed = co_await merged->pread(actual.data(), actual.size(), 0);
        REQUIRE(changed == static_cast<ssize_t>(expected.size()));
        REQUIRE(actual == expected);
        auto* active = merged.get();
        const int grown = co_await elio::spawn_blocking([active] { return active->grow(65536); });
        REQUIRE(grown == 0);
        expected.resize(65536, 0);
        const auto tail = co_await merged->pwrite(bytes.data(), bytes.size(), 65536 - 512);
        REQUIRE(tail == 512);
        std::fill_n(expected.end() - 512, 512, 0x51);
        const int checkpoint = co_await merged->writable_top().checkpoint();
        REQUIRE(checkpoint == 0);
        merged.reset();
        lower_source = co_await source::LocalFileSource::open(lower_path.string());
        lower = co_await format::LsmtLayer::open(std::move(lower_source));
        layers.push_back(std::move(lower));
        upper = co_await format::HybridRwLayer::open_existing(directory + "/data", directory + "/index");
        merged = co_await format::MergedWritable::open(std::move(layers), std::move(upper));
        actual.resize(expected.size());
        const auto reopened = co_await merged->pread(actual.data(), actual.size(), 0);
        REQUIRE(reopened == static_cast<ssize_t>(expected.size()));
        REQUIRE(actual == expected);
        REQUIRE(merged->size() == 65536);
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(read_fixture(lower_path) == lower_before);
}

TEST_CASE("format: hybrid merged read holds mutation and grow through physical IO",
          "[format][hybrid]") {
    for (const std::string mode : {"overwrite", "discard", "zero-to-data"}) {
    DYNAMIC_SECTION(mode) {
    test::TempDir temporary;
    elio::run_config config;
    config.num_threads = 1;
    config.blocking_threads = 1;
    const int rc = elio::run([&]() -> elio::coro::task<int> {
        auto upper = co_await format::HybridRwLayer::create(temporary / "pair", 32768);
        const size_t read_size = mode == "zero-to-data" ? 1024 : 512;
        std::vector<uint8_t> old(read_size, 0x11), replacement(512, 0x22), actual(read_size);
        const auto wrote = co_await upper->pwrite(old.data(), old.size(), 512);
        REQUIRE(wrote == static_cast<ssize_t>(read_size));
        if (mode == "zero-to-data") {
            const int zeroed = co_await upper->discard(512, 512);
            REQUIRE(zeroed == 0);
            std::fill_n(old.begin(), 512, 0);
        }
        auto expected_after = old;
        std::fill_n(expected_after.begin(), 512, mode == "discard" ? 0 : 0x22);
        auto held = std::make_unique<HeldHybrid>(std::move(upper));
        auto* observed = held.get();
        auto merged = co_await format::MergedWritable::open({}, std::move(held));
        elio::sync::event attempted;
        elio::coro::task_group tasks;
        ssize_t read_result = 0, write_result = 0;
        int grow_result = 0;
        unsigned entries_while_held = 99;
        std::exception_ptr failure;
        tasks.spawn([&]() -> elio::coro::task<void> {
            read_result = co_await merged->pread(actual.data(), actual.size(), 512);
        });
        try {
            co_await observed->view.entered.wait();
            auto* active = merged.get();
            grow_result = co_await elio::spawn_blocking([active] { return active->grow(65536); });
            tasks.spawn([&]() -> elio::coro::task<void> {
                attempted.set();
                if (mode == "discard") write_result = co_await merged->discard(512, 512);
                else write_result = co_await merged->pwrite(replacement.data(), replacement.size(), 512);
            });
            co_await attempted.wait();
            entries_while_held = observed->mutation_entries;
        } catch (...) { failure = std::current_exception(); }
        observed->view.release.set();
        co_await tasks.join();
        if (failure) std::rethrow_exception(failure);
        REQUIRE(grow_result == -EBUSY);
        REQUIRE(entries_while_held == 0);
        REQUIRE(read_result == static_cast<ssize_t>(read_size));
        REQUIRE(actual == old);
        REQUIRE(write_result == (mode == "discard" ? 0 : 512));
        const auto after = co_await merged->pread(actual.data(), actual.size(), 512);
        REQUIRE(after == static_cast<ssize_t>(read_size));
        REQUIRE(actual == expected_after);
        co_return 0;
    }, config);
    REQUIRE(rc == 0);
    }
    }
}

TEST_CASE("format: hybrid offline seal validates overrides and reports exact artifact digest",
          "[format][hybrid]") {
    const auto fixtures = std::filesystem::path(__FILE__).parent_path().parent_path() /
                          "fixtures/hybrid";
    for (const auto* producer : {"overlaybd", "agentenv"}) {
        DYNAMIC_SECTION(producer) {
            test::TempDir temporary;
            const auto data = temporary / "data", index = temporary / "index";
            std::filesystem::copy_file(fixtures / producer / "writable.data", data);
            std::filesystem::copy_file(fixtures / producer / "writable.index", index);
            const auto original_data = read_fixture(data), original_index = read_fixture(index);
            auto expected = read_fixture(fixtures / producer / "expected.raw");
            expected.resize(65536, 0);
            struct stat before{};
            REQUIRE(::stat(data.c_str(), &before) == 0);
            std::string digest = "untouched", reject;
            uint64_t size = 123;
            const int rc = test::run_coro([&]() -> elio::coro::task<int> {
                for (const uint64_t invalid : {uint64_t{513}, uint64_t{16384}}) {
                    const int refused = co_await format::HybridRwLayer::seal_file(
                        data, index, "", &digest, &size, invalid, &reject);
                    REQUIRE(refused == -EINVAL);
                    REQUIRE_FALSE(reject.empty());
                    REQUIRE(digest == "untouched");
                    REQUIRE(size == 123);
                    REQUIRE(read_fixture(data) == original_data);
                    REQUIRE(read_fixture(index) == original_index);
                }
                const int long_tag = co_await format::HybridRwLayer::seal_file(
                    data, index, std::string(256, 'x'), &digest, &size, 0, &reject);
                REQUIRE(long_tag == -EINVAL);
                REQUIRE(read_fixture(data) == original_data);
                REQUIRE(read_fixture(index) == original_index);
                auto owner = co_await format::HybridRwLayer::open_existing(data, index);
                const int busy = co_await format::HybridRwLayer::seal_file(
                    data, index, "", &digest, &size);
                REQUIRE(busy == -EWOULDBLOCK);
                owner.reset();
                const int sealed = co_await format::HybridRwLayer::seal_file(
                    data, index, "offline test", &digest, &size, 65536, &reject);
                REQUIRE(sealed == 0);
                REQUIRE(reject.empty());
                auto source = co_await source::LocalFileSource::open(data);
                auto layer = co_await format::LsmtLayer::open(std::move(source));
                REQUIRE(layer->virtual_size() == 65536);
                REQUIRE(layer->header().user_tag == "offline test");
                std::vector<std::unique_ptr<format::LsmtLayer>> layers;
                layers.push_back(std::move(layer));
                auto merged = co_await format::MergedLsmt::open(std::move(layers));
                std::vector<uint8_t> actual(expected.size());
                const auto read = co_await merged->pread(actual.data(), actual.size(), 0);
                REQUIRE(read == static_cast<ssize_t>(expected.size()));
                REQUIRE(actual == expected);
                const auto published = read_fixture(data);
                const int again = co_await format::HybridRwLayer::seal_file(
                    data, index, "different tag", nullptr, nullptr);
                REQUIRE(again == -EALREADY);
                REQUIRE(read_fixture(data) == published);
                co_return 0;
            });
            REQUIRE(rc == 0);
            const auto sealed = read_fixture(data);
            REQUIRE(size == sealed.size());
            REQUIRE(digest == common::Sha256::hex(sealed.data(), sealed.size()));
            REQUIRE(sealed.size() >= original_data.size());
            REQUIRE(std::equal(original_data.begin() + 4096, original_data.end(), sealed.begin() + 4096));
            struct stat after{};
            REQUIRE(::stat(data.c_str(), &after) == 0);
            REQUIRE(after.st_dev == before.st_dev);
            REQUIRE(after.st_ino == before.st_ino);
        }
    }
}

TEST_CASE("format: hybrid offline seal does not create missing artifacts",
          "[format][hybrid]") {
    test::TempDir temporary;
    const auto data = temporary / "data", index = temporary / "index";
    std::string digest = "untouched";
    uint64_t size = 123;
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        co_return co_await format::HybridRwLayer::seal_file(data, index, "", &digest, &size);
    });
    REQUIRE(rc == -ENOENT);
    REQUIRE_FALSE(std::filesystem::exists(data));
    REQUIRE_FALSE(std::filesystem::exists(index));
    REQUIRE(digest == "untouched");
    REQUIRE(size == 123);
}

TEST_CASE("format: hybrid merged shared reads overlap and release mutation guards",
          "[format][hybrid][shared-read]") {
    for (bool fail : {false, true}) {
    DYNAMIC_SECTION("read exception=" << fail) {
        test::TempDir temporary;
        const int rc = elio::run([&]() -> elio::coro::task<int> {
            auto upper = co_await format::HybridRwLayer::create(temporary / "pair", 32768);
            std::array<uint8_t, 512> old{};
            old.fill(0x41);
            const auto wrote = co_await upper->pwrite(old.data(), old.size(), 0);
            REQUIRE(wrote == 512);
            auto held = std::make_unique<HeldHybrid>(std::move(upper));
            auto* observed = held.get();
            observed->view.fail = fail;
            auto merged = co_await format::MergedWritable::open({}, std::move(held));
            std::array<std::array<uint8_t, 512>, 2> data{};
            std::vector<elio::coro::join_handle<ssize_t>> tasks;
            tasks.reserve(3);
            std::vector<ssize_t> results;
            elio::sync::event attempted;
            std::exception_ptr failure;
            bool overlapped = false, excluded = false;
            int grow_result = 0;
            try {
                for (size_t i = 0; i < data.size(); ++i)
                    tasks.push_back(elio::spawn([&, i]() -> elio::coro::task<ssize_t> {
                        try { co_return co_await merged->pread(data[i].data(), 512, 0); }
                        catch (const std::runtime_error&) { co_return -ECANCELED; }
                    }));
                for (unsigned i = 0; i < 100 && observed->view.entries < 2; ++i)
                    co_await elio::time::sleep_for(std::chrono::milliseconds(1));
                overlapped = observed->view.entries == 2;
                grow_result = co_await elio::spawn_blocking([&] { return merged->grow(65536); });
                tasks.push_back(elio::spawn([&]() -> elio::coro::task<ssize_t> {
                    attempted.set();
                    co_return co_await merged->discard(0, 512);
                }));
                co_await attempted.wait();
                excluded = observed->mutation_entries == 0;
            } catch (...) { failure = std::current_exception(); }
            observed->view.release.set();
            for (auto& task : tasks) {
                try { results.push_back(co_await task); }
                catch (...) { if (!failure) failure = std::current_exception(); }
                co_await task.wait_destroyed_async();
            }
            observed->view.fail = false;
            if (failure) std::rethrow_exception(failure);
            REQUIRE(overlapped);
            REQUIRE(excluded);
            REQUIRE(grow_result == -EBUSY);
            REQUIRE(results == std::vector<ssize_t>{fail ? -ECANCELED : 512, fail ? -ECANCELED : 512, 0});
            if (!fail) { REQUIRE(data[0] == old); REQUIRE(data[1] == old); }
            const auto read = co_await merged->pread(data[0].data(), 512, 0);
            REQUIRE(read == 512);
            REQUIRE(std::all_of(data[0].begin(), data[0].end(), [](auto b) { return b == 0; }));
            const auto grown = co_await elio::spawn_blocking([&] { return merged->grow(65536); });
            REQUIRE(grown == 0);
            co_return 0;
        }, {.num_threads = 1, .blocking_threads = 1});
        REQUIRE(rc == 0);
    }
    }
}

TEST_CASE("format: hybrid concurrent readers observe complete write and discard states",
          "[format][hybrid][shared-read]") {
    test::TempDir temporary;
    const int rc = elio::run([&]() -> elio::coro::task<int> {
        auto upper = co_await format::HybridRwLayer::create(temporary / "pair", 32768);
        auto merged = co_await format::MergedWritable::open({}, std::move(upper));
        std::array<uint8_t, 4096> initial{};
        initial.fill(0x11);
        const auto wrote = co_await merged->pwrite(initial.data(), initial.size(), 0);
        REQUIRE(wrote == 4096);
        std::vector<elio::coro::join_handle<bool>> tasks;
        tasks.reserve(5);
        elio::sync::event start;
        std::exception_ptr failure;
        bool valid = true;
        try {
            for (unsigned i = 0; i < 4; ++i) {
                tasks.push_back(elio::spawn([&]() -> elio::coro::task<bool> {
                    std::array<uint8_t, 4096> data{};
                    co_await start.wait();
                    bool ok = true;
                    for (unsigned read = 0; read < 200; ++read) {
                        const auto n = co_await merged->pread(data.data(), data.size(), 0);
                        const auto value = data.front();
                        ok = ok && n == 4096 && (value == 0 || value == 0x11 || value == 0x22) &&
                             std::all_of(data.begin(), data.end(), [value](auto b) { return b == value; });
                    }
                    co_return ok;
                }));
            }
            tasks.push_back(elio::spawn([&]() -> elio::coro::task<bool> {
                std::array<uint8_t, 4096> data{};
                co_await start.wait();
                bool ok = true;
                for (unsigned write = 0; write < 200; ++write) {
                    if (write % 3 == 0) {
                        const int result = co_await merged->discard(0, data.size());
                        ok = ok && result == 0;
                    } else {
                        data.fill(write % 3 == 1 ? 0x11 : 0x22);
                        const auto result = co_await merged->pwrite(data.data(), data.size(), 0);
                        ok = ok && result == 4096;
                    }
                }
                co_return ok;
            }));
        } catch (...) { failure = std::current_exception(); }
        start.set();
        for (auto& task : tasks) {
            try { const bool ok = co_await task; valid = valid && ok; }
            catch (...) { if (!failure) failure = std::current_exception(); }
            co_await task.wait_destroyed_async();
        }
        if (failure) std::rethrow_exception(failure);
        REQUIRE(valid);
        const int flushed = co_await merged->flush();
        REQUIRE(flushed == 0);
        co_return 0;
    }, {.num_threads = 4, .blocking_threads = 1});
    REQUIRE(rc == 0);
}
