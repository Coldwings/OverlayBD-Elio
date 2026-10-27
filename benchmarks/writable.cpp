// Ordinary-file writable-layer comparison. No ublk or privileged operations.
#include "format/lsmt_rw.hpp"
#include "format/sparse_rw.hpp"
#include "format/hybrid_rw.hpp"
#include "format/merged_writable.hpp"
#include "format/lsmt.hpp"
#include "source/local_file.hpp"
#include <elio/runtime/async_main.hpp>
#include <elio/runtime/spawn_blocking.hpp>
#include <elio/coro/task_group.hpp>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/resource.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <numeric>
#include <random>
#include <thread>

using namespace obd;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
struct Settings {
    std::string mode, directory;
    uint64_t size, populate, samples, seed;
    unsigned qd, workers, cycles;
};
double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
json footprint(const std::string& directory) {
    uint64_t logical = 0, allocated = 0;
    json files = json::object();
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        struct stat st{};
        if (::lstat(entry.path().c_str(), &st) != 0) {
            if (errno == ENOENT) continue; // A temporary was atomically retired.
            throw std::system_error(errno, std::system_category(), "benchmark stat");
        }
        if (!S_ISREG(st.st_mode)) continue;
        const auto bytes = static_cast<uint64_t>(st.st_size);
        const auto blocks = static_cast<uint64_t>(st.st_blocks) * 512;
        logical += bytes; allocated += blocks;
        files[entry.path().lexically_relative(directory).string()] =
            {{"bytes", bytes}, {"allocated_bytes", blocks}};
    }
    return {{"logical_bytes", logical}, {"allocated_bytes", allocated}, {"files", files}};
}
elio::coro::task<json> snapshot(const Settings* settings) {
    co_return co_await elio::spawn_blocking([settings] { return footprint(settings->directory); });
}
void require(bool valid, const char* what) {
    if (!valid) throw std::runtime_error(what);
}
struct Peak {
    std::atomic<uint64_t> allocated{0};
    std::atomic<bool> error{false}, active{false};
};

elio::coro::task<int> exercise(const Settings* s, json* report, Peak* peak) {
    const auto data_path = s->directory + "/upper";
    std::unique_ptr<format::WritableLayer> top;
    if (s->mode == "lsmt") top = co_await format::LsmtRwLayer::create(data_path, s->size);
    else if (s->mode == "sparse") top = co_await format::SparseRwLayer::open(data_path, s->size);
    else top = co_await format::HybridRwLayer::create(data_path, s->size);
    auto* layer = top.get();
    auto root = co_await format::MergedWritable::open({}, std::move(top));
    std::vector<uint64_t> offsets(s->size / 4096);
    std::iota(offsets.begin(), offsets.end(), uint64_t{0});
    std::mt19937_64 random(s->seed);
    std::shuffle(offsets.begin(), offsets.end(), random);
    offsets.resize(s->samples);
    std::vector<uint8_t> payload(4096);
    for (auto& b : payload) b = static_cast<uint8_t>(random());
    for (unsigned phase = 0; phase < 2 + s->cycles; ++phase) {
        const std::string name = phase == 0 ? "first_write" : phase == 1 ? "rewrite" : "discard_rewrite_" + std::to_string(phase - 1);
        std::vector<double> latencies(s->samples);
        std::atomic<size_t> next{0}, in_flight{0}, max_in_flight{0};
        std::mutex failure_mutex;
        std::exception_ptr failure;
        elio::coro::task_group tasks;
        const auto start = Clock::now();
        for (unsigned worker = 0; worker < s->qd; ++worker) {
            tasks.spawn([&]() -> elio::coro::task<void> {
                try {
                    for (;;) {
                        const auto index = next.fetch_add(1);
                        if (index >= s->samples) break;
                        const auto active = in_flight.fetch_add(1) + 1;
                        auto old = max_in_flight.load();
                        while (old < active && !max_in_flight.compare_exchange_weak(old, active)) {}
                        const auto began = Clock::now();
                        if (phase >= 2) {
                            const auto rc = co_await root->discard(offsets[index] * 4096, 4096);
                            require(rc == 0, "discard failed");
                        }
                        const auto rc = co_await root->pwrite(payload.data(), payload.size(), offsets[index] * 4096);
                        require(rc == 4096, "write failed");
                        latencies[index] = milliseconds(began);
                        in_flight.fetch_sub(1);
                    }
                } catch (...) {
                    std::lock_guard guard(failure_mutex);
                    if (!failure) failure = std::current_exception();
                }
            });
        }
        co_await tasks.join();
        if (failure) std::rethrow_exception(failure);
        const auto elapsed = milliseconds(start);
        const auto flush_start = Clock::now();
        const int flushed = co_await root->flush();
        require(flushed == 0, "flush failed");
        const auto flush_ms = milliseconds(flush_start);
        std::sort(latencies.begin(), latencies.end());
        (*report)["phases"][name] = {{"operations", s->samples}, {"wall_ms", elapsed},
            {"operations_per_second", s->samples * 1000.0 / elapsed},
            {"latency_p50_ms", latencies[latencies.size()/2]},
            {"latency_p99_ms", latencies[(latencies.size()-1)*99/100]},
            {"latency_max_ms", latencies.back()}, {"flush_ms", flush_ms},
            {"peak_awaiting_operations", max_in_flight.load()}};
        auto state = co_await snapshot(s);
        (*report)["phases"][name]["live_mappings"] = layer->segments_snapshot().size();
        if (s->mode == "hybrid")
            (*report)["phases"][name]["log_records"] =
                (state["files"]["upper/index"]["bytes"].get<uint64_t>() - 4096) / 16;
        (*report)["phases"][name]["footprint"] = std::move(state);
        // Every sampled sector is verified before the next phase.
        std::vector<uint8_t> actual(4096);
        for (const auto block : offsets) {
            const auto read = co_await root->pread(actual.data(), actual.size(), block * 4096);
            require(read == 4096 && actual == payload, "sample verification failed");
        }
    }
    // Separate full population from timed random samples; size is not a proxy
    // for actually written payload. Full runs set populate_bytes == size_bytes.
    std::vector<uint8_t> fill(1 << 20);
    for (auto& b : fill) b = static_cast<uint8_t>(random());
    const auto fill_start = Clock::now();
    for (uint64_t offset = 0; offset < s->populate; offset += fill.size()) {
        const auto count = static_cast<size_t>(std::min<uint64_t>(fill.size(), s->populate - offset));
        const auto rc = co_await root->pwrite(fill.data(), count, offset);
        require(rc == static_cast<ssize_t>(count), "population failed");
    }
    const int flushed = co_await root->flush();
    require(flushed == 0, "population flush failed");
    (*report)["populate_ms_including_flush"] = milliseconds(fill_start);
    (*report)["before_reopen"] = co_await snapshot(s);
    root.reset();
    const auto reopen_start = Clock::now();
    if (s->mode == "lsmt")
        top = co_await format::LsmtRwLayer::open_existing(data_path, data_path + ".index");
    else if (s->mode == "sparse")
        top = co_await format::SparseRwLayer::open(data_path, s->size);
    else
        top = co_await format::HybridRwLayer::open_existing(data_path + "/data", data_path + "/index");
    (*report)["reopen_ms"] = milliseconds(reopen_start);
    layer = top.get();
    root = co_await format::MergedWritable::open({}, std::move(top));
    // Verify the populated region after recovery, including its tail.
    std::vector<uint8_t> actual(fill.size());
    for (uint64_t offset = 0; offset < s->populate; offset += fill.size()) {
        const auto count = static_cast<size_t>(std::min<uint64_t>(fill.size(), s->populate - offset));
        const auto read = co_await root->pread(actual.data(), count, offset);
        require(read == static_cast<ssize_t>(count) && std::equal(actual.begin(), actual.begin()+count, fill.begin()),
                "populated bytes differ after reopen");
    }
    const auto before = co_await snapshot(s);
    peak->allocated.store(before.at("allocated_bytes").get<uint64_t>());
    peak->active.store(true);
    const auto seal_start = Clock::now();
    if (s->mode == "hybrid") {
        const int rc = co_await dynamic_cast<format::HybridRwLayer*>(layer)->seal();
        require(rc == 0, "hybrid seal failed");
    } else if (s->mode == "lsmt") {
        const int rc = co_await dynamic_cast<format::LsmtRwLayer*>(layer)->seal();
        require(rc == 0, "lsmt seal failed");
    } else {
        // Offline Sparse export needs exclusive ownership of the source.
        // Include releasing/reopening it and the API's digest in this timing.
        root.reset();
        layer = nullptr;
        const auto result = co_await format::SparseRwLayer::export_file(
            data_path, data_path + ".lsmt");
        (*report)["export_sha256"] = result.sha256;
    }
    peak->active.store(false);
    (*report)["seal_ms"] = milliseconds(seal_start);
    (*report)["seal_note"] = s->mode == "sparse"
        ? "Offline export to a separate LSMT delta, including owner release, reopen and API digest; source retained."
        : "Live seal, including format-required durability; no external digest pass.";
    auto after = co_await snapshot(s);
    (*report)["seal_peak_allocated_bytes_sampled"] = std::max(
        peak->allocated.load(), after.at("allocated_bytes").get<uint64_t>());
    (*report)["seal_peak_sample_period_ms"] = 10;
    (*report)["after_seal"] = std::move(after);
    root.reset();
    {
        const auto verify_start = Clock::now();
        const auto sealed_path = s->mode == "hybrid" ? data_path + "/data"
            : s->mode == "sparse" ? data_path + ".lsmt" : data_path;
        auto sealed_source = co_await source::LocalFileSource::open(sealed_path);
        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
        layers.push_back(co_await format::LsmtLayer::open(std::move(sealed_source)));
        auto sealed = co_await format::MergedLsmt::open(std::move(layers));
        require(sealed->size() == s->size, "sealed virtual size differs");
        for (uint64_t offset = 0; offset < s->populate; offset += fill.size()) {
            const auto count = static_cast<size_t>(std::min<uint64_t>(fill.size(), s->populate - offset));
            const auto read = co_await sealed->pread(actual.data(), count, offset);
            require(read == static_cast<ssize_t>(count) &&
                    std::equal(actual.begin(), actual.begin() + count, fill.begin()),
                    "sealed populated bytes differ");
        }
        uint64_t sampled_tail_bytes = 0;
        for (const auto block : offsets) {
            if (block * 4096 < s->populate) continue;
            const auto read = co_await sealed->pread(actual.data(), payload.size(), block * 4096);
            require(read == static_cast<ssize_t>(payload.size()) &&
                    std::equal(payload.begin(), payload.end(), actual.begin()),
                    "sealed sampled tail bytes differ");
            sampled_tail_bytes += payload.size();
        }
        (*report)["sealed_verification"] = {{"populated_bytes", s->populate},
            {"sampled_tail_bytes", sampled_tail_bytes},
            {"wall_ms", milliseconds(verify_start)}, {"passed", true}};
    }
    co_return 0;
}

int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "usage: obd-bench-writable SETTINGS.json REPORT.json\n"; return 2; }
    const int report_fd = ::open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (report_fd < 0) { std::cerr << "cannot exclusively create report\n"; return 2; }
    json report;
    try {
        std::ifstream input(argv[1]);
        const auto j = json::parse(input);
        Settings s{j.at("mode"), j.at("directory"), j.at("size_bytes"), j.at("populate_bytes"),
            j.value("samples", uint64_t{4096}), j.value("seed", uint64_t{146}),
            j.value("qd", 1u), j.value("workers", 2u), j.value("cycles", 2u)};
        require(s.mode == "lsmt" || s.mode == "sparse" || s.mode == "hybrid", "invalid mode");
        require(s.size && s.size % 4096 == 0 && s.size <= (uint64_t{64} << 30), "size outside aligned 1..64GiB range");
        require(s.populate && s.populate <= s.size && s.populate % 4096 == 0, "invalid population");
        require(s.samples && s.samples <= s.size/4096 && s.samples <= 1048576, "invalid sample count");
        require((s.qd == 1 || s.qd == 16) && s.workers > 0 && s.workers <= 64 && s.cycles <= 100, "invalid concurrency/cycles");
        require(fs::path(s.directory).is_absolute(), "directory must be absolute");
        require(fs::create_directory(s.directory), "refuse existing data directory");
        report = {{"settings", j}, {"status", "running"},
            {"path", "MergedWritable userspace IO; QD counts awaiting operations, not kernel queue depth"},
            {"cache", "warm OS cache, no global cache drop"}};
        Peak peak;
        std::jthread sampler([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                try {
                    if (!peak.active.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    }
                    const auto bytes = footprint(s.directory).at("allocated_bytes").get<uint64_t>();
                    auto old = peak.allocated.load();
                    while (old < bytes && !peak.allocated.compare_exchange_weak(old, bytes)) {}
                } catch (const fs::filesystem_error& e) {
                    if (e.code().value() != ENOENT) peak.error.store(true);
                } catch (...) { peak.error.store(true); }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        elio::runtime::run_config config;
        config.num_threads = s.workers;
        config.blocking_threads = 2;
        config.shutdown_timeout = std::chrono::milliseconds::max();
        const int rc = elio::run(config, exercise, &s, &report, &peak);
        sampler.request_stop(); sampler.join();
        require(rc == 0 && !peak.error.load(), "run or footprint sampler failed");
        struct rusage usage{}; require(::getrusage(RUSAGE_SELF, &usage) == 0, "getrusage failed");
        report["max_rss_kib"] = usage.ru_maxrss;
        report["cpu_user_seconds"] = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6;
        report["cpu_system_seconds"] = usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
        report["status"] = "passed";
    } catch (const std::exception& e) {
        report["status"] = "failed"; report["error"] = e.what();
    }
    const auto text = report.dump(2) + "\n";
    size_t done = 0;
    while (done < text.size()) {
        const auto n = ::write(report_fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ::close(report_fd); return 1; }
        done += static_cast<size_t>(n);
    }
    const int closed = ::close(report_fd);
    return closed == 0 && report.value("status", "") == "passed" ? 0 : 1;
}
