#include "ublk/tag_engine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <thread>

TEST_CASE("ublk: foreign result mailbox enforces tag generations",
          "[ublk][tag-engine]") {
    using obd::ublk::ForeignResultMailbox;
    using obd::ublk::OwnerResult;

    CHECK_THROWS_AS(ForeignResultMailbox(0), std::invalid_argument);
    ForeignResultMailbox mailbox(2);
    OwnerResult result;

    CHECK_FALSE(mailbox.arm(2, 1));
    CHECK_FALSE(mailbox.arm(0, 0));
    CHECK_FALSE(mailbox.arm(0, std::numeric_limits<uint64_t>::max()));
    REQUIRE(mailbox.arm(0, 3));
    CHECK_FALSE(mailbox.retire(0, 4));
    REQUIRE(mailbox.retire(0, 3));
    CHECK_FALSE(mailbox.publish({0, 3, -EIO}));
    REQUIRE(mailbox.arm(1, 17));
    CHECK_FALSE(mailbox.arm(1, 18));
    CHECK_FALSE(mailbox.publish({1, 16, -EIO}));
    CHECK_FALSE(mailbox.take(1, 16, result));

    std::atomic<bool> accepted{false};
    std::jthread foreign([&] {
        accepted.store(mailbox.publish({1, 17, -ECANCELED}));
    });
    foreign.join();

    CHECK(accepted.load());
    CHECK_FALSE(mailbox.publish({1, 17, -EIO}));
    REQUIRE(mailbox.take(1, 17, result));
    CHECK(result.tag == 1);
    CHECK(result.generation == 17);
    CHECK(result.result == -ECANCELED);
    CHECK_FALSE(mailbox.take(1, 17, result));

    REQUIRE(mailbox.arm(1, 18));
    CHECK_FALSE(mailbox.publish({1, 17, -EIO}));
    REQUIRE(mailbox.publish({1, 18, 4096}));
    REQUIRE(mailbox.take(1, 18, result));
    CHECK((result == OwnerResult{1, 18, 4096}));
}
