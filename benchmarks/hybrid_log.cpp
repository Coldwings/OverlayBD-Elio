// Controlled replay scaling and real automatic-compaction cadence.
#include "format/hybrid_writer.hpp"
#include <elio/runtime/async_main.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace obd;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void write(std::ofstream& out, const void* data, size_t size) {
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    check(out.good(), "fixture write failed");
}
// Explicit synthetic upstream-layout history: one live DATA mapping repeated
// N times. No local attributes are forged; first writable open adopts it.
void history(const fs::path& directory, uint64_t records) {
    check(records && records <= format::hybrid::kMaxLogRecords, "invalid history count");
    check(fs::create_directory(directory), "history directory exists");
    format::lsmt::HeaderTrailer header;
    header.flags = format::hybrid::kHybridFlag | 3u;
    header.version = header.sub_version = 1;
    header.virtual_size = 512;
    header.uuid = "14600000-0000-4000-8000-000000000001";
    std::array<uint8_t, 4096> raw{};
    header.serialize(raw.data());
    std::ofstream data(directory / "data", std::ios::binary);
    write(data, raw.data(), raw.size());
    std::array<uint8_t, 512> payload; payload.fill(0x5a);
    write(data, payload.data(), payload.size());
    data.close(); check(data.good(), "data close failed");
    header.flags = format::hybrid::kHybridFlag | 1u;
    header.index_offset = 4096;
    header.serialize(raw.data());
    std::ofstream index(directory / "index", std::ios::binary);
    write(index, raw.data(), raw.size());
    std::array<uint8_t, 65536> chunk{};
    for (size_t offset = 0; offset < chunk.size(); offset += 16)
        bytes::store_segment_le(chunk.data() + offset, {0, 1, 8, false, 0});
    for (uint64_t done = 0; done < records;) {
        const auto count = std::min<uint64_t>(records - done, chunk.size()/16);
        write(index, chunk.data(), static_cast<size_t>(count * 16)); done += count;
    }
    index.close(); check(index.good(), "index close failed");
}
elio::coro::task<int> replay(const std::string* directory, json* result) {
    for (int pass = 0; pass < 5; ++pass) {
        const auto start = Clock::now();
        auto pair = co_await format::hybrid::FilePair::open_existing(
            *directory + "/data", *directory + "/index", pass != 0);
        const auto ms = elapsed(start);
        check(pair->mappings().size() == 1, "history live mapping count changed");
        const auto mapping = pair->mappings().front();
        check(mapping.offset == 0 && mapping.length == 1 && mapping.moffset == 8 && !mapping.zeroed,
              "history recovered wrong mapping");
        if (pass != 0) {
            format::hybrid::Writer writer(std::move(pair));
            std::array<uint8_t, 512> actual{};
            const auto read = co_await writer.pread(actual.data(), actual.size(), 0);
            check(read == 512 && std::all_of(actual.begin(), actual.end(), [](uint8_t b) { return b == 0x5a; }),
                  "history recovered wrong bytes");
        }
        (*result)["opens"].push_back({{"kind", pass == 0 ? "unmanaged_readonly" : pass == 1 ? "first_writable_adoption" : "managed_writable"},
                                    {"wall_ms", ms}});
    }
    co_return 0;
}
elio::coro::task<int> cadence(const std::string* directory, uint64_t live, json* result) {
    auto pair = co_await format::hybrid::FilePair::create(*directory, live * 1024);
    auto* observed = pair.get();
    format::hybrid::Writer writer(std::move(pair));
    std::array<uint8_t, 512> payload; payload.fill(0x5a);
    for (uint64_t i = 0; i < live; ++i) {
        const auto rc = co_await writer.pwrite(payload.data(), payload.size(), i * 1024);
        check(rc == 512, "cadence initialization failed");
    }
    const auto physical = writer.physical_size();
    uint64_t maximum_records = (observed->index_bytes() - 4096) / 16;
    const auto start = Clock::now();
    for (uint64_t operation = 1; operation <= 8192; ++operation) {
        const auto generation = observed->index_generation();
        const auto before = (observed->index_bytes() - 4096) / 16;
        const auto count = writer.snapshot().size();
        check(count == live, "cadence live mapping count changed");
        const auto threshold = std::max<uint64_t>(4096, 2 * count + 16);
        if (operation % 2) {
            const int rc = co_await writer.discard(0, 512);
            check(rc == 0, "cadence discard failed");
        } else {
            const auto rc = co_await writer.pwrite(payload.data(), payload.size(), 0);
            check(rc == 512, "cadence rewrite failed");
        }
        const auto after = (observed->index_bytes() - 4096) / 16;
        maximum_records = std::max(maximum_records, after);
        check(writer.physical_size() == physical, "cadence payload grew during backing reuse");
        if (observed->index_generation() != generation) {
            check(before >= threshold && observed->index_generation() == generation + 1,
                  "unexpected compaction trigger");
            (*result)["compactions"].push_back({{"operation", operation}, {"before_records", before},
                {"after_records", after}, {"live_mappings", count}, {"threshold", threshold}});
        } else check(before < threshold, "threshold did not trigger compaction");
    }
    const int flushed = co_await writer.flush(); check(flushed == 0, "cadence flush failed");
    std::array<uint8_t, 512> actual{};
    const auto read = co_await writer.pread(actual.data(), actual.size(), 0);
    check(read == 512 && actual == payload, "cadence payload changed");
    (*result)["wall_ms_including_flush"] = elapsed(start);
    (*result)["operations"] = 8192;
    (*result)["max_records"] = maximum_records;
    (*result)["final_records"] = (observed->index_bytes() - 4096) / 16;
    (*result)["physical_payload_file_bytes"] = physical;
    check(result->contains("compactions") && !(*result)["compactions"].empty(), "no compaction observed");
    co_return 0;
}
int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: obd-bench-hybrid-log NEW_ABSOLUTE_DIRECTORY\n"; return 2; }
    const fs::path root(argv[1]);
    json report = {{"status", "running"}, {"history", "synthetic repeated valid mapping; one live sector; warm OS cache"}};
    try {
        check(root.is_absolute() && fs::create_directory(root), "refuse nonabsolute or existing output");
        elio::runtime::run_config config;
        config.num_threads = 2; config.blocking_threads = 2;
        config.shutdown_timeout = std::chrono::milliseconds::max();
        for (const uint64_t count : {uint64_t{1}, uint64_t{4096}, uint64_t{65536}, uint64_t{1048576}, format::hybrid::kMaxLogRecords}) {
            const auto path = root / ("history-" + std::to_string(count));
            history(path, count);
            const auto directory = path.string();
            json result = {{"records", count}, {"index_bytes", 4096 + count * 16}, {"live_mappings", 1}};
            check(elio::run(config, replay, &directory, &result) == 0, "replay run failed");
            report["replay"].push_back(std::move(result));
            std::cerr << "REPLAY passed records=" << count << '\n';
        }
        for (const uint64_t live : {uint64_t{1}, uint64_t{4096}}) {
            const auto directory = (root / ("cadence-" + std::to_string(live))).string();
            json result = {{"initial_live_mappings", live}};
            check(elio::run(config, cadence, &directory, live, &result) == 0, "cadence run failed");
            report["cadence"].push_back(std::move(result));
            std::cerr << "CADENCE passed live=" << live << '\n';
        }
        report["status"] = "passed";
    } catch (const std::exception& e) {
        report["status"] = "failed"; report["error"] = e.what();
        // Never write into a pre-existing caller directory on admission failure.
        std::cerr << report.dump(2) << '\n'; return 1;
    }
    std::ofstream out(root / "report.json"); out << report.dump(2) << '\n';
    return out.good() ? 0 : 1;
}
