// Bounded LayerStore Filling-state reader comparison. Creates a fresh owned
// directory and retains it for inspection; no privileged device or cache drop.
#include "source/layer_store.hpp"
#include "common/sha256.hpp"

#include <elio/runtime/async_main.hpp>
#include <elio/time/timer.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace obd;
constexpr size_t kExtent = 64 * 1024;

class MemorySource final : public source::BlobSource {
public:
    explicit MemorySource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
    elio::coro::task<ssize_t> pread(void* out, size_t n, uint64_t offset) override {
        if (offset >= bytes_.size()) co_return 0;
        n = std::min(n, bytes_.size() - static_cast<size_t>(offset));
        std::memcpy(out, bytes_.data() + offset, n);
        co_return static_cast<ssize_t>(n);
    }
    uint64_t size() const noexcept override { return bytes_.size(); }
    std::string_view label() const noexcept override { return "extent-bench-memory"; }
private:
    std::vector<uint8_t> bytes_;
};

elio::coro::task<void> run(const char* directory, const std::vector<uint8_t>& bytes) {
    auto store = co_await source::LayerStore::open(
        std::make_unique<MemorySource>(bytes), directory,
        common::Sha256::hex(bytes.data(), bytes.size()));
    std::vector<uint8_t> output(kExtent);
    for (unsigned e = 0; e < 8; ++e) {
        const auto n = co_await store->pread(output.data(), kExtent, e * kExtent);
        if (n != static_cast<ssize_t>(kExtent) ||
            !std::equal(output.begin(), output.end(), bytes.begin() + e * kExtent))
            throw std::runtime_error("preparation byte mismatch");
    }
    for (unsigned i = 0; store->extents_present() != 8 && i < 10000; ++i)
        co_await elio::time::sleep_for(std::chrono::milliseconds(1));
    if (store->extents_present() != 8) throw std::runtime_error("persistence timeout");
    const auto fetches = store->remote_fetches();
    // 0 denotes the whole-extent control. Timings include full byte equality
    // checks; they are actual-reader elapsed time, not isolated CRC/CPU cost.
    for (unsigned working : {1u, 4u, 8u, 0u}) {
        const size_t length = working ? 4096 : kExtent;
        for (unsigned round = 0; round < 4; ++round) {
            const unsigned count = round ? 1000 : 100;
            uint64_t random = 173, checksum = 0;
            const auto start = std::chrono::steady_clock::now();
            for (unsigned i = 0; i < count; ++i) {
                random = random * 6364136223846793005ULL + 1;
                const uint64_t offset = working
                    ? (i % working) * kExtent + ((random >> 32) % 16) * 4096 : 0;
                const auto n = co_await store->pread(output.data(), length, offset);
                if (n != static_cast<ssize_t>(length) ||
                    !std::equal(output.begin(), output.begin() + length, bytes.begin() + offset))
                    throw std::runtime_error("read byte mismatch");
                checksum += output[0];
            }
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (round) {
                std::cout << "{\"working_extents\":" << working
                          << ",\"length\":" << length << ",\"round\":" << round - 1
                          << ",\"ns_per_read\":" << static_cast<double>(ns) / count
                          << ",\"checksum\":" << checksum << "}\n";
            }
        }
    }
    if (store->state() != source::LayerStore::State::Filling ||
        store->remote_fetches() != fetches || store->crc_failures() != 0)
        throw std::runtime_error("measurement escaped verified local Filling reads");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2 || !std::filesystem::create_directory(argv[1]))
            throw std::runtime_error("usage: obd-bench-layer-store-read NEW_DIRECTORY");
        std::vector<uint8_t> bytes(16 * kExtent);
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<uint8_t>((i * 17 + i / 512) % 251);
        elio::run([&]() -> elio::coro::task<void> { co_await run(argv[1], bytes); },
                  {.num_threads = 1, .blocking_threads = 1});
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
