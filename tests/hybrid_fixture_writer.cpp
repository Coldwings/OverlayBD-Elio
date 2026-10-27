// Ordinary-file producer for independent upstream Hybrid compatibility checks.
#include "format/hybrid_writer.hpp"
#include "format/hybrid_rw.hpp"
#include "format/hybrid_transaction.hpp"
#include "common/errors.hpp"
#include <elio/runtime/spawn_blocking.hpp>
#include <elio/runtime/async_main.hpp>
#include <array>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "offline-seal") {
        const std::string data(argv[2]), index(argv[3]);
        return elio::run([&]() -> elio::coro::task<int> {
            std::string digest;
            uint64_t size = 0;
            const int rc = co_await obd::format::HybridRwLayer::seal_file(
                data, index, "", &digest, &size);
            if (rc != 0) co_return 1;
            std::printf("SEALED sha256=%s size=%llu\n", digest.c_str(),
                        static_cast<unsigned long long>(size));
            co_return 0;
        });
    }
    if (argc == 5 && (std::string(argv[1]) == "crash-grow" ||
                      std::string(argv[1]) == "crash-seal")) {
        if (argv[4][0] < '1' || argv[4][0] > '8' || argv[4][1] != '\0') return 64;
        const int stage = argv[4][0] - '0';
        const bool grow = std::string(argv[1]) == "crash-grow";
        const std::string data(argv[2]), index(argv[3]);
        try {
            return elio::run([&]() -> elio::coro::task<int> {
                auto pair = co_await obd::format::hybrid::FilePair::open_existing(data, index, true);
                obd::format::hybrid::Writer writer(std::move(pair));
                obd::format::hybrid::transaction_fault_for_test(stage, true);
                if (grow) {
                    auto* active = &writer;
                    const int rc = co_await elio::spawn_blocking([active] { return active->grow(65536); });
                    co_return rc == 0 ? 0 : 1;
                }
                const int rc = co_await writer.seal();
                co_return rc == 0 ? 0 : 1;
            });
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
    if (argc == 4 && std::string(argv[1]) == "recover") {
        const std::string data(argv[2]), index(argv[3]);
        try {
            return elio::run([&]() -> elio::coro::task<int> {
                auto pair = co_await obd::format::hybrid::FilePair::open_existing(data, index, true);
                std::printf("RECOVERED virtual_size=%llu\n", static_cast<unsigned long long>(pair->header().virtual_size));
                co_return 0;
            });
        } catch (const obd::error& error) {
            if (error.errno_value() == EALREADY) {
                std::puts("ALREADY_SEALED");
                return 0;
            }
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
    if (argc != 2 && !(argc == 4 && (std::string(argv[1]) == "compact" || std::string(argv[1]) == "seal"))) return 64;
    if (argc == 4) {
        const std::string data(argv[2]), index(argv[3]);
        const bool seal = std::string(argv[1]) == "seal";
        try {
            return elio::run([&]() -> elio::coro::task<int> {
                auto pair = co_await obd::format::hybrid::FilePair::open_existing(data, index, true);
                obd::format::hybrid::Writer writer(std::move(pair));
                const int compacted = seal ? co_await writer.seal() : co_await writer.compact();
                if (compacted != 0) co_return 1;
                const int flushed = co_await writer.flush();
                co_return flushed == 0 ? 0 : 1;
            });
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
    const std::string directory(argv[1]);
    try {
        return elio::run([&]() -> elio::coro::task<int> {
            auto pair = co_await obd::format::hybrid::FilePair::create(directory, 32768);
            obd::format::hybrid::Writer writer(std::move(pair));
            struct Op { uint64_t sector; size_t count; uint8_t value; bool zero; };
            const std::array<Op, 8> operations = {{{1,2,0x11,false},{8,3,0x22,false},
                {2,1,0x33,false},{9,1,0,true},{9,1,0x44,false},{10,1,0,true},
                {16,2,0,true},{63,1,0x77,false}}};
            for (const auto& op : operations) {
                if (op.zero) {
                    const int rc = co_await writer.discard(op.sector * 512, op.count * 512);
                    if (rc != 0) co_return 1;
                } else {
                    std::vector<uint8_t> bytes(op.count * 512, op.value);
                    const auto written = co_await writer.pwrite(bytes.data(), bytes.size(), op.sector * 512);
                    if (written != static_cast<ssize_t>(bytes.size())) co_return 1;
                }
            }
            const int rc = co_await writer.flush();
            co_return rc == 0 ? 0 : 1;
        });
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
