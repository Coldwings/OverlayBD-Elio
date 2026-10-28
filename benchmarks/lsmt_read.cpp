// Bounded CPU-path experiment: actual merged reader, memory-backed sealed
// fixture, random aligned 4 KiB requests. This does not measure ublk or disk I/O.
#include "format/lsmt.hpp"

#include <elio/runtime/async_main.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using namespace obd;

class MemorySource final : public source::BlobSource {
public:
    std::vector<uint8_t> data;
    uint64_t calls = 0;

    elio::coro::task<ssize_t> pread(void* out, size_t count,
                                    uint64_t offset) override {
        ++calls;
        if (offset >= data.size()) co_return 0;
        count = std::min(count, data.size() - offset);
        std::memcpy(out, data.data() + offset, count);
        co_return static_cast<ssize_t>(count);
    }
    uint64_t size() const noexcept override { return data.size(); }
    std::string_view label() const noexcept override { return "bench-memory"; }
};

elio::coro::task<void> measure(unsigned chunk_sectors) {
    constexpr unsigned sectors = 8192;
    constexpr unsigned iterations = 100000;
    constexpr unsigned warmup_iterations = 10000;
    auto source = std::make_unique<MemorySource>();
    auto* observation = source.get();
    const unsigned entries = (sectors + chunk_sectors - 1) / chunk_sectors;
    const uint64_t index_offset = 4096 + sectors * 512;
    source->data.resize(index_offset + entries * 16 + 4096);
    for (unsigned sector = 0; sector < sectors; ++sector) {
        std::memset(source->data.data() + 4096 + sector * 512,
                    sector % 251, 512);
    }
    format::lsmt::HeaderTrailer header;
    header.flags = 7;
    header.virtual_size = sectors * 512;
    header.index_offset = index_offset;
    header.index_size = entries;
    header.serialize(source->data.data());
    header.flags = 6;
    header.serialize(source->data.data() + index_offset + entries * 16);
    for (unsigned i = 0; i < entries; ++i) {
        bytes::segment_mapping mapping;
        mapping.offset = i * chunk_sectors;
        mapping.length = std::min(chunk_sectors, sectors - i * chunk_sectors);
        mapping.moffset = 8 + mapping.offset;
        bytes::store_segment_le(source->data.data() + index_offset + i * 16,
                                mapping);
    }
    auto layer = co_await format::LsmtLayer::open(std::move(source));
    std::vector<std::unique_ptr<format::LsmtLayer>> layers;
    layers.push_back(std::move(layer));
    auto view = co_await format::MergedLsmt::open(std::move(layers));
    std::array<uint8_t, 4096> output{};
    for (unsigned round = 0; round < 6; ++round) {
        const unsigned count = round == 0 ? warmup_iterations : iterations;
        uint64_t random = 12345;
        uint64_t checksum = 0;
        observation->calls = 0;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < count; ++i) {
            random = random * 6364136223846793005ULL + 1;
            const uint64_t sector = ((random >> 32) % 1024) * 8;
            const auto read = co_await view->pread(
                output.data(), output.size(), sector * 512);
            if (read != 4096) throw std::runtime_error("short merged read");
            for (unsigned j = 0; j < 8; ++j) {
                if (output[j * 512] != (sector + j) % 251)
                    throw std::runtime_error("incorrect sector payload");
            }
            checksum += output[0];
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (round == 0) continue;
        std::cout << "{\"chunk_sectors\":" << chunk_sectors
                  << ",\"round\":" << round - 1
                  << ",\"segments\":" << view->merged_index().size()
                  << ",\"ns_per_read\":" << double(elapsed) / iterations
                  << ",\"source_calls\":" << observation->calls
                  << ",\"checksum\":" << checksum << "}\n";
    }
}
}  // namespace

int main() {
    elio::run([]() -> elio::coro::task<void> {
        co_await measure(1);
        co_await measure(8);
        co_await measure(8192);
    }, {.num_threads = 1, .blocking_threads = 1});
}
