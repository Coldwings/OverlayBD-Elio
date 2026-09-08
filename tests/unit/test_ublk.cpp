// Unit tests: ublk module — ABI geometry helpers (kernel-dependent E2E
// lives in tests/integration and self-skips without /dev/ublk-control).
#include "ublk/ctrl.hpp"
#include "ublk/queue.hpp"
#include "ublk/uapi_compat.hpp"

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace obd;

TEST_CASE("ublk: command buffer geometry matches the driver layout",
          "[ublk]") {
    // ublk_drv.c: stride = round_up(4096 * desc_size, PAGE); size =
    // round_up(depth * desc_size, PAGE). desc is 24B → stride 98304.
    REQUIRE(ublk::cmd_buf_stride(sizeof(ublksrv_io_desc)) == 98304);
    REQUIRE(ublk::cmd_buf_size(128, sizeof(ublksrv_io_desc)) == 4096);
    REQUIRE(ublk::cmd_buf_size(4096, sizeof(ublksrv_io_desc)) == 98304);
}

TEST_CASE("ublk: IoRequest byte math is sector based", "[ublk]") {
    ublk::IoRequest req;
    req.tag = 3;
    req.start_sector = 100;
    req.nr_sectors = 8;
    REQUIRE(req.byte_offset() == 51200);
    REQUIRE(req.byte_len() == 4096);
}

TEST_CASE("ublk: recovery feature flags follow device params", "[ublk]") {
    using obd::ublk::DeviceParams;
    DeviceParams p;
    p.enable_recovery = true;
    const uint64_t f = obd::ublk::dev_info_flags(p);
    REQUIRE((f & UBLK_F_USER_RECOVERY) != 0);
    REQUIRE((f & UBLK_F_USER_RECOVERY_REISSUE) != 0);
    p.enable_recovery = false;
    const uint64_t f2 = obd::ublk::dev_info_flags(p);
    REQUIRE((f2 & UBLK_F_USER_RECOVERY) == 0);
    REQUIRE((f2 & UBLK_F_USER_RECOVERY_REISSUE) == 0);
}
