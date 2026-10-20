#include "supervisor/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

using namespace obd::supervisor;
using nlohmann::json;

namespace {
StartupOwnerStatus retained_owner() {
    StartupOwnerStatus owner;
    owner.generation = 19;
    owner.revision = 4;
    owner.phase = "retained";
    owner.device_id = 7;
    owner.reason_errno = 5;
    owner.raw_published = 3;
    owner.raw_retired = 1;
    return owner;
}
} // namespace

TEST_CASE("supervisor: startup owner codec preserves exact identities",
          "[supervisor][startup-protocol]") {
    auto owner = retained_owner();
    owner.generation = std::numeric_limits<uint64_t>::max();
    owner.revision = std::numeric_limits<uint64_t>::max();
    owner.raw_published = std::numeric_limits<uint64_t>::max();
    owner.release_sequence = (uint64_t{1} << 63) - 1;
    owner.release = "refused";
    owner.release_errno = 16;
    const auto wire = make_device_status(
        DeviceStatus{"failed", "", "retained", owner});
    const auto decoded = parse_device_status(wire);
    REQUIRE(decoded);
    REQUIRE(decoded->startup_owner);
    REQUIRE(*decoded->startup_owner == owner);

    // JSON has no wire-level signed/unsigned integer distinction. A caller
    // constructing an equivalent object must get the same validation result.
    auto object = startup_owner_json(retained_owner());
    for (const char* key : {"generation", "revision", "raw_published",
                            "raw_retired", "release_sequence"}) {
        object[key] = object[key].get<int64_t>();
    }
    REQUIRE(parse_startup_owner(object) == retained_owner());
    object["extension"] = {{"ignored", true}};
    REQUIRE(parse_startup_owner(object) == retained_owner());
}

TEST_CASE("supervisor: malformed startup ownership never becomes legacy status",
          "[supervisor][startup-protocol]") {
    const auto good = startup_owner_json(retained_owner());
    for (auto it = good.begin(); it != good.end(); ++it) {
        INFO("missing field: " << it.key());
        auto incomplete = good;
        incomplete.erase(it.key());
        const json line = {{"state", "failed"}, {"startup_owner", incomplete}};
        REQUIRE_FALSE(parse_device_status(line.dump()));
    }
    for (const char* field : {"generation", "revision", "raw_published",
                              "raw_retired", "release_sequence"}) {
        for (const auto& bad : {json(-1), json(1.5), json(true), json("1"),
                               json(nullptr), json::array()}) {
            INFO("field: " << field << ", value: " << bad.dump());
            auto object = good;
            object[field] = bad;
            REQUIRE_FALSE(parse_startup_owner(object));
        }
    }
    for (const char* field : {"device_id", "reason_errno", "release_errno"}) {
        INFO("oversized field: " << field);
        auto object = good;
        object[field] = std::numeric_limits<uint64_t>::max();
        REQUIRE_FALSE(parse_startup_owner(object));
    }
    for (const char* field : {"control_pending", "kernel_debt_known"}) {
        auto object = good;
        object[field] = 1;
        REQUIRE_FALSE(parse_startup_owner(object));
    }
    for (const auto& patch : {
             json{{"generation", uint64_t{0}}},
             json{{"version", 2}},
             json{{"device_id", -2}},
             json{{"reason_errno", -1}},
             json{{"release_errno", -1}},
             json{{"release_sequence", uint64_t{1} << 63}},
             json{{"raw_retired", uint64_t{4}}},
             json{{"phase", "future_phase"}},
             json{{"release", "future_release"}}}) {
        auto object = good;
        object.update(patch);
        REQUIRE_FALSE(parse_startup_owner(object));
    }
}

TEST_CASE("supervisor: released startup status requires settled ownership facts",
          "[supervisor][startup-protocol]") {
    auto owner = retained_owner();
    owner.phase = "released";
    owner.release = "released";
    owner.release_sequence = 7;
    owner.raw_retired = owner.raw_published;
    const auto good = startup_owner_json(owner);
    REQUIRE(parse_startup_owner(good) == owner);

    // Matching counts alone are not a retirement assertion if commands or
    // unknown obligations remain. Parsing must not legitimize contradictions.
    for (const auto& patch : {
             json{{"control_pending", true}},
             json{{"kernel_debt_known", false}},
             json{{"raw_retired", uint64_t{2}}},
             json{{"release_sequence", uint64_t{0}}},
             json{{"release", "pending"}},
             json{{"release", "refused"}, {"release_errno", 5}},
             json{{"release", "none"}},
             json{{"phase", "retained"}},
             json{{"release_errno", 5}}}) {
        auto object = good;
        object.update(patch);
        INFO(patch.dump());
        REQUIRE_FALSE(parse_startup_owner(object));
    }
    // A safe failure before publication can finish without an operator
    // request; it is distinct from a matching release acknowledgement.
    owner.release = "none";
    owner.release_sequence = 0;
    REQUIRE(parse_startup_owner(startup_owner_json(owner)) == owner);
    owner.release_errno = 5;
    REQUIRE_FALSE(parse_startup_owner(startup_owner_json(owner)));
}

TEST_CASE("supervisor: legacy status remains separate from startup protection",
          "[supervisor][startup-protocol]") {
    const auto legacy_aggregate = make_device_status({"failed", "", "boom"});
    const auto aggregate_decoded = parse_device_status(legacy_aggregate);
    REQUIRE(aggregate_decoded);
    REQUIRE(aggregate_decoded->error == "boom");
    const auto legacy = parse_device_status(
        R"({"state":"ready","device":"/dev/ublkb7","extension":true})");
    REQUIRE(legacy);
    REQUIRE_FALSE(legacy->startup_owner);
    REQUIRE_FALSE(parse_device_status(
        R"({"state":"failed","startup_owner":null})"));
    REQUIRE_FALSE(parse_device_status(R"({"state":"failed","error":4})"));
    REQUIRE_FALSE(parse_device_status(R"({"state":"ready","device":[]})"));
}
