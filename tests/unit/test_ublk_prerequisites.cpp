#include "../ublk_prerequisites.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("ublk prerequisites: missing access or capability skips before setup",
          "[ublk-prerequisites]") {
    constexpr uint64_t admin = uint64_t{1} << CAP_SYS_ADMIN;
    for (const int error : {ENOENT, ENODEV, ENXIO}) {
        INFO("open errno: " << error);
        for (const uint64_t capabilities : {uint64_t{0}, admin}) {
            const auto reason =
                obd::test::ublk_prerequisite_skip_reason(error, capabilities);
            REQUIRE(reason.find("unavailable") != std::string::npos);
        }
    }
    for (const int error : {EACCES, EPERM}) {
        INFO("open errno: " << error);
        for (const uint64_t capabilities : {uint64_t{0}, admin}) {
            const auto reason =
                obd::test::ublk_prerequisite_skip_reason(error, capabilities);
            REQUIRE(reason.find("O_RDWR") != std::string::npos);
            REQUIRE(reason.find("permission denied") != std::string::npos);
        }
    }
    // Neither a UID check nor permitted/inheritable capabilities can admit
    // these tests: only the effective mask is supplied to the classifier.
    for (const uint64_t capabilities :
         {uint64_t{0}, uint64_t{1} << CAP_DAC_OVERRIDE, uint64_t{1} << 63}) {
        const auto reason =
            obd::test::ublk_prerequisite_skip_reason(0, capabilities);
        REQUIRE(reason.find("effective CAP_SYS_ADMIN") != std::string::npos);
    }
    REQUIRE(obd::test::ublk_prerequisite_skip_reason(0, admin).empty());
    REQUIRE(obd::test::ublk_prerequisite_skip_reason(
                0, admin | (uint64_t{1} << 63)).empty());
}

TEST_CASE("ublk prerequisites: unexpected probe errors remain failures",
          "[ublk-prerequisites]") {
    constexpr uint64_t admin = uint64_t{1} << CAP_SYS_ADMIN;
    for (const int error : {EIO, EMFILE, ENFILE, ENOMEM, EINVAL, ENOTDIR}) {
        INFO("open errno: " << error);
        for (const uint64_t capabilities : {uint64_t{0}, admin}) {
            try {
                obd::test::ublk_prerequisite_skip_reason(error, capabilities);
                FAIL("unexpected probe error was hidden as a skip");
            } catch (const std::system_error& failure) {
                REQUIRE(failure.code().value() == error);
            }
        }
    }
}
