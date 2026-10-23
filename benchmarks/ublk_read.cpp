#include "ublk/device.hpp"
#include "format/lsmt.hpp"
#include "source/local_file.hpp"
#include <elio/runtime/async_main.hpp>
#include <elio/runtime/spawn_blocking.hpp>
#include <nlohmann/json.hpp>
#include <sys/resource.h>
#include <sys/wait.h>
#include <spawn.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <exception>
#include <limits>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

extern char** environ;
using json = nlohmann::json;
struct Settings {
    std::string mode, image, prefix, fio;
    uint64_t bytes;
    unsigned workers, blocking, qd, queues, depth, warm_seconds, run_seconds;
    bool fail_after_warmup = false;
};

uint64_t positive(const json& value, const char* key, uint64_t fallback,
                  uint64_t maximum) {
    const auto it = value.find(key);
    uint64_t n = fallback;
    if (it != value.end()) {
        if (!it->is_number_integer()) {
            throw std::runtime_error(std::string(key) + " must be an integer");
        }
        if (it->is_number_unsigned()) {
            n = it->get<uint64_t>();
        } else {
            const auto signed_value = it->get<int64_t>();
            if (signed_value <= 0) throw std::runtime_error(key);
            n = static_cast<uint64_t>(signed_value);
        }
    }
    if (!n || n > maximum) throw std::runtime_error(key);
    return n;
}

Settings parse_settings(const json& input) {
    if (!input.is_object()) throw std::runtime_error("config must be an object");
    Settings settings;
    settings.mode = input.at("mode").get<std::string>();
    if (settings.mode != "zero" && settings.mode != "lsmt") {
        throw std::runtime_error("mode must be zero or lsmt");
    }
    settings.image = input.value("image", std::string());
    settings.prefix = input.at("prefix").get<std::string>();
    settings.fio = input.at("fio").get<std::string>();
    if (!std::filesystem::path(settings.prefix).is_absolute() ||
        !std::filesystem::path(settings.fio).is_absolute() ||
        (settings.mode == "lsmt" &&
         !std::filesystem::path(settings.image).is_absolute())) {
        throw std::runtime_error("prefix, fio and LSMT image paths must be absolute");
    }
    for (const char* suffix : {".owner.json", ".warm.json", ".fio.json", ".server.json"}) {
        if (std::filesystem::symlink_status(settings.prefix + suffix).type() !=
            std::filesystem::file_type::not_found) {
            throw std::runtime_error("refusing existing trial output: " + settings.prefix + suffix);
        }
    }
    settings.bytes = positive(input, "bytes", 0, std::numeric_limits<int64_t>::max());
    if (settings.bytes % 4096) throw std::runtime_error("bytes must be a multiple of 4096");
    settings.workers = static_cast<unsigned>(positive(input, "workers", 1, 65535));
    settings.blocking = static_cast<unsigned>(positive(input, "blocking_threads", 2, 65535));
    settings.queues = static_cast<unsigned>(positive(input, "queues", 1, 65535));
    settings.depth = static_cast<unsigned>(positive(input, "queue_depth", 128, 65535));
    settings.qd = static_cast<unsigned>(positive(input, "qd", 1, 65535));
    settings.warm_seconds = static_cast<unsigned>(positive(input, "warm_seconds", 1, 86400));
    settings.run_seconds = static_cast<unsigned>(positive(input, "run_seconds", 5, 86400));
    settings.fail_after_warmup = input.value("fail_after_warmup", false);
    return settings;
}

class ZeroSource final : public obd::source::BlobSource {
public:
    explicit ZeroSource(uint64_t size) : bytes_(size) {}
    elio::coro::task<ssize_t> pread(void* out, size_t n, uint64_t offset) override {
        if (!n || offset >= bytes_) co_return 0;
        if (!out || n > size_t(std::numeric_limits<ssize_t>::max())) co_return -EINVAL;
        n = static_cast<size_t>(std::min<uint64_t>(n, bytes_ - offset));
        std::memset(out, 0, n);
        co_return static_cast<ssize_t>(n);
    }
    uint64_t size() const noexcept override { return bytes_; }
    std::string_view label() const noexcept override { return "bench-zero"; }
private:
    uint64_t bytes_;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

json cpu_snapshot() {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage)) throw std::runtime_error("getrusage");
    auto micros = [](const timeval& time) {
        return static_cast<int64_t>(time.tv_sec) * 1000000 + time.tv_usec;
    };
    json threads = json::object();
    for (const auto& task : std::filesystem::directory_iterator("/proc/self/task")) {
        try {
            threads[task.path().filename().string()] = {
                {"stat", read_file(task.path() / "stat")},
                {"status", read_file(task.path() / "status")}};
        } catch (const std::filesystem::filesystem_error&) {
        }
    }
    return {{"user_us", micros(usage.ru_utime)}, {"system_us", micros(usage.ru_stime)},
            {"voluntary", usage.ru_nvcsw}, {"involuntary", usage.ru_nivcsw},
            {"process_stat", read_file("/proc/self/stat")},
            {"clock_ticks", ::sysconf(_SC_CLK_TCK)}, {"threads", std::move(threads)}};
}

void write_json(const std::string& path, const json& value) {
    std::ofstream output(path);
    output << value.dump(2) << '\n';
    output.flush();
    if (!output) throw std::runtime_error("cannot write " + path);
}

json run_fio(const Settings& settings, const std::string& device,
             const std::string& output, unsigned seconds) {
    std::vector<std::string> arguments = {
        settings.fio, "--name=ublk-read", "--filename=" + device,
        "--rw=randread", "--bs=4k", "--direct=1", "--ioengine=io_uring",
        "--iodepth=" + std::to_string(settings.qd), "--numjobs=1",
        "--size=" + std::to_string(settings.bytes), "--time_based=1",
        "--runtime=" + std::to_string(seconds), "--randrepeat=1",
        "--norandommap=1", "--output-format=json", "--output=" + output,
        "--group_reporting=1", "--eta=never"};
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    pid_t child = -1;
    const int error = ::posix_spawn(&child, argv[0], nullptr, nullptr,
                                    argv.data(), environ);
    if (error) throw std::runtime_error("posix_spawn: " + std::string(std::strerror(error)));
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) throw std::runtime_error("waitpid fio");
    }
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) :
                     WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
    return {{"command", arguments}, {"pid", child}, {"exit", code}};
}

elio::coro::task<int> exercise(Settings settings, json* report,
                                std::unique_ptr<obd::ublk::Device>* owner) {
    obd::source::BlobSourcePtr source;
    size_t segments = 0;
    if (settings.mode == "zero") {
        source = std::make_unique<ZeroSource>(settings.bytes);
    } else {
        auto file = co_await obd::source::LocalFileSource::open(settings.image);
        std::vector<std::unique_ptr<obd::format::LsmtLayer>> layers;
        layers.push_back(co_await obd::format::LsmtLayer::open(std::move(file)));
        auto merged = co_await obd::format::MergedLsmt::open(std::move(layers));
        segments = merged->merged_index().size();
        source = std::move(merged);
    }
    if (source->size() != settings.bytes) throw std::runtime_error("source size mismatch");
    obd::ublk::DeviceParams params;
    params.dev_sectors = source->size() / 512;
    params.nr_queues = static_cast<uint16_t>(settings.queues);
    params.queue_depth = static_cast<uint16_t>(settings.depth);
    params.enable_recovery = false;
    // The owning unique_ptr lives in synchronous main, beyond elio::run.
    // Coroutine unwinding can never destroy Device on an Elio worker.
    *owner = co_await obd::ublk::Device::create(params, std::move(source));
    auto* device = owner->get();
    json result;
    std::exception_ptr failure;
    int status = 0;
    try {
        result = {{"mode", settings.mode}, {"bytes", settings.bytes},
                  {"queues", device->queue_count()},
                  {"workers", elio::runtime::scheduler::current()->num_threads(std::memory_order_acquire)},
                  {"queue_depth", settings.depth},
                  {"qd", settings.qd}, {"segments", segments}};
        const auto path = device->bdev_path();
        // Persist the actual owner before fio can block; the runner never
        // turns these historical numbers into authority to signal/delete.
        co_await elio::spawn_blocking([&] {
            write_json(settings.prefix + ".owner.json",
                       {{"pid", ::getpid()}, {"process_stat", read_file("/proc/self/stat")},
                        {"process_status", read_file("/proc/self/status")}, {"dev_id", device->dev_id()}, {"device", path}});
        });
        const auto warm = co_await elio::spawn_blocking([&] {
            return run_fio(settings, path, settings.prefix + ".warm.json",
                           settings.warm_seconds);
        });
        result["warm"] = warm;
        if (warm.at("exit").get<int>()) throw std::runtime_error("warm fio failed");
        if (settings.fail_after_warmup) {
            throw std::runtime_error("requested body failure after warm-up");
        }
        // Snapshot and waitpid work are cold diagnostic operations off-worker.
        result["before"] = co_await elio::spawn_blocking(cpu_snapshot);
        const auto begin = std::chrono::steady_clock::now();
        result["measured"] = co_await elio::spawn_blocking([&] {
            return run_fio(settings, path, settings.prefix + ".fio.json",
                           settings.run_seconds);
        });
        const auto end = std::chrono::steady_clock::now();
        result["after"] = co_await elio::spawn_blocking(cpu_snapshot);
        result["server_window_ns"] = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
        status = result["measured"].at("exit").get<int>();
    } catch (...) {
        failure = std::current_exception();
    }
    co_await device->stop_async();
    result["drain_complete"] = true;
    *report = std::move(result);
    if (failure) std::rethrow_exception(failure);
    co_return status;
}

int main(int argc, char** argv) {
    if (argc != 3 || std::string(argv[1]) != "--config") {
        std::fprintf(stderr, "usage: obd-bench-ublk-read --config run.json\n");
        return 2;
    }
    std::unique_ptr<obd::ublk::Device> owner;
    json report;
    std::string prefix;
    int status = 1;
    std::exception_ptr failure;
    try {
        const auto settings = parse_settings(json::parse(read_file(argv[2])));
        prefix = settings.prefix;
        elio::runtime::run_config config;
        config.num_threads = settings.workers;
        config.blocking_threads = settings.blocking;
        // A failure before stop starts may retain this main-owned Device
        // indefinitely. Never replace graceful drain with a force-stop budget.
        config.shutdown_timeout = std::chrono::milliseconds::max();
        status = elio::run(config, exercise, settings, &report, &owner);
    } catch (...) {
        failure = std::current_exception();
    }
    // Main-thread-only on normal or exceptional runtime return. This public
    // destructor barrier can still wait indefinitely; it never frees early.
    owner.reset();
    try {
        if (failure) {
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception& error) {
                std::fprintf(stderr, "benchmark failed: %s\n", error.what());
                report["error"] = error.what();
            } catch (...) {
                std::fprintf(stderr, "benchmark failed with an unknown exception\n");
                report["error"] = "unknown exception";
            }
        }
        if (!prefix.empty()) {
            report["cleanup_complete"] = true;
            write_json(prefix + ".server.json", report);
        }
    } catch (...) {
        std::fprintf(stderr, "could not record benchmark result\n");
        return 1;
    }
    return status;
}
