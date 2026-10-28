// Bounded memory-source CPU experiment; input creation and loading are outside
// the timed region. Pass a ZFile and its uncompressed reference bytes.
#include "format/zfile.hpp"

#include <elio/runtime/async_main.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace obd;

std::vector<uint8_t> load(const char* path, size_t limit) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open benchmark input");
    const auto length = input.tellg();
    if (length < 0 || static_cast<uint64_t>(length) > limit)
        throw std::runtime_error("benchmark input exceeds size limit");
    std::vector<uint8_t> data(static_cast<size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    if (!input) throw std::runtime_error("short benchmark input");
    return data;
}

class MemorySource final : public source::BlobSource {
public:
    explicit MemorySource(std::vector<uint8_t> bytes) : data_(std::move(bytes)) {}
    elio::coro::task<ssize_t> pread(void* out, size_t count,
                                    uint64_t offset) override {
        if (offset >= data_.size()) co_return 0;
        count = std::min(count, data_.size() - offset);
        std::memcpy(out, data_.data() + offset, count);
        co_return static_cast<ssize_t>(count);
    }
    uint64_t size() const noexcept override { return data_.size(); }
    std::string_view label() const noexcept override { return "bench-memory"; }
private:
    std::vector<uint8_t> data_;
};

elio::coro::task<void> measure(std::vector<uint8_t> encoded,
                               const std::vector<uint8_t>& reference) {
    auto memory = std::make_unique<MemorySource>(std::move(encoded));
    auto source = co_await format::ZFileSource::open(std::move(memory), true);
    if (source->size() != reference.size() || reference.size() < 4096)
        throw std::runtime_error("reference size does not match ZFile");
    std::array<uint8_t, 4096> output{};
    for (unsigned length : {4096u, 512u}) {
        for (unsigned round = 0; round < 6; ++round) {
            const unsigned count = round == 0 ? 10000 : 50000;
            uint64_t random = 12345;
            uint64_t checksum = 0;
            const auto start = std::chrono::steady_clock::now();
            for (unsigned i = 0; i < count; ++i) {
                random = random * 6364136223846793005ULL + 1;
                const uint64_t offset =
                    ((random >> 32) % (reference.size() / 4096)) * 4096 +
                    (length == 512 ? 512 : 0);
                const auto read = co_await source->pread(
                    output.data(), length, offset);
                if (read != static_cast<ssize_t>(length) ||
                    output[0] != reference[offset] ||
                    output[length - 1] != reference[offset + length - 1])
                    throw std::runtime_error("incorrect decoded bytes");
                checksum += output[0];
            }
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count();
            if (round == 0) continue;
            std::cout << "{\"length\":" << length
                      << ",\"round\":" << round - 1
                      << ",\"ns_per_read\":" << double(elapsed) / count
                      << ",\"checksum\":" << checksum << "}\n";
        }
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: obd-bench-zfile-read compressed.zfile reference.raw\n";
        return 2;
    }
    try {
        auto encoded = load(argv[1], 32 * 1024 * 1024);
        const auto reference = load(argv[2], 16 * 1024 * 1024);
        elio::run([&]() -> elio::coro::task<void> {
            co_await measure(std::move(encoded), reference);
        }, {.num_threads = 1, .blocking_threads = 1});
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
