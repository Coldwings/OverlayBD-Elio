// Ordinary-file process-crash fixture. Exit after completed operations without
// FLUSH, checkpoint, object destruction or runtime shutdown.
#include "format/lsmt_rw.hpp"
#include "format/lsmt_rw_file.hpp"
#include <elio/runtime/async_main.hpp>
#include <elio/runtime/spawn_blocking.hpp>
#include <array>
#include <string>
#include <vector>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4 && argc != 5) return 64;
    const std::string data = argv[1], index = argv[2];
    elio::run_config runtime;
    runtime.num_threads = 2;
    runtime.blocking_threads = 1;
    return elio::run([&]() -> elio::coro::task<int> {
        if (argc == 4 && std::string(argv[3]) == "create-reference") {
            auto layer = co_await obd::format::LsmtRwLayer::create_persistent(data, index, 32768);
            struct Op { size_t sector, count; uint8_t value; bool zero; };
            const std::array<Op, 8> ops = {{{1,2,0x11,false}, {8,3,0x22,false},
                {2,1,0x33,false}, {9,1,0,true}, {9,1,0x44,false},
                {10,1,0,true}, {16,2,0,true}, {63,1,0x77,false}}};
            for (const auto& op : ops) {
                if (op.zero) {
                    if (co_await layer->discard(op.sector * 512, op.count * 512) != 0)
                        co_return 8;
                } else {
                    std::vector<uint8_t> bytes(op.count * 512, op.value);
                    if (co_await layer->pwrite(bytes.data(), bytes.size(), op.sector * 512)
                            != static_cast<ssize_t>(bytes.size())) co_return 9;
                }
            }
            ::_exit(73); // No flush or destructor before upstream reopens the pair.
        }
        if (argc == 4 && std::string(argv[3]) == "seal-reference") {
            co_return co_await obd::format::LsmtRwLayer::seal_file(data, "", nullptr, nullptr);
        }
#ifdef OBD_TEST_HOOKS
        if (argc == 5 && std::string(argv[3]) == "create-crash") {
            obd::format::lsmt_rw::create_fault_for_test(std::stoi(argv[4]), true);
            (void)co_await obd::format::LsmtRwLayer::create_persistent(data, index, 32768);
            co_return 6;
        }
        if (argc == 5 && std::string(argv[3]) == "migrate-crash") {
            obd::format::lsmt_rw::migration_fault_for_test(std::stoi(argv[4]), true);
            (void)co_await obd::format::LsmtRwLayer::migrate_checkpoint(data, index);
            co_return 7;
        }
#endif
        auto upper = co_await obd::format::LsmtRwLayer::open_existing(data, index);
#ifdef OBD_TEST_HOOKS
        if (argc == 5 && std::string(argv[3]) == "grow-crash") {
            obd::format::lsmt_rw::grow_fault_for_test(std::stoi(argv[4]), true);
            auto grow = [layer = upper.get()] { return layer->grow(65536); };
            (void)co_await elio::spawn_blocking(std::move(grow));
            co_return 5;
        }
#endif
        std::array<uint8_t, 512> bytes{};
        bytes.fill(0xa1);
        if (co_await upper->pwrite(bytes.data(), bytes.size(), 512) != 512) co_return 1;
        bytes.fill(0xb2);
        if (co_await upper->pwrite(bytes.data(), bytes.size(), 30 * 512) != 512) co_return 2;
        if (co_await upper->discard(2 * 512, 512) != 0) co_return 3;
        bytes.fill(0xc3);
        if (co_await upper->pwrite(bytes.data(), bytes.size(), 10 * 512) != 512) co_return 4;
        ::_exit(73);
    }, runtime);
}
