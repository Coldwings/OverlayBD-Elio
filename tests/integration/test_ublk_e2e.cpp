// Integration test: ublk end-to-end. Requires /dev/ublk-control and
// CAP_SYS_ADMIN; self-skips otherwise (CI/dev sandboxes usually lack both).
#include "ublk/device.hpp"

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>

using namespace obd;

TEST_CASE("integration: ublk device serves sector reads from a blob",
          "[integration]") {
    if (::access("/dev/ublk-control", F_OK) != 0) {
        SKIP("/dev/ublk-control unavailable (kernel ublk not enabled)");
    }
    const int rc = test::run_coro([]() -> elio::coro::task<int> {
        auto data = test::pattern_bytes(512 * 64, 81);
        source::BlobSourcePtr src =
            std::make_unique<test::VectorSource>(data);
        ublk::DeviceParams params;
        params.dev_sectors = data.size() / 512;
        auto dev = co_await ublk::Device::create(params, std::move(src));
        const int fd = ::open(dev->bdev_path().c_str(), O_RDONLY);
        REQUIRE(fd >= 0);
        std::vector<uint8_t> buf(4096);
        REQUIRE(::pread(fd, buf.data(), buf.size(), 1024) == 4096);
        REQUIRE(buf == std::vector<uint8_t>(data.begin() + 1024,
                                            data.begin() + 1024 + 4096));
        ::close(fd);
        dev->stop();
        dev.reset();
        co_return 0;
    });
    REQUIRE(rc == 0);
}
