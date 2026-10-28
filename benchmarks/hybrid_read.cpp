// Bounded warm-file Hybrid reader comparison. Requires a fresh directory;
// retains the fixture for inspection. Four workers, one blocking worker.
#include "format/hybrid_rw.hpp"
#include "format/merged_writable.hpp"
#include <elio/runtime/async_main.hpp>
#include <elio/runtime/spawn.hpp>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <sys/resource.h>
#include <vector>

namespace {
using namespace obd;
using Clock = std::chrono::steady_clock;
double cpu() {
    rusage u{}; getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_stime.tv_sec +
           (u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1e-6;
}
elio::coro::task<void> run(const char* directory) {
    constexpr size_t size = 4 * 1024 * 1024;
    auto upper = co_await format::HybridRwLayer::create(directory, size);
    auto view = co_await format::MergedWritable::open({}, std::move(upper));
    std::vector<uint8_t> payload(size);
    for (size_t i = 0; i < size; ++i)
        payload[i] = static_cast<uint8_t>(i * 17 + (i / 512) * 23 + (i / 4096) * 7);
    const auto written = co_await view->pwrite(payload.data(), payload.size(), 0);
    const auto flushed = co_await view->flush();
    if (written != ssize_t(size) || flushed) throw std::runtime_error("fixture write");
    std::vector<uint8_t> check(size);
    const auto verified = co_await view->pread(check.data(), check.size(), 0);
    if (verified != ssize_t(size) || check != payload) throw std::runtime_error("fixture bytes");
    for (unsigned qd : {1u, 16u}) {
        for (unsigned round = 0; round < 6; ++round) {
            const size_t count = round == 0 ? 10000 : 100000;
            auto reader = [&](unsigned id) -> elio::coro::task<bool> {
                std::array<uint8_t, 4096> bytes;
                bool valid = true;
                for (size_t i = id; i < count; i += qd) {
                    const auto offset = ((i * 7919) % (size / 4096)) * 4096;
                    const auto n = co_await view->pread(bytes.data(), bytes.size(), offset);
                    valid = valid && n == 4096 && bytes.front() == payload[offset] &&
                            bytes.back() == payload[offset + bytes.size() - 1];
                }
                co_return valid;
            };
            std::vector<elio::coro::join_handle<bool>> tasks;
            tasks.reserve(qd);
            bool valid = true;
            const auto start = Clock::now(); const auto before = cpu();
            try { for (unsigned i=0; i<qd; ++i) tasks.push_back(elio::spawn(reader, i)); }
            catch (...) { valid = false; }
            for (auto& task : tasks) {
                try { const auto ok = co_await task; valid = valid && ok; }
                catch (...) { valid = false; }
                co_await task.wait_destroyed_async();
            }
            const auto wall = std::chrono::duration<double>(Clock::now()-start).count();
            const auto used = cpu()-before;
            if (!valid) throw std::runtime_error("read bytes");
            if (round) std::cout << "{\"qd\":" << qd << ",\"round\":" << round
                << ",\"reads\":" << count << ",\"iops\":" << count/wall
                << ",\"cpu_us\":" << used*1e6/count << "}\n";
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: obd-bench-hybrid-read NEW_DIRECTORY\n";
        return 2;
    }
    try { elio::run([&]()->elio::coro::task<void>{co_await run(argv[1]);},
        {.num_threads=4,.blocking_threads=1}); }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
