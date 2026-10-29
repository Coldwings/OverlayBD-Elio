// Same public-API workload for coverage and extent-owned Sparse revisions.
#include "format/merged_writable.hpp"
#include "format/sparse_rw.hpp"
#include "format/writer.hpp"
#include "source/local_file.hpp"
#include <elio/runtime/async_main.hpp>
#include <elio/runtime/spawn_blocking.hpp>
#include <elio/time/timer.hpp>
#ifdef OBD_TEST_HOOKS
#include <elio/io/io_context.hpp>
#include <elio/io/io_uring_backend.hpp>
#endif
#include <array>
#include <nlohmann/json.hpp>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <system_error>

using namespace obd;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
using elio::coro::task;
static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Optional accounting run. Never use its latency as production performance:
// path classification adds a readlink per request. Ordinary builds omit it.
namespace traffic {
#ifdef OBD_TEST_HOOKS
struct Counts { uint64_t reads = 0, read_bytes = 0, writes = 0, write_bytes = 0; };
inline std::array<Counts, 3> counts;
inline bool enabled = false, active = false;
class Backend final : public elio::io::io_backend {
    elio::io::io_uring_backend inner_;
public:
    bool supports_file_operation(elio::io::io_op op) const noexcept override { return inner_.supports_file_operation(op); }
    bool prepare(const elio::io::io_request& request) override {
        using elio::io::io_op;
        int category = -1;
        const bool read = request.op == io_op::read || request.op == io_op::readv;
        const bool write = request.op == io_op::write || request.op == io_op::writev;
        uint64_t bytes = request.length;
        if (active && (read || write)) {
            char name[64], target[4096];
            std::snprintf(name, sizeof(name), "/proc/self/fd/%d", request.fd);
            const auto size = ::readlink(name, target, sizeof(target));
            if (size < 0) throw std::system_error(errno, std::system_category(), "probe fd lookup");
            const std::string_view path(target, static_cast<size_t>(size));
            if (path.ends_with("/upper")) category = 0;
            else if (path.ends_with("/upper.coverage")) category = 1;
            else if (path.ends_with("/lower.lsmt")) category = 2;
            if (request.op == io_op::readv || request.op == io_op::writev) {
                bytes = 0;
                for (size_t i = 0; i < request.iovec_count; ++i) bytes += request.iovecs[i].iov_len;
            }
        }
        const bool accepted = inner_.prepare(request);
        // No allocation or exception is permitted after accepting the request.
        if (accepted && category >= 0) {
            auto& value = counts[static_cast<size_t>(category)];
            if (read) { ++value.reads; value.read_bytes += bytes; }
            else { ++value.writes; value.write_bytes += bytes; }
        }
        return accepted;
    }
    int submit() override { return inner_.submit(); }
    int poll(std::chrono::milliseconds timeout) override { return inner_.poll(timeout); }
    bool has_pending() const noexcept override { return inner_.has_pending(); }
    size_t pending_count() const noexcept override { return inner_.pending_count(); }
    bool cancel(void* data) override { return inner_.cancel(data); }
    // Elio's true branch permits concrete io_uring_backend casts for raw
    // batching/destructor close. This adapter is not that concrete type.
    // Generic requests still delegate to the ring; traffic runs use no delay.
    bool is_io_uring() const noexcept override { return false; }
    void notify() noexcept override { inner_.notify(); }
    void drain_notify() override { inner_.drain_notify(); }
};
void enable() {
    enabled = true;
    elio::io::detail::worker_backend_factory_for_test.store(+[](size_t) -> elio::io::io_backend* { return new Backend; });
}
void begin() { counts = {}; active = enabled; }
void end() { active = false; }
json snapshot() {
    if (!enabled) return nullptr;
    json result;
    const std::array<const char*, 3> labels{"upper_data", "ownership_metadata", "lower_blob"};
    for (size_t i = 0; i < counts.size(); ++i) {
        const auto& c = counts[i];
        result[labels[i]] = {{"read_requests", c.reads}, {"read_requested_bytes", c.read_bytes},
            {"write_requests", c.writes}, {"write_requested_bytes", c.write_bytes}};
    }
    return result;
}
#else
void enable() { throw std::runtime_error("traffic accounting requires a test-enabled build"); }
void begin() {}
void end() {}
json snapshot() { return nullptr; }
#endif
}
struct Settings { std::string directory; size_t samples; unsigned delay_us; uint64_t seed; };
struct Meter { uint64_t calls = 0, bytes = 0; bool active = false; };
class Lower final : public source::BlobSource {
public:
    Lower(source::BlobSourcePtr source, Meter* meter, unsigned delay)
        : source_(std::move(source)), meter_(meter), delay_(delay) {}
    task<ssize_t> pread(void* buffer, size_t count, uint64_t offset) override {
        if (meter_->active) {
            ++meter_->calls; meter_->bytes += count;
            if (delay_) co_await elio::time::sleep_for(std::chrono::microseconds(delay_));
        }
        co_return co_await source_->pread(buffer, count, offset);
    }
    uint64_t size() const noexcept override { return source_->size(); }
    std::string_view label() const noexcept override { return source_->label(); }
private:
    source::BlobSourcePtr source_;
    Meter* meter_;
    unsigned delay_;
};
static double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
static json resources(const std::string& directory) {
    struct rusage r {};
    require(::getrusage(RUSAGE_SELF, &r) == 0, "getrusage");
    json result = {{"user_us", r.ru_utime.tv_sec * 1000000LL + r.ru_utime.tv_usec},
                   {"system_us", r.ru_stime.tv_sec * 1000000LL + r.ru_stime.tv_usec}};
    std::ifstream io("/proc/self/io");
    std::string name; uint64_t value;
    while (io >> name >> value) result[name.substr(0, name.size()-1)] = value;
    uint64_t allocated = 0;
    json files = json::object();
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.path().filename().string().rfind("upper", 0) != 0) continue;
        struct stat st {};
        require(::stat(entry.path().c_str(), &st) == 0, "stat upper");
        allocated += static_cast<uint64_t>(st.st_blocks) * 512;
        files[entry.path().filename().string()] = {{"size", st.st_size}, {"allocated", st.st_blocks * 512}};
    }
    result["allocated_upper_bytes"] = allocated;
    result["upper_files"] = files;
    return result;
}
static task<json> sample(const Settings* s) {
    co_return co_await elio::spawn_blocking([s] { return resources(s->directory); });
}
static task<std::unique_ptr<format::MergedWritable>> open(const Settings* s, Meter* meter, uint64_t size) {
    auto file = co_await source::LocalFileSource::open(s->directory + "/lower.lsmt");
    auto lower = co_await format::LsmtLayer::open(std::make_unique<Lower>(std::move(file), meter, s->delay_us));
    std::vector<std::unique_ptr<format::LsmtLayer>> layers;
    layers.push_back(std::move(lower));
    auto top = co_await format::SparseRwLayer::open(s->directory + "/upper", size);
    co_return co_await format::MergedWritable::open(std::move(layers), std::move(top));
}
static task<void> verify(format::MergedWritable* root, const std::vector<unsigned char>* model) {
    std::vector<unsigned char> actual(model->size());
    const auto n = co_await root->pread(actual.data(), actual.size(), 0);
    require(n == static_cast<ssize_t>(actual.size()) && actual == *model, "full model mismatch");
}
static task<int> exercise(const Settings* s, std::vector<unsigned char>* model, json* report) {
    Meter meter;
    auto root = co_await open(s, &meter, model->size());
    std::vector<size_t> permutation(4 * s->samples);
    std::iota(permutation.begin(), permutation.end(), size_t{0});
    std::mt19937_64 random(s->seed);
    std::shuffle(permutation.begin(), permutation.end(), random);
    permutation.resize(s->samples);
    const std::vector<std::string> phases = {"first_512", "second_sector_same_block_512", "overwrite_same_sector_512",
        "random_512", "aligned_4096", "two_512", "warm_read_4096_after_two_512"};
    std::vector<unsigned char> payload(4096), readback(4096);
    for (size_t phase = 0; phase < phases.size(); ++phase) {
        std::fill(payload.begin(), payload.end(), static_cast<unsigned char>(0x90 + phase));
        const size_t operations = s->samples * (phase == 6 ? 4 : 1);
        std::vector<double> latencies;
        latencies.reserve(operations);
        const auto before = co_await sample(s);
        meter.calls = 0; meter.bytes = 0; meter.active = true;
        traffic::begin();
        const auto began = Clock::now();
        uint64_t guest_bytes = 0, guest_requests = 0;
        for (size_t i = 0; i < operations; ++i) {
            const auto timed = Clock::now();
            const size_t block = phase <= 2 ? i : phase == 3 ? s->samples + permutation[i]
                : phase == 4 ? 5 * s->samples + i : 6 * s->samples + i % s->samples;
            const size_t offset = block * 4096 + (phase <= 3 ? (phase == 1 ? 1024 : 512) : 0);
            const size_t count = phase == 4 || phase == 6 ? 4096 : 512;
            if (phase == 6) {
                const auto n = co_await root->pread(readback.data(), count, offset);
                require(n == static_cast<ssize_t>(count), "read failed");
                require(std::memcmp(readback.data(), model->data() + offset, count) == 0, "timed read mismatch");
            } else {
                const auto n = co_await root->pwrite(payload.data(), count, offset);
                require(n == static_cast<ssize_t>(count), "write failed");
                std::copy_n(payload.begin(), count, model->begin() + static_cast<ptrdiff_t>(offset));
                if (phase == 5) {
                    const auto second = co_await root->pwrite(payload.data(), 512, offset + 1024);
                    require(second == 512, "second write failed");
                    std::copy_n(payload.begin(), 512, model->begin() + static_cast<ptrdiff_t>(offset + 1024));
                    guest_bytes += 512; ++guest_requests;
                }
            }
            guest_bytes += count; ++guest_requests;
            latencies.push_back(elapsed(timed));
        }
        const double wall = elapsed(began);
        meter.active = false;
        traffic::end();
        const auto io_requests = traffic::snapshot();
        const auto after = co_await sample(s);
        std::sort(latencies.begin(), latencies.end());
        json delta;
        for (const char* key : {"user_us", "system_us", "rchar", "wchar", "syscr", "syscw", "read_bytes", "write_bytes"})
            delta[key] = after.at(key).get<int64_t>() - before.at(key).get<int64_t>();
        (*report)["phases"].push_back({{"name", phases[phase]}, {"wall_ms", wall},
            {"sequence_p50_ms", latencies[latencies.size()/2]},
            {"sequence_p99_ms", latencies[(latencies.size()-1)*99/100]},
            {"guest_requests", guest_requests}, {"guest_bytes", guest_bytes},
            {"requests_per_second", static_cast<double>(guest_requests) * 1000 / wall},
            {"lower_calls", meter.calls}, {"lower_requested_bytes", meter.bytes},
            {"merged_fragments", root->merged_index().size()}, {"resources_delta", delta}, {"io_requests", io_requests},
            {"allocated_upper_bytes", after.at("allocated_upper_bytes")}, {"upper_files", after.at("upper_files")}});
        // Outside measured phases and delay model; preserve complete byte oracle.
        co_await verify(root.get(), model);
    }
    root.reset(); // Intentionally no FLUSH before recovery timing.
    const auto recovered = Clock::now();
    root = co_await open(s, &meter, model->size());
    (*report)["unflushed_same_process_close_reopen_ms"] = elapsed(recovered);
    co_await verify(root.get(), model);
    const auto flushing = Clock::now();
    const int flushed = co_await root->flush();
    (*report)["flush_after_reopen_ms"] = elapsed(flushing);
    require(flushed == 0, "flush failed");
    (*report)["final"] = co_await sample(s);
    co_return 0;
}
int main(int argc, char** argv) {
    if (argc != 6 && argc != 7) { std::cerr << "usage: sparse-copyup NEW_DIRECTORY REPORT SAMPLES LOWER_DELAY_US SEED [--traffic]\n"; return 2; }
    json report;
    try {
        const bool accounting = argc == 7;
        if (accounting) { require(std::string_view(argv[6]) == "--traffic", "invalid mode"); traffic::enable(); }
        Settings s{argv[1], std::stoull(argv[3]), static_cast<unsigned>(std::stoul(argv[4])), std::stoull(argv[5])};
        require(s.samples && s.samples <= 65536 && s.delay_us <= 1000000, "invalid bounds");
        require(!accounting || s.delay_us == 0, "traffic accounting uses ordinary file IO without delay");
        require(fs::path(s.directory).is_absolute() && fs::create_directory(s.directory), "directory must be new and absolute");
        require(!fs::exists(argv[2]), "refuse existing report");
        std::vector<unsigned char> model(s.samples * 7 * 4096);
        for (size_t i = 0; i < model.size(); ++i) model[i] = static_cast<unsigned char>(1 + (i * 17 + i/4096) % 251);
        const auto raw = s.directory + "/lower.raw";
        { std::ofstream file(raw, std::ios::binary); file.write(reinterpret_cast<const char*>(model.data()), static_cast<std::streamsize>(model.size())); require(file.good(), "write lower"); }
        const int fd = ::open(raw.c_str(), O_RDONLY | O_CLOEXEC);
        require(fd >= 0, "open lower");
        try { format::write_lsmt_single_layer(fd, model.size(), s.directory + "/lower.lsmt"); }
        catch (...) { ::close(fd); throw; }
        require(::close(fd) == 0, "close lower");
        report = {{"traffic_accounting_only", accounting}, {"samples", s.samples}, {"lower_delay_us", s.delay_us}, {"seed", s.seed},
            {"scope", "QD1, one worker, warm local lower; optional constant delay per payload source call; no real network or physical amplification claim"},
            {"measurement", "phase CPU and proc/self/io deltas include sampling overhead; per-sequence timer includes model bookkeeping; two_512 has two writes per sequence; full verification outside timers warms following reads; unflushed same-process reopen includes recovery writeback; final flush follows that writeback; st_blocks snapshots before FLUSH may reflect delayed allocation; proc syscall counters do not count io_uring payload requests"}};
        elio::runtime::run_config config;
        config.num_threads = 1; config.blocking_threads = 2;
        config.shutdown_timeout = std::chrono::milliseconds::max();
        require(elio::run(config, exercise, &s, &model, &report) == 0, "runtime failed");
        report["status"] = "passed";
    } catch (const std::exception& e) { report["status"] = "failed"; report["error"] = e.what(); }
    const int fd = ::open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return 1;
    const auto text = report.dump(2) + "\n";
    size_t done = 0;
    while (done < text.size()) { const auto n = ::write(fd, text.data()+done, text.size()-done); if (n < 0 && errno == EINTR) continue; if (n <= 0) { ::close(fd); return 1; } done += static_cast<size_t>(n); }
    const int closed = ::close(fd);
    return closed == 0 && report.value("status", "") == "passed" ? 0 : 1;
}
