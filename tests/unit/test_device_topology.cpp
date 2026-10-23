#include "common/device_topology.hpp"
#include "common/errors.hpp"
#include "image/config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>

#ifdef OBD_TEST_DEVICE_BIN
#include "../support.hpp"
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

extern char** environ;
#endif

using namespace obd;

TEST_CASE("common: device topology matches automatic workers and queues", "[common]") {
    for (const auto cpus : {0u, 1u, 3u, 8u, 64u, 4096u}) {
        const auto topology = resolve_device_topology({}, {cpus, 0});
        CHECK(topology.queues == std::min(std::max(cpus, 1u), 8u));
        CHECK(topology.workers == topology.queues);
        CHECK_FALSE(topology.recovery);
    }
    const auto quota = resolve_device_topology({}, {64, 2});
    CHECK(quota.queues == 2);
    CHECK(quota.workers == 2);
    const auto affinity = resolve_device_topology({}, {1, 8});
    CHECK(affinity.queues == 1);
    CHECK(affinity.workers == 1);
}

TEST_CASE("common: device topology validates manual and recovery counts", "[common]") {
    const auto queues_only = resolve_device_topology({12, 0}, {2, 0});
    CHECK(queues_only.queues == 12);
    CHECK(queues_only.workers == 12);
    const auto workers_only = resolve_device_topology({0, 16}, {2, 0});
    CHECK(workers_only.queues == 16);
    CHECK(workers_only.workers == 16);
    const auto extra_workers = resolve_device_topology({4, 8}, {2, 0});
    CHECK(extra_workers.queues == 4);
    CHECK(extra_workers.workers == 8);
    CHECK_THROWS_AS(resolve_device_topology({4, 2}, {8, 0}), error);
    CHECK_THROWS_AS(resolve_device_topology({257, 0}, {8, 0}), error);
    CHECK_THROWS_AS(resolve_device_topology({0, 257}, {8, 0}), error);

    // A changed CPU budget must not resize an existing kernel device.
    const auto recovered = resolve_device_topology({}, {1, 1}, 12);
    CHECK(recovered.queues == 12);
    CHECK(recovered.workers == 12);
    CHECK(recovered.recovery);
    const auto recovered_extra = resolve_device_topology({4, 6}, {1, 1}, 4);
    CHECK(recovered_extra.queues == 4);
    CHECK(recovered_extra.workers == 6);
    CHECK_THROWS_AS(resolve_device_topology({2, 4}, {8, 0}, 4), error);
    CHECK_THROWS_AS(resolve_device_topology({0, 2}, {8, 0}, 4), error);
    CHECK_THROWS_AS(resolve_device_topology({}, {8, 0}, 0), error);
    CHECK_THROWS_AS(resolve_device_topology({}, {8, 0}, 257), error);
}

namespace {
DeviceCpuBudget fixture_budget(
    uint32_t cpus, std::string_view membership, std::string_view mounts,
    const std::map<std::string, std::string>& files) {
    return detail::device_cpu_budget_from_cgroups(cpus, membership, mounts,
        [&](const std::filesystem::path& path) -> std::optional<std::string> {
            const auto it = files.find(path.string());
            if (it == files.end()) return std::nullopt;
            return it->second;
        });
}
}  // namespace

TEST_CASE("common: CPU budget honors cgroup v2 ancestors and affinity", "[common]") {
    const std::string membership = "0::/tenant/app\n";
    const std::string mounts = "36 25 0:32 /tenant /cg/cpu rw - cgroup2 cgroup rw\n";
    const auto parent_limited = fixture_budget(8, membership, mounts, {
        {"/cg/cpu/app/cpu.max", "250000 100000\n"},
        {"/cg/cpu/cpu.max", "100000 100000\n"}});
    CHECK(parent_limited.quota_cpus == 1);
    CHECK(parent_limited.available_cpus() == 1);
    const auto fractional = fixture_budget(8, membership, mounts, {
        {"/cg/cpu/app/cpu.max", "150000 100000\n"},
        {"/cg/cpu/cpu.max", "max 100000\n"}});
    CHECK(fractional.quota_cpus == 2);
    CHECK(fractional.available_cpus() == 2);
    const auto affinity = fixture_budget(1, membership, mounts, {
        {"/cg/cpu/app/cpu.max", "800000 100000\n"}});
    CHECK(affinity.available_cpus() == 1);
}

TEST_CASE("common: CPU budget follows cgroup v1 mounted roots", "[common]") {
    const auto budget = fixture_budget(8,
        "3:cpuacct:/unrelated\n7:cpu,cpuacct:/service/work\n",
        "36 25 0:32 /service /cg/cpu\\040group rw - cgroup cgroup rw,cpu,cpuacct\n",
        {{"/cg/cpu group/work/cpu.cfs_quota_us", "150000\n"},
         {"/cg/cpu group/work/cpu.cfs_period_us", "100000\n"},
         {"/cg/cpu group/cpu.cfs_quota_us", "-1\n"},
         {"/cg/cpu group/cpu.cfs_period_us", "100000\n"}});
    CHECK(budget.quota_cpus == 2);
    CHECK(budget.available_cpus() == 2);
}

TEST_CASE("common: CPU budget ignores unavailable and malformed quota files", "[common]") {
    const auto missing = fixture_budget(3, "0::/\n",
        "36 25 0:32 / /cg rw - cgroup2 cgroup rw\n", {});
    CHECK(missing.affinity_cpus == 3);
    CHECK(missing.quota_cpus == 0);
    CHECK(missing.available_cpus() == 3);
    const auto malformed = fixture_budget(3, "0::/work\n",
        "36 25 0:32 / /cg rw - cgroup2 cgroup rw\n",
        {{"/cg/work/cpu.max", "100000 0"},
         {"/cg/cpu.max", "999999999999999999999999 100000"}});
    CHECK(malformed.available_cpus() == 3);
    const auto outside_mount = fixture_budget(3, "0::/other/work\n",
        "36 25 0:32 /tenant /cg rw - cgroup2 cgroup rw\n",
        {{"/other/work/cpu.max", "1 100000"}});
    CHECK(outside_mount.quota_cpus == 0);
    const auto outside_namespace = fixture_budget(3, "0::/\n",
        "36 25 0:32 /.. /cg rw - cgroup2 cgroup rw\n",
        {{"/cg/cpu.max", "1 100000"}});
    CHECK(outside_namespace.quota_cpus == 0);
    const auto outside_membership = fixture_budget(3, "0::/../other\n",
        "36 25 0:32 / /cg rw - cgroup2 cgroup rw\n",
        {{"/cg/other/cpu.max", "1 100000"}});
    CHECK(outside_membership.quota_cpus == 0);
}

TEST_CASE("image: global config validates device queue and worker settings", "[image]") {
    const auto automatic = image::GlobalConfig::from_json_text("{}");
    CHECK(automatic.device_topology.queues == 0);
    CHECK(automatic.device_topology.workers == 0);
    const auto manual = image::GlobalConfig::from_json_text(
        R"({"ublkConfig":{"queues":4,"workers":8,"enableRecovery":false,"futureField":1}})");
    CHECK(manual.device_topology.queues == 4);
    CHECK(manual.device_topology.workers == 8);
    CHECK_FALSE(manual.ublk_recovery);
    for (const auto text : {
        R"({"ublkConfig":{"queues":4,"workers":2}})",
        R"({"ublkConfig":{"queues":-1}})",
        R"({"ublkConfig":{"workers":257}})",
        R"({"ublkConfig":{"queues":4294967296}})",
        R"({"ublkConfig":{"queues":true}})",
        R"({"ublkConfig":{"workers":1.5}})",
        R"({"ublkConfig":{"queues":"4"}})",
        R"({"ublkConfig":{"workers":null}})"}) {
        INFO(text);
        CHECK_THROWS_AS(image::GlobalConfig::from_json_text(text), DeviceTopologyError);
    }
}

#ifdef OBD_TEST_DEVICE_BIN
namespace {
struct DeviceCapture {
    bool completed;
    int exit_code;
    std::string output;
};

DeviceCapture run_device(std::vector<std::string> arguments,
                         const std::string& output_path, bool supervised = false) {
    struct Fd {
        int value;
        ~Fd() { if (value >= 0) ::close(value); }
    } output{::open(output_path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600)};
    REQUIRE(output.value >= 0);
    Fd parent{-1}, device{-1};
    constexpr int control_fd = 198;
    if (supervised) {
        int pair[2];
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
        parent.value = pair[0];
        device.value = pair[1];
        arguments.insert(arguments.end(), {"--control-fd", std::to_string(control_fd),
                                           "--startup-generation", "77"});
        // Queue the ordinary supervisor grant before spawning. The real child
        // still parses it through its production control-session coroutine.
        const std::string grant =
            "{\"cmd\":\"startup_begin\",\"version\":1,\"generation\":77}\n";
        REQUIRE(::send(parent.value, grant.data(), grant.size(), MSG_NOSIGNAL) ==
                static_cast<ssize_t>(grant.size()));
    }
    std::vector<char*> argv{const_cast<char*>(OBD_TEST_DEVICE_BIN)};
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    const int initialized = ::posix_spawn_file_actions_init(&actions);
    int rc = initialized;
    if (!rc) rc = ::posix_spawn_file_actions_adddup2(&actions, output.value, STDOUT_FILENO);
    if (!rc) rc = ::posix_spawn_file_actions_adddup2(&actions, output.value, STDERR_FILENO);
    if (!rc && output.value != STDOUT_FILENO && output.value != STDERR_FILENO)
        rc = ::posix_spawn_file_actions_addclose(&actions, output.value);
    if (!rc && supervised) rc = ::posix_spawn_file_actions_adddup2(&actions, device.value, control_fd);
    if (!rc && supervised && device.value != control_fd)
        rc = ::posix_spawn_file_actions_addclose(&actions, device.value);
    pid_t child = -1;
    if (!rc) rc = ::posix_spawn(&child, OBD_TEST_DEVICE_BIN, &actions, nullptr, argv.data(), environ);
    if (!initialized) ::posix_spawn_file_actions_destroy(&actions);
    REQUIRE(rc == 0);
    if (device.value >= 0) { ::close(device.value); device.value = -1; }

    // Wait on this owned child's exit event; a timeout reaps it before any
    // assertion can unwind the fixture. No host device is created by callers.
    Fd exited{static_cast<int>(::syscall(SYS_pidfd_open, child, 0))};
    pollfd event{exited.value, POLLIN, 0};
    int observed = -1;
    if (exited.value >= 0) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) break;
            observed = ::poll(&event, 1, static_cast<int>(remaining));
        } while (observed < 0 && errno == EINTR);
    }
    const bool completed = observed == 1 && (event.revents & POLLIN);
    if (!completed) (void)::kill(child, SIGKILL);
    int status = 0;
    pid_t reaped;
    do { reaped = ::waitpid(child, &status, 0); } while (reaped < 0 && errno == EINTR);
    REQUIRE(reaped == child);
    std::ifstream file(output_path);
    return {completed, WIFEXITED(status) ? WEXITSTATUS(status) : -1,
            std::string(std::istreambuf_iterator<char>(file), {})};
}

std::string global_file(test::TempDir& tmp, const std::string& text) {
    return test::write_file(tmp / "global.json",
        std::vector<uint8_t>(text.begin(), text.end()));
}
}  // namespace

TEST_CASE("cli: device topology config sets the actual runtime worker count", "[cli]") {
    test::TempDir tmp;
    const auto global = global_file(tmp, R"({"ublkConfig":{"queues":2,"workers":4}})");
    const auto manual = run_device({"--config", tmp / "missing-image.json", "--global", global},
                                   tmp / "manual.log");
    INFO(manual.output);
    CHECK(manual.completed);
    CHECK(manual.exit_code == 1);
    CHECK(manual.output.find("requested_queues=2 requested_workers=4 resolved_queues=2 workers=4") != std::string::npos);

    const auto automatic = run_device({"--config", tmp / "missing-image.json"}, tmp / "automatic.log");
    INFO(automatic.output);
    CHECK(automatic.completed);
    CHECK(automatic.exit_code == 1);
    const auto start = automatic.output.find("device topology: ");
    REQUIRE(start != std::string::npos);
    unsigned requested_queues = 1, requested_workers = 1, queues = 0;
    size_t workers = 0;
    const int fields = std::sscanf(automatic.output.c_str() + start,
        "device topology: requested_queues=%u requested_workers=%u resolved_queues=%u workers=%zu",
        &requested_queues, &requested_workers, &queues, &workers);
    REQUIRE(fields == 4);
    CHECK(requested_queues == 0);
    CHECK(requested_workers == 0);
    CHECK(queues >= 1);
    CHECK(queues <= 8);
    CHECK(workers == queues);
}

TEST_CASE("cli: device topology is validated before image or blank startup", "[cli]") {
    test::TempDir tmp;
    const auto global = global_file(tmp, R"({"ublkConfig":{"queues":4,"workers":2}})");
    const auto image = run_device({"--config", tmp / "missing-image.json", "--global", global},
                                  tmp / "image.log");
    const auto blank = run_device({"--blank-size", "4096", "--blank-dir", tmp / "blank",
                                  "--global", global}, tmp / "blank.log");
    for (const auto* result : {&image, &blank}) {
        INFO(result->output);
        CHECK(result->completed);
        CHECK(result->exit_code == 2);
        CHECK(result->output.find("workers must be at least queues") != std::string::npos);
        CHECK(result->output.find("device topology: ") == std::string::npos);
        CHECK(result->output.find("startup control:") == std::string::npos);
    }
    CHECK_FALSE(std::filesystem::exists(tmp / "blank"));
}

TEST_CASE("cli: device topology reaches blank and supervised startup", "[cli]") {
    test::TempDir tmp;
    const auto global = global_file(tmp, R"({"ublkConfig":{"queues":2,"workers":4}})");
    const auto occupied = test::write_file(tmp / "occupied", std::vector<uint8_t>{1});
    unsigned invocation = 0;
    for (const bool supervised : {false, true}) {
        for (const bool blank : {false, true}) {
            auto arguments = blank
                ? std::vector<std::string>{"--blank-size", "4096", "--blank-dir", occupied}
                : std::vector<std::string>{"--config", tmp / "missing-image.json"};
            arguments.insert(arguments.end(), {"--global", global});
            const auto result = run_device(std::move(arguments),
                tmp / ("startup-" + std::to_string(invocation++) + ".log"), supervised);
            INFO("supervised=" << supervised << ", blank=" << blank << '\n' << result.output);
            CHECK(result.completed);
            CHECK(result.exit_code == 1);
            CHECK(result.output.find("requested_queues=2 requested_workers=4 resolved_queues=2 workers=4") != std::string::npos);
            // Both assembly failures happen before Device construction, so this
            // composition test never probes or creates a host ublk device.
            CHECK(result.output.find(blank ? "cannot create blank device workspace" : "missing-image.json") != std::string::npos);
            CHECK(result.output.find("parent channel closed before startup grant") == std::string::npos);
        }
    }
}

TEST_CASE("cli: device preserves operational global config failure cleanup", "[cli]") {
    test::TempDir tmp;
    const auto malformed = global_file(tmp, "{");
    for (const auto& [global, log] : std::vector<std::pair<std::string, std::string>>{
        {tmp / "missing-global.json", tmp / "missing-global.log"},
        {malformed, tmp / "malformed-global.log"}}) {
        const auto result = run_device({"--config", tmp / "missing-image.json", "--global", global}, log);
        INFO(result.output);
        CHECK(result.completed);
        CHECK(result.exit_code == 1);
        CHECK(result.output.find("startup control:") != std::string::npos);
        CHECK(result.output.find("unresolved (startup preflight unavailable)") != std::string::npos);
    }
}
#endif
