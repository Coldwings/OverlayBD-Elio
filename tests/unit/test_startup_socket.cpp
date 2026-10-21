#include "cmd/startup_socket.hpp"
#include "../support.hpp"

#include <elio/runtime/spawn.hpp>
#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>
#include <spawn.h>
#include <fcntl.h>

#include <csignal>
#include <fstream>
#include <new>
#include <sstream>
#include <thread>
#include <vector>

extern char** environ;

#ifdef OBD_TEST_HOOKS
namespace obd::cmd {
struct StartupSocketTestAccess {
    static void fail_join(StartupSocket& socket, bool& observed_live_client) {
        socket.join_context_ = &observed_live_client;
        socket.before_join_ = [](size_t outstanding, void* context) {
            *static_cast<bool*>(context) = outstanding != 0;
            throw std::bad_alloc();
        };
    }
};
} // namespace obd::cmd
#endif

namespace {
using nlohmann::json;
using namespace std::chrono_literals;
std::atomic<unsigned> socket_sequence{0};

std::string socket_name() {
    return "@obd-startup-test-" + std::to_string(::getpid()) + "-" +
           std::to_string(socket_sequence.fetch_add(1));
}

elio::net::unix_address address(const std::string& name) {
    return elio::net::unix_address::abstract(std::string_view(name).substr(1));
}

elio::coro::task<elio::net::uds_stream> connect(const std::string& name) {
    auto stream = co_await elio::net::uds_connect(address(name));
    if (!stream) throw std::runtime_error("startup test connect failed");
    co_return std::move(*stream);
}

elio::coro::task<void> send_command(elio::net::uds_stream& stream, const json& command) {
    const auto line = command.dump() + "\n";
    const auto result = co_await stream.write_exactly(line.data(), line.size());
    if (result.result != static_cast<ssize_t>(line.size()))
        throw std::runtime_error("startup test write failed");
}

// Read one byte at a time to leave the subsequent release outcome in the
// socket, rather than accidentally consuming two records in the first read.
elio::coro::task<json> read_line(elio::net::uds_stream& stream) {
    auto result = co_await elio::with_timeout(3s,
        [&](elio::coro::cancel_token token) -> elio::coro::task<json> {
            std::string line;
            char byte;
            while (line.size() < 8192) {
                const auto r = co_await stream.read(&byte, 1, token);
                if (r.result != 1) throw std::runtime_error("startup reply EOF");
                if (byte == '\n') co_return json::parse(line);
                line += byte;
            }
            throw std::runtime_error("oversized startup reply");
        });
    if (!result) throw std::runtime_error("startup reply timeout");
    co_return std::move(*result);
}

elio::coro::task<json> startup_exchange(const std::string& name, const json& command) {
    auto stream = co_await connect(name);
    co_await send_command(stream, command);
    co_return co_await read_line(stream);
}
} // namespace

TEST_CASE("device: standalone startup socket remains reachable with a stalled client",
          "[startup-socket]") {
    const auto name = socket_name();
    std::vector<json> replies;
    bool duplicate_refused = false;
    bool terminal_waited_for_cleanup = false;
    bool fail_join = false;
    bool observed_live_client = false;
#ifdef OBD_TEST_HOOKS
    SECTION("native join") {}
    SECTION("join observer setup failure retains client frames") { fail_join = true; }
#endif
    obd::test::run_coro([&]() -> elio::coro::task<void> {
        obd::ublk::StartupControl startup(91);
        obd::cmd::StartupSocket socket(name, startup);
#ifdef OBD_TEST_HOOKS
        if (fail_join)
            obd::cmd::StartupSocketTestAccess::fail_join(socket, observed_live_client);
#endif
        auto server = elio::spawn([&]() -> elio::coro::task<void> {
            co_await socket.run();
        });
        std::exception_ptr failure;
        std::optional<elio::net::uds_stream> stalled;
        try {
            try { obd::cmd::StartupSocket duplicate(name, startup); }
            catch (const std::system_error& error) {
                duplicate_refused = error.code().value() == EADDRINUSE;
            }
            stalled.emplace(co_await connect(name));
            const char partial = '{';
            (void)co_await stalled->write(&partial, 1);
            json command = {{"cmd", "startup_status"}};
            replies.push_back(co_await startup_exchange(name, command));
            command = {{"cmd", "startup_release"}, {"generation", 90}, {"seq", 1}};
            replies.push_back(co_await startup_exchange(name, command));
            command = {{"cmd", "startup_release"}, {"generation", 91}, {"seq", -1}};
            replies.push_back(co_await startup_exchange(name, command));
            command = {{"cmd", "startup_release"}, {"generation", 91}, {"seq", uint64_t{1} << 63}};
            replies.push_back(co_await startup_exchange(name, command));
            command = {{"cmd", "destroy"}};
            replies.push_back(co_await startup_exchange(name, command));
            auto release = co_await connect(name);
            command = {{"cmd", "startup_release"}, {"generation", 91}, {"seq", 7}};
            co_await send_command(release, command);
            replies.push_back(co_await read_line(release));
            command = {{"cmd", "startup_release"}, {"generation", 91}, {"seq", 7}};
            replies.push_back(co_await startup_exchange(name, command));
            command = {{"cmd", "startup_release"}, {"generation", 91}, {"seq", 8}};
            replies.push_back(co_await startup_exchange(name, command));
            // This fixture has no Device owner. Final caller cleanup must
            // settle the accepted request before the native socket frames drain.
            (void)startup.finish_before_start();
            co_await elio::time::sleep_for(30ms);
            char probe;
            const auto before_cleanup = ::recv(release.fd(), &probe, 1,
                                                 MSG_PEEK | MSG_DONTWAIT);
            terminal_waited_for_cleanup = before_cleanup < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK);
            socket.finish(startup.snapshot());
            replies.push_back(co_await read_line(release));
        } catch (...) { failure = std::current_exception(); }
        (void)startup.finish_before_start();
        socket.finish(startup.snapshot());
        co_await server.wait_destroyed_async();
        server.await_resume();
        if (failure) std::rethrow_exception(failure);
        // Listener closure happened after accept completion; another owner
        // can now bind the same abstract name without an unlink operation.
        obd::cmd::StartupSocket replacement(name, startup);
    });
    REQUIRE(duplicate_refused);
    REQUIRE(terminal_waited_for_cleanup);
    if (fail_join) REQUIRE(observed_live_client);
    REQUIRE(replies.size() == 9);
    REQUIRE(replies[0]["ok"] == true);
    REQUIRE(replies[0]["startup_owner"]["generation"] == 91);
    REQUIRE(replies[0]["startup_owner"]["phase"] == "preparing");
    for (size_t i = 1; i != 4; ++i) REQUIRE(replies[i]["release"] == "stale");
    REQUIRE(replies[4]["ok"] == false);
    REQUIRE(replies[5]["release"] == "pending");
    REQUIRE(replies[6]["release"] == "stale");
    REQUIRE(replies[7]["release"] == "busy");
    REQUIRE(replies[8]["release"] == "released");
    REQUIRE(replies[8]["startup_owner"]["phase"] == "released");
    REQUIRE(replies[8]["startup_owner"]["release_sequence"] == 7);
    REQUIRE(replies[8]["startup_owner"]["raw_published"] == 0);
}

TEST_CASE("device: standalone startup socket rejects filesystem paths",
          "[startup-socket]") {
    REQUIRE_FALSE(obd::cmd::StartupSocket::valid_name(""));
    REQUIRE_FALSE(obd::cmd::StartupSocket::valid_name("@"));
    REQUIRE_FALSE(obd::cmd::StartupSocket::valid_name("/tmp/never-unlink.sock"));
    REQUIRE_FALSE(obd::cmd::StartupSocket::valid_name("@" + std::string(108, 'x')));
    REQUIRE_FALSE(obd::cmd::StartupSocket::valid_name(std::string("@a\0b", 4)));
    REQUIRE(obd::cmd::StartupSocket::valid_name("@" + std::string(107, 'x')));
}

#ifdef OBD_TEST_DEVICE_BIN
TEST_CASE("device: standalone recovery assembly failure drains its control endpoint",
          "[startup-socket]") {
    bool fixture_enabled = false;
    SECTION("ordinary invocation") {}
    SECTION("fixture construction retains its directory through the blocking call") {
        fixture_enabled = true;
    }
    obd::test::TempDir dir;
    const auto name = socket_name();
    const auto log = (dir.path() / "device.log").string();
    const auto missing = (dir.path() / "missing-image.json").string();
    std::vector<std::string> arguments{OBD_TEST_DEVICE_BIN, "--config", missing,
        "--recover", "--dev-id", "7", "--startup-socket", name};
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    std::vector<std::string> environment;
    for (char** item = environ; *item; ++item)
        if (std::string_view(*item).find("OBD_TEST_STARTUP_FIXTURE_DIR=") != 0)
            environment.emplace_back(*item);
    if (fixture_enabled) {
        std::ofstream marker(dir.path() / "partial-recovery-v1");
        marker << "test\n";
        marker.close();
        REQUIRE(marker.good());
        environment.push_back("OBD_TEST_STARTUP_FIXTURE_DIR=" + dir.path().string());
    }
    std::vector<char*> envp;
    for (auto& item : environment) envp.push_back(item.data());
    envp.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    REQUIRE(::posix_spawn_file_actions_init(&actions) == 0);
    const int open_error = ::posix_spawn_file_actions_addopen(
        &actions, STDERR_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (open_error != 0) {
        ::posix_spawn_file_actions_destroy(&actions);
        FAIL("cannot capture standalone device stderr");
    }
    pid_t pid = -1;
    const int error = ::posix_spawn(&pid, OBD_TEST_DEVICE_BIN, &actions,
                                   nullptr, argv.data(), envp.data());
    ::posix_spawn_file_actions_destroy(&actions);
    REQUIRE(error == 0);
    int status = 0;
    bool reaped = false;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (::waitpid(pid, &status, WNOHANG) == pid) { reaped = true; break; }
        std::this_thread::sleep_for(10ms);
    }
    if (!reaped) {
        // Only this fixture's still-unreaped process is eligible for cleanup.
        ::kill(pid, SIGKILL);
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    REQUIRE(reaped);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 1);
    std::ifstream input(log);
    const std::string text{std::istreambuf_iterator<char>(input), {}};
    REQUIRE(text.find("startup control: " + name + " generation=") != std::string::npos);
    bool terminal = false;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        const auto record = json::parse(line, nullptr, false);
        if (record.is_object() && record.contains("startup_owner")) {
            const auto owner = record.at("startup_owner");
            terminal |= owner.at("phase") == "released" &&
                        owner.at("raw_published") == 0 &&
                        owner.at("generation").get<uint64_t>() != 0;
        }
    }
    REQUIRE(terminal);
    obd::test::run_coro([&]() -> elio::coro::task<void> {
        obd::ublk::StartupControl startup(92);
        obd::cmd::StartupSocket socket(name, startup);
        co_return;
    });
}
#endif
