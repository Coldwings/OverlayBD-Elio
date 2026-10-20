#include "supervisor/protocol.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>

using namespace obd::supervisor;
using nlohmann::json;

TEST_CASE("supervisor: create validates device ids before integer narrowing",
          "[supervisor][protocol][device-id]") {
    for (const bool blank : {false, true}) {
        INFO("blank create: " << blank);
        json request = {{"cmd", "create"}, {"id", "range"}};
        if (blank) request["blank"] = {{"size", 512}};
        else request["config"] = "/unused";
        std::string error;
        const auto absent = parse_command(request.dump(), error);
        REQUIRE(absent);
        REQUIRE(absent->value("dev_id", -1) == -1);

        for (const int value : {-1, 0, 1, INT32_MAX}) {
            request["dev_id"] = value;
            const auto parsed = parse_command(request.dump(), error);
            REQUIRE(parsed);
            REQUIRE(parsed->value("dev_id", -1) == value);
        }
        for (const auto& value : {
                 json(-2),
                 json(std::numeric_limits<int64_t>::min()),
                 json(uint64_t{INT32_MAX} + 1),
                 json(uint64_t{UINT32_MAX}),
                 json(uint64_t{INT64_MAX}),
                 json(uint64_t{INT64_MAX} + 1),
                 json(std::numeric_limits<uint64_t>::max()),
                 json(1.0), json(-1.0), json(true), json("7"), json(nullptr)}) {
            INFO("invalid dev_id: " << value.dump());
            request["dev_id"] = value;
            error.clear();
            const auto parsed = parse_command(request.dump(), error);
            REQUIRE_FALSE(parsed);
            REQUIRE(error.find("dev_id") != std::string::npos);
        }
    }
}
