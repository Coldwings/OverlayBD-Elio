// Unit tests: supervisor module — protocol and child lifecycle with fake
// device binaries.
#include "supervisor/child.hpp"
#include "supervisor/daemon.hpp"
#include "supervisor/protocol.hpp"

#include "../support.hpp"
#include "../runtime_faults.hpp"
#include "../child_reap_evidence.hpp"

#include <elio/coro/task_group.hpp>
#include <elio/runtime/spawn_blocking.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <csignal>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace obd;

namespace {

bool reap_child(supervisor::Child& child) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        child.try_reap();
        if (child.reaped()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return child.reaped();
}

class ReapFixtureEnvironment {
public:
    explicit ReapFixtureEnvironment(const std::string& directory) {
        if (const char* previous = std::getenv("OBD_TEST_STARTUP_FIXTURE_DIR"))
            previous_ = previous;
        if (::setenv("OBD_TEST_STARTUP_FIXTURE_DIR", directory.c_str(), 1) != 0)
            throw std::system_error(errno, std::generic_category(), "set reap fixture environment");
    }
    ~ReapFixtureEnvironment() {
        if (previous_) ::setenv("OBD_TEST_STARTUP_FIXTURE_DIR", previous_->c_str(), 1);
        else ::unsetenv("OBD_TEST_STARTUP_FIXTURE_DIR");
    }
private:
    std::optional<std::string> previous_;
};

}  // namespace

TEST_CASE("supervisor: protocol commands parse and reject garbage",
          "[supervisor]") {
    std::string err;
    auto j = supervisor::parse_command(
        R"({"cmd":"create","id":"a","config":"/c.json"})", err);
    REQUIRE(j.has_value());
    REQUIRE((*j)["cmd"] == "create");
    REQUIRE(!supervisor::parse_command(R"({"cmd":"create","id":"a"})",
                                       err)
                 .has_value());
    REQUIRE(!supervisor::parse_command(R"({"cmd":"bogus"})", err).has_value());
    // 'dev_id' range: a JSON integer outside int must be a clean parse
    // error here — the handler's get<int>() would otherwise throw out of
    // the handler and answer "internal error" instead of a protocol error.
    REQUIRE(supervisor::parse_command(
                R"({"cmd":"create","id":"a","config":"/c.json","dev_id":7})",
                err)
                .has_value());
    REQUIRE(!supervisor::parse_command(
                 R"({"cmd":"create","id":"a","config":"/c.json","dev_id":4294967296})",
                 err)
                 .has_value());
    REQUIRE(err.find("dev_id") != std::string::npos);
    REQUIRE(!supervisor::parse_command(
                 R"({"cmd":"create","id":"a","config":"/c.json","dev_id":-2})",
                 err)
                 .has_value());
    REQUIRE(err.find("dev_id") != std::string::npos);
    REQUIRE(!supervisor::parse_command("not json", err).has_value());
    REQUIRE(supervisor::parse_command(R"({"cmd":"list"})", err).has_value());
    REQUIRE(supervisor::parse_command(R"({"cmd":"status","id":"a"})", err)
                .has_value());

    const auto st = supervisor::parse_device_status(
        R"({"state":"ready","device":"/dev/ublkb3"})");
    REQUIRE(st.has_value());
    REQUIRE(st->state == "ready");
    REQUIRE(st->device == "/dev/ublkb3");
    REQUIRE(!supervisor::parse_device_status("{}").has_value());

    const std::string line =
        supervisor::make_device_status({"failed", "", "boom"});
    const auto rt = supervisor::parse_device_status(line);
    REQUIRE(rt.has_value());
    REQUIRE(rt->state == "failed");
    REQUIRE(rt->error == "boom");
}

TEST_CASE("supervisor: hello handshake replies with protocol version and features",
          "[supervisor]") {
    std::string err;
    // hello needs no other fields; extra fields are ignored (additive-only
    // rule: servers ignore unknown request fields).
    REQUIRE(supervisor::parse_command(R"({"cmd":"hello"})", err).has_value());
    REQUIRE(supervisor::parse_command(
                R"({"cmd":"hello","future_field":42})", err)
                .has_value());

    // contains() before operator[]: a broken implementation must fail the
    // test cleanly, not hit UB on a missing key.
    const auto reply =
        nlohmann::json::parse(supervisor::reply_hello());
    REQUIRE(reply.value("ok", false) == true);
    REQUIRE(reply.contains("protocol"));
    REQUIRE(reply["protocol"].is_number_integer());
    REQUIRE(reply["protocol"].get<int>() >= 1);
    REQUIRE(reply["protocol"].get<int>() ==
            supervisor::kProtocolVersion);
    REQUIRE(reply.contains("version"));
    REQUIRE(reply["version"].is_string());
    REQUIRE(!reply["version"].get<std::string>().empty());
    REQUIRE(reply.contains("features"));
    REQUIRE(reply["features"].is_array());
    REQUIRE(std::find(reply["features"].begin(), reply["features"].end(),
                      nlohmann::json("hybrid_upper")) != reply["features"].end());

    // Bad input is still answered, never dropped: unknown cmd and malformed
    // JSON are rejected with a reason the daemon can reply with.
    REQUIRE(!supervisor::parse_command(R"({"cmd":"bogus"})", err).has_value());
    REQUIRE(err.find("unknown cmd") != std::string::npos);
    REQUIRE(!supervisor::parse_command("not json", err).has_value());
    REQUIRE(err.find("malformed JSON") != std::string::npos);
    const auto err_reply = nlohmann::json::parse(supervisor::reply_error(err));
    REQUIRE(err_reply.value("ok", true) == false);
    REQUIRE(err_reply.contains("error"));
    REQUIRE(err_reply["error"].is_string());
}

TEST_CASE("supervisor: child spawn execs and reports through the channel",
          "[supervisor]") {
    // Fake obd-device: /bin/sh -c that writes a ready line to fd 3.
    // (child.cpp execs the binary directly, so wrap with /bin/sh via a
    // small script file.)
    test::TempDir dir;
    const std::string script = dir / "fake-device.sh";
    {
        const std::string content =
            "#!/bin/sh\necho '{\"state\":\"ready\",\"device\":\"/dev/ublkb9\"}' >&3\nexit 0\n";
        test::write_file(script,
                         std::vector<uint8_t>(content.begin(), content.end()));
        ::chmod(script.c_str(), 0755);
    }
    supervisor::ChildSpec spec;
    spec.id = "fake";
    spec.device_bin = script;
    spec.config_path = "/nonexistent-config.json";  // ignored by the fake
    auto child = supervisor::Child::spawn(spec);
    REQUIRE(child->pid() > 0);
    REQUIRE(child->control_fd() >= 0);

    // Read the status line the fake wrote (control fd is nonblocking).
    std::string line;
    char buf[256];
    for (;;) {
        pollfd pfd{child->control_fd(), POLLIN, 0};
        REQUIRE(::poll(&pfd, 1, 5000) == 1);
        const ssize_t r = ::read(child->control_fd(), buf, sizeof(buf));
        if (r <= 0) break;
        line.append(buf, static_cast<size_t>(r));
        if (line.find('\n') != std::string::npos) break;
    }
    REQUIRE(line.find("\"ready\"") != std::string::npos);
    const auto st = supervisor::parse_device_status(line);
    REQUIRE(st.has_value());
    REQUIRE(st->device == "/dev/ublkb9");

    REQUIRE(reap_child(*child));
    REQUIRE(child->status().state == "exited");
    REQUIRE(child->status().exit_code == 0);
    ::close(child->release_control_fd());
}

TEST_CASE("supervisor: commit command parses and validates its fields",
          "[supervisor]") {
    // ADR-0014 additive protocol: commit requires `id`; `user_tag` is
    // optional; unknown fields are ignored.
    std::string err;
    auto j = supervisor::parse_command(R"({"cmd":"commit","id":"a"})", err);
    REQUIRE(j.has_value());
    REQUIRE((*j)["cmd"] == "commit");
    REQUIRE(supervisor::parse_command(
                R"({"cmd":"commit","id":"a","user_tag":"v1","future":1})",
                err)
                .has_value());
    REQUIRE(!supervisor::parse_command(R"({"cmd":"commit"})", err)
                 .has_value());
    REQUIRE(err.find("commit requires 'id'") != std::string::npos);

    // Malformed field TYPES are clean protocol errors at parse time —
    // they must not reach the handler (which would throw a
    // json::type_error out of get<std::string>()).
    REQUIRE(!supervisor::parse_command(R"({"cmd":"commit","id":123})", err)
                 .has_value());
    REQUIRE(err.find("must be a string") != std::string::npos);
    REQUIRE(!supervisor::parse_command(
                R"({"cmd":"commit","id":"a","user_tag":42})", err)
                 .has_value());
    REQUIRE(err.find("user_tag") != std::string::npos);

    // The hello handshake advertises the capability gate for commit —
    // including the protocol version field itself (clients gate on both).
    const auto hello = nlohmann::json::parse(supervisor::reply_hello());
    REQUIRE(hello.value("ok", false) == true);
    REQUIRE(hello.contains("protocol"));
    REQUIRE(hello["protocol"].get<int>() == supervisor::kProtocolVersion);
    REQUIRE((hello.contains("features") && hello["features"].is_array()));
    const auto features =
        hello["features"].get<std::vector<std::string>>();
    REQUIRE(std::find(features.begin(), features.end(), "commit") !=
            features.end());
}

TEST_CASE("supervisor: resize command parses and validates its fields",
          "[supervisor]") {
    // D3 additive protocol: resize requires a string `id` and a
    // non-negative integer `size` (bytes); unknown fields are ignored.
    // Semantic rules (positive, 512-aligned, grow-only) live in the
    // handlers, where the current size is known.
    std::string err;
    auto j = supervisor::parse_command(
        R"({"cmd":"resize","id":"a","size":65536})", err);
    REQUIRE(j.has_value());
    REQUIRE((*j)["cmd"] == "resize");
    REQUIRE((*j)["size"] == 65536);
    REQUIRE(supervisor::parse_command(
                R"({"cmd":"resize","id":"a","size":1,"future":1})", err)
                .has_value());
    REQUIRE(!supervisor::parse_command(R"({"cmd":"resize","id":"a"})", err)
                 .has_value());
    REQUIRE(err.find("resize requires 'id' and 'size'") !=
            std::string::npos);
    REQUIRE(!supervisor::parse_command(R"({"cmd":"resize","size":1})", err)
                 .has_value());
    REQUIRE(err.find("resize requires 'id' and 'size'") !=
            std::string::npos);

    // Malformed field TYPES are clean parse-time errors: a string or
    // float `size`, or a negative size (a shrink cannot even be
    // expressed), never reach the handler. (A small positive integer is
    // fine at parse time — positivity/alignment are handler rules.)
    REQUIRE(supervisor::parse_command(
                R"({"cmd":"resize","id":"a","size":123})", err)
                .has_value());
    REQUIRE(!supervisor::parse_command(
                R"({"cmd":"resize","id":"a","size":"big"})", err)
                 .has_value());
    REQUIRE(err.find("non-negative integer") != std::string::npos);
    REQUIRE(!supervisor::parse_command(
                R"({"cmd":"resize","id":"a","size":1.5})", err)
                 .has_value());
    REQUIRE(err.find("non-negative integer") != std::string::npos);
    REQUIRE(!supervisor::parse_command(
                R"({"cmd":"resize","id":"a","size":-1})", err)
                 .has_value());
    REQUIRE(err.find("non-negative integer") != std::string::npos);

    // The hello handshake advertises the capability gate for resize,
    // with the protocol version bumped by the D3 batch.
    const auto hello = nlohmann::json::parse(supervisor::reply_hello());
    REQUIRE(hello.value("ok", false) == true);
    REQUIRE(hello["protocol"].get<int>() == supervisor::kProtocolVersion);
    const auto features =
        hello["features"].get<std::vector<std::string>>();
    REQUIRE(std::find(features.begin(), features.end(), "resize") !=
            features.end());
}

TEST_CASE("supervisor: create virtual_size override parses and validates",
          "[supervisor]") {
    // D3 headroom: create's optional `virtual_size` (bytes) must be a
    // non-negative integer; other types are clean parse-time errors.
    std::string err;
    REQUIRE(supervisor::parse_command(
                R"({"cmd":"create","id":"a","config":"/c.json",
                    "virtual_size":65536})",
                err)
                .has_value());
    REQUIRE(supervisor::parse_command(
                R"({"cmd":"create","id":"a","config":"/c.json"})", err)
                .has_value());
    REQUIRE(!supervisor::parse_command(
                R"({"cmd":"create","id":"a","config":"/c.json",
                    "virtual_size":"big"})",
                err)
                 .has_value());
    REQUIRE(err.find("non-negative integer") != std::string::npos);
    REQUIRE(!supervisor::parse_command(
                R"({"cmd":"create","id":"a","config":"/c.json",
                    "virtual_size":-1})",
                err)
                 .has_value());
    REQUIRE(err.find("non-negative integer") != std::string::npos);
}

TEST_CASE("supervisor: commit virtual_size override parses and validates",
          "[supervisor]") {
    // D3 commit re-baseline: commit's optional `virtual_size` (bytes)
    // must be a non-negative integer; other types are clean parse-time
    // errors (the grow-only rules need the checkpoint, so they live in
    // the seal path).
    std::string err;
    REQUIRE(supervisor::parse_command(
                R"({"cmd":"commit","id":"a","virtual_size":65536})", err)
                .has_value());
    REQUIRE(supervisor::parse_command(
                R"({"cmd":"commit","id":"a","user_tag":"v1",
                    "virtual_size":65536})",
                err)
                .has_value());
    REQUIRE(!supervisor::parse_command(
                R"({"cmd":"commit","id":"a","virtual_size":"big"})", err)
                 .has_value());
    REQUIRE(err.find("non-negative integer") != std::string::npos);
    REQUIRE(!supervisor::parse_command(
                R"({"cmd":"commit","id":"a","virtual_size":-1})", err)
                 .has_value());
    REQUIRE(err.find("non-negative integer") != std::string::npos);
}

TEST_CASE("supervisor: bdev path parses to device id", "[supervisor]") {
    REQUIRE(supervisor::dev_id_from_bdev_path("/dev/ublkb7") == 7);
    REQUIRE(supervisor::dev_id_from_bdev_path("/dev/ublkb0") == 0);
    REQUIRE(supervisor::dev_id_from_bdev_path("/dev/ublkb123") == 123);
    REQUIRE(supervisor::dev_id_from_bdev_path("/dev/ublkc7") == -1);
    REQUIRE(supervisor::dev_id_from_bdev_path("/dev/ublkb") == -1);
    REQUIRE(supervisor::dev_id_from_bdev_path("/dev/ublkb7x") == -1);
    REQUIRE(supervisor::dev_id_from_bdev_path("") == -1);
}

TEST_CASE("supervisor: recover spec adds the recover flag to child argv",
          "[supervisor]") {
    // ADR-0010: a recovery respawn must pass --recover (and the dev id) to
    // obd-device. The fake device logs its argv for inspection.
    test::TempDir dir;
    const std::string log = dir / "argv.log";
    const std::string script = dir / "fake-device.sh";
    {
        const std::string content =
            "#!/bin/sh\necho \"$@\" > \"" + log + "\"\nexit 0\n";
        test::write_file(script,
                         std::vector<uint8_t>(content.begin(), content.end()));
        ::chmod(script.c_str(), 0755);
    }
    supervisor::ChildSpec spec;
    spec.id = "fake-recover";
    spec.device_bin = script;
    spec.config_path = "/c.json";
    spec.dev_id_request = 7;
    spec.recover = true;
    auto child = supervisor::Child::spawn(spec);
    REQUIRE(child->pid() > 0);
    REQUIRE(reap_child(*child));
    ::close(child->release_control_fd());

    struct stat st {};
    REQUIRE(::stat(log.c_str(), &st) == 0);
    std::string argv(static_cast<size_t>(st.st_size), '\0');
    const int fd = ::open(log.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    REQUIRE(::read(fd, argv.data(), argv.size()) ==
            static_cast<ssize_t>(argv.size()));
    ::close(fd);
    REQUIRE(argv.find("--recover") != std::string::npos);
    REQUIRE(argv.find("--dev-id 7") != std::string::npos);
    REQUIRE(argv.find("--config /c.json") != std::string::npos);
}

TEST_CASE("supervisor: exec failure surfaces as exit 127", "[supervisor]") {
    supervisor::ChildSpec spec;
    spec.id = "missing";
    spec.device_bin = "/definitely/not/a/binary";
    spec.config_path = "/x";
    auto child = supervisor::Child::spawn(spec);
    REQUIRE(reap_child(*child));
    REQUIRE(child->status().exit_code == 127);
    REQUIRE(child->status().error.find("exec failed") !=
            std::string::npos);
    ::close(child->release_control_fd());
}

TEST_CASE("supervisor: reap evidence preserves actual child wait status",
          "[supervisor][reap-barrier]") {
    const char* marker = GENERATE("partial-recovery-v1", "adopted-source-v1", "report-refusal-v1");
    CAPTURE(marker);
    int expected_exit = 0;
    int expected_signal = 0;
    std::string ending = "exit 0\n";
    SECTION("normal zero exit") {}
    SECTION("normal nonzero exit is not a signal") {
        expected_exit = 134;
        ending = "exit 134\n";
    }
    SECTION("abort after a stopped self-report is not success") {
        expected_signal = SIGABRT;
        ending = "kill -ABRT $$\nexit 9\n";
    }
    SECTION("intentional crash retains its raw signal") {
        expected_signal = SIGKILL;
        ending = "kill -KILL $$\nexit 9\n";
    }
    test::TempDir directory;
    test::write_file(directory / marker, {});
    ReapFixtureEnvironment environment(directory.str());
    const auto script = directory / "exit-device.sh";
    const std::string content =
        "#!/bin/sh\nulimit -c 0\necho '{\"state\":\"stopped\"}' >&3\n" + ending;
    test::write_file(script, std::vector<uint8_t>(content.begin(), content.end()));
    REQUIRE(::chmod(script.c_str(), 0755) == 0);
    supervisor::ChildSpec spec;
    spec.id = "actual-reap-evidence";
    spec.device_bin = script;
    spec.startup_generation = 73;
    auto child = supervisor::Child::spawn(spec);
    const auto pid = child->pid();
    // Neither a child-reported stopped state nor process exit itself can
    // publish the waitpid witness before this Child actually collects it.
    auto stopped = child->status();
    stopped.state = "stopped";
    child->update_status(stopped);
    CHECK_FALSE(test::read_child_reap(directory.str(), spec.startup_generation).has_value());
    CHECK_FALSE(child->reaped_event().is_set());
    REQUIRE(reap_child(*child));
    REQUIRE(child->reaped_event().is_set());
    const auto evidence = test::read_child_reap(directory.str(), spec.startup_generation);
    REQUIRE(evidence.has_value());
    CHECK(evidence->pid == static_cast<uint64_t>(pid));
    CHECK(evidence->reaper_pid == static_cast<uint64_t>(::getpid()));
    CHECK(evidence->generation == spec.startup_generation);
    if (expected_signal != 0) {
        CHECK_FALSE(WIFEXITED(evidence->wait_status));
        REQUIRE(WIFSIGNALED(evidence->wait_status));
        CHECK(WTERMSIG(evidence->wait_status) == expected_signal);
        CHECK(child->status().exit_code == 128 + expected_signal);
    } else {
        REQUIRE(WIFEXITED(evidence->wait_status));
        CHECK(WEXITSTATUS(evidence->wait_status) == expected_exit);
        CHECK_FALSE(WIFSIGNALED(evidence->wait_status));
        CHECK(child->status().exit_code == expected_exit);
    }
    CHECK_FALSE(child->try_reap());
    child.reset();
    const auto retained = test::read_child_reap(directory.str(), spec.startup_generation);
    REQUIRE(retained.has_value());
    CHECK(retained->pid == evidence->pid);
    CHECK(retained->wait_status == evidence->wait_status);
}

TEST_CASE("supervisor: unavailable reap evidence does not change child collection",
          "[supervisor][reap-barrier]") {
    const char* marker = GENERATE("partial-recovery-v1", "adopted-source-v1", "report-refusal-v1");
    CAPTURE(marker);
    bool occupied = false;
    SECTION("missing explicit fixture marker") {}
    SECTION("exclusive generation record already exists") { occupied = true; }
    test::TempDir directory;
    ReapFixtureEnvironment environment(directory.str());
    const auto record = directory / "child-74.reap";
    const std::vector<uint8_t> sentinel{1, 3, 5, 7};
    if (occupied) {
        test::write_file(directory / marker, {});
        test::write_file(record, sentinel);
    }
    supervisor::ChildSpec spec;
    spec.id = "unavailable-reap-evidence";
    spec.device_bin = "/bin/true";
    spec.startup_generation = 74;
    auto child = supervisor::Child::spawn(spec);
    REQUIRE(reap_child(*child));
    CHECK(child->status().exit_code == 0);
    CHECK(child->reaped_event().is_set());
    if (occupied) {
        std::ifstream input(record, std::ios::binary);
        const std::vector<uint8_t> contents{std::istreambuf_iterator<char>(input),
                                            std::istreambuf_iterator<char>()};
        CHECK(contents == sentinel);
        CHECK_THROWS(test::read_child_reap(directory.str(), spec.startup_generation));
    } else {
        CHECK_FALSE(std::filesystem::exists(record));
        CHECK_FALSE(test::read_child_reap(directory.str(), spec.startup_generation).has_value());
    }
}

TEST_CASE("supervisor: control EOF stays distinct from a confirmed reap", "[supervisor][reap-barrier]") {
    test::TempDir directory;
    const std::string script = directory / "eof-device.py";
    const std::string release = directory / "release";
    const std::string content = R"(#!/usr/bin/env python3
import os, sys, time
os.close(3)
deadline = time.monotonic() + 5
while not os.path.exists(sys.argv[2]):
    if time.monotonic() >= deadline:
        sys.exit(9)
    time.sleep(0.001)
sys.exit(7)
)";
    test::write_file(script, std::vector<uint8_t>(content.begin(), content.end()));
    REQUIRE(::chmod(script.c_str(), 0755) == 0);
    supervisor::ChildSpec spec;
    spec.id = "eof-before-reap";
    spec.device_bin = script;
    spec.config_path = release;
    auto child = supervisor::Child::spawn(spec);
    pollfd closed{child->control_fd(), POLLIN, 0};
    REQUIRE(::poll(&closed, 1, 5000) == 1);
    char byte;
    REQUIRE(::read(child->control_fd(), &byte, 1) == 0);
    auto eof = child->status();
    eof.state = "exited";
    child->update_status(eof);
    CHECK(child->exit_event().is_set());
    CHECK_FALSE(child->reaped_event().is_set());
    CHECK_FALSE(child->reaped());
    CHECK_FALSE(child->try_reap());

    auto stale = eof;
    stale.state = "stopped";
    test::write_file(release, {});
    REQUIRE(reap_child(*child));
    CHECK(child->reaped_event().is_set());
    CHECK(child->status().exit_code == 7);
    // A buffered monitor line prepared before waitpid cannot erase its
    // terminal state or exit code. Repeated reap/signals are safe no-ops.
    child->update_status(stale);
    child->terminate();
    child->kill();
    CHECK_FALSE(child->try_reap());
    CHECK(child->status().state == "exited");
    CHECK(child->status().exit_code == 7);
}

TEST_CASE("supervisor: confirmed reap retries failed lifecycle notifications", "[supervisor][reap-barrier]") {
    supervisor::ChildSpec spec;
    spec.id = "reap-notify-fault";
    spec.device_bin = "/bin/true";
    auto child = supervisor::Child::spawn(spec);
    test::WriterEventFault fault;
    std::atomic<unsigned> completed{0};
    bool registered = false;
    bool reap_fact = false;
    bool retained_waiter = false;
    bool pending_notification = false;
    bool retried = false;
    elio::run_config runtime;
    runtime.num_threads = 1;
    runtime.blocking_threads = 1;
    const int result = elio::run([&]() -> elio::coro::task<int> {
        elio::coro::task_group observers;
        const std::array events{&child->ready_event(), &child->exit_event(),
                                &child->reaped_event()};
        for (auto* event : events) {
            observers.spawn([event, &completed]() -> elio::coro::task<void> {
                co_await event->wait();
                completed.fetch_add(1);
            });
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            registered = std::all_of(events.begin(), events.end(), [](auto* event) {
                return event->waiter_count_for_test() == 1;
            });
            if (registered) break;
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        // The existing fault seam targets plain threads; collecting here
        // deterministically fails the first (reaped) event's dispatch reserve.
        co_await elio::spawn_blocking([&] {
            while (!child->reaped() && std::chrono::steady_clock::now() < deadline) {
                child->try_reap();
                if (!child->reaped()) std::this_thread::yield();
            }
            reap_fact = child->reaped();
            pending_notification = !child->reaped_event().is_set();
            retained_waiter = child->reaped_event().waiter_count_for_test() == 1;
            retried = child->try_notify_waiters();
            if (!reap_fact || !retried) {
                // Fixture recovery only: no pending observer may survive a
                // failed assertion or an unexpected process-setup failure.
                for (auto* event : events) event->release_waiters_for_test();
            }
        });
        co_await observers.join();
        co_return 0;
    }, runtime);
    CHECK(result == 0);
    CHECK(registered);
    CHECK(reap_fact);
    CHECK(pending_notification);
    CHECK(retained_waiter);
    CHECK(retried);
    CHECK(fault.failures() == 1);
    CHECK(completed.load() == 3);
    CHECK(child->status().exit_code == 0);
}

TEST_CASE("supervisor: create blank spec parses and validates size and mkfs",
          "[supervisor]") {
    // ADR-0014 modes 2/3: create accepts the additive "blank" object in
    // place of "config"; field TYPES are parse errors, value-level rules
    // (positive, 512-aligned, size bound, mkfs charset) come from
    // parse_blank_spec.
    std::string err;
    const auto blank_ok =
        supervisor::parse_command(R"({"cmd":"create","id":"a",)"
                                  R"("blank":{"size":4096}})",
                                  err);
    REQUIRE(blank_ok.has_value());
    const auto blank_mkfs = supervisor::parse_command(
        R"({"cmd":"create","id":"a","blank":{"size":4096,"mkfs":"ext4"}})",
        err);
    REQUIRE(blank_mkfs.has_value());
    const auto mkfs_val =
        supervisor::parse_blank_spec((*blank_mkfs)["blank"], err);
    REQUIRE(mkfs_val.has_value());
    REQUIRE(mkfs_val->size == 4096);
    REQUIRE(mkfs_val->mkfs == "ext4");
    // mode 2 (no mkfs) → empty mkfs type
    const auto plain =
        supervisor::parse_blank_spec((*blank_ok)["blank"], err);
    REQUIRE(plain.has_value());
    REQUIRE(plain->size == 4096);
    REQUIRE(plain->mkfs.empty());

    // config and blank are mutually exclusive; one is mandatory.
    REQUIRE(!supervisor::parse_command(R"({"cmd":"create","id":"a",)"
                                       R"("config":"/c.json",)"
                                       R"("blank":{"size":4096}})",
                                       err)
                 .has_value());
    REQUIRE(err.find("exactly one of 'config'") != std::string::npos);
    REQUIRE(!supervisor::parse_command(R"({"cmd":"create","id":"a"})", err)
                 .has_value());

    // Field TYPE errors at parse time.
    REQUIRE(!supervisor::parse_command(R"({"cmd":"create","id":"a",)"
                                       R"("blank":5})",
                                       err)
                 .has_value());
    REQUIRE(err.find("'blank' must be an object") != std::string::npos);
    REQUIRE(!supervisor::parse_command(
                R"({"cmd":"create","id":"a","blank":{"size":"big"}})", err)
                 .has_value());
    REQUIRE(!supervisor::parse_command(
                R"({"cmd":"create","id":"a","blank":{"size":4096,)"
                R"("mkfs":7}})",
                err)
                 .has_value());
    REQUIRE(err.find("'mkfs' must be a string") != std::string::npos);

    // Value-level validation (parse_blank_spec).
    const auto bad_size = supervisor::parse_blank_spec(
        nlohmann::json{{"size", 0}}, err);
    REQUIRE(!bad_size.has_value());
    REQUIRE(err.find("positive") != std::string::npos);
    const auto unaligned =
        supervisor::parse_blank_spec(nlohmann::json{{"size", 100}}, err);
    REQUIRE(!unaligned.has_value());
    REQUIRE(err.find("multiple of 512") != std::string::npos);
    const auto oversized = supervisor::parse_blank_spec(
        nlohmann::json{{"size", supervisor::kMaxBlankSizeBytes + 512}},
        err);
    REQUIRE(!oversized.has_value());
    REQUIRE(err.find("exceeds") != std::string::npos);
    const auto bad_type = supervisor::parse_blank_spec(
        nlohmann::json{{"size", 4096}, {"mkfs", "EXT4"}}, err);
    REQUIRE(!bad_type.has_value());
    REQUIRE(err.find("invalid blank 'mkfs'") != std::string::npos);
    const auto bad_type2 = supervisor::parse_blank_spec(
        nlohmann::json{{"size", 4096}, {"mkfs", "ext4/../sh"}}, err);
    REQUIRE(!bad_type2.has_value());
    REQUIRE(supervisor::valid_mkfs_type("ext4"));
    REQUIRE(supervisor::valid_mkfs_type("xfs"));
    REQUIRE(!supervisor::valid_mkfs_type(""));
    REQUIRE(!supervisor::valid_mkfs_type("Ext4"));
    REQUIRE(!supervisor::valid_mkfs_type("e:t4"));
    REQUIRE(!supervisor::valid_mkfs_type(std::string(17, 'a')));
}

namespace {

/// Creates an executable shell shim under `dir` named `name`.
void make_shim(const std::string& dir, const std::string& name,
               const std::string& body) {
    const std::string path = dir + "/" + name;
    const std::string content = "#!/bin/sh\n" + body;
    test::write_file(path,
                     std::vector<uint8_t>(content.begin(), content.end()));
    REQUIRE(::chmod(path.c_str(), 0755) == 0);
}

/// Saves/restores PATH (the default mkfs runner resolves `mkfs.<type>`
/// through the process PATH via execlp).
struct PathGuard {
    std::string prev;
    PathGuard() {
        const char* p = ::getenv("PATH");
        prev = p != nullptr ? p : "";
    }
    ~PathGuard() { ::setenv("PATH", prev.c_str(), 1); }
    PathGuard(const PathGuard&) = delete;
    PathGuard& operator=(const PathGuard&) = delete;
};

}  // namespace

// NOTE: no comma in the name — Catch2 test specs split on commas, so a
// comma would make the test unreachable by exact-name filters (and via
// catch_discover_tests' ctest entries).
TEST_CASE("supervisor: mkfs runner maps exit codes and bounds the timeout",
          "[supervisor]") {
    // The REAL default runner (ADR-0014 mode 3), exercised against
    // throwaway PATH shims: success, a nonzero exit, the 127 "not found"
    // mapping, and a bounded timeout. No host mkfs is involved — the
    // shims are test scripts.
    test::TempDir dir;
    const std::string bin = dir / "bin";
    REQUIRE(::mkdir(bin.c_str(), 0755) == 0);
    const std::string argv_log = dir / "mkfs-argv.log";
    make_shim(bin, "mkfs.ext4",
              "echo \"$@\" > \"" + argv_log + "\"\nexit 0\n");
    make_shim(bin, "mkfs.xfs", "exit 1\n");
    make_shim(bin, "mkfs.btrfs", "sleep 5\nexit 0\n");
    PathGuard path_guard;
    REQUIRE(::setenv("PATH", (bin + ":/usr/bin:/bin").c_str(), 1) == 0);

    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto runner = supervisor::make_default_mkfs_runner(5);
        std::string err;
        int r = co_await runner->run("ext4", "/dev/ublkb70", &err);
        REQUIRE(r == 0);
        REQUIRE(err.empty());

        err.clear();
        r = co_await runner->run("xfs", "/dev/ublkb70", &err);
        REQUIRE(r == 1);
        REQUIRE(err.find("exited with code 1") != std::string::npos);

        // An absent mkfs.<type> execs nothing: the child's 127 exit is
        // mapped to a "not found" message (never ECHILD / a hang).
        err.clear();
        r = co_await runner->run("nosuchfs", "/dev/ublkb70", &err);
        REQUIRE(r == 127);
        REQUIRE(err.find("not found or not executable") != std::string::npos);

        // A type outside the safe charset never reaches argv.
        err.clear();
        r = co_await runner->run("ext4/../sh", "/dev/ublkb70", &err);
        REQUIRE(r == -EINVAL);
        REQUIRE(err.find("invalid fs type") != std::string::npos);

        // Bounded: a shim sleeping past the timeout is SIGKILLed and
        // reported as -ETIMEDOUT (the runner owns its child; the daemon's
        // reaper must never have reaped it).
        auto slow = supervisor::make_default_mkfs_runner(1);
        err.clear();
        r = co_await slow->run("btrfs", "/dev/ublkb70", &err);
        REQUIRE(r == -ETIMEDOUT);
        REQUIRE(err.find("timed out") != std::string::npos);
        co_return 0;
    });
    REQUIRE(rc == 0);

    // The device argument reached the child's argv (recorded by the shim).
    const int fd = ::open(argv_log.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    std::string logged;
    char buf[256];
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    REQUIRE(n > 0);
    logged.assign(buf, static_cast<size_t>(n));
    ::close(fd);
    REQUIRE(logged.find("/dev/ublkb70") != std::string::npos);
}

TEST_CASE("supervisor: blank spawn argv carries the blank flags and global",
          "[supervisor]") {
    // ADR-0014 spawn contract: a blank child gets
    // `--blank-size N --blank-dir D [--global G] --control-fd 3` and NO
    // `--config` — and `--global` is passed for blanks too (the device
    // still reads ublkConfig and the other daemon-wide knobs from it).
    test::TempDir dir;
    const std::string log = dir / "argv.log";
    const std::string script = dir / "fake-device.sh";
    {
        const std::string content =
            "#!/bin/sh\necho \"$@\" > \"" + log + "\"\nexit 0\n";
        test::write_file(script,
                         std::vector<uint8_t>(content.begin(),
                                              content.end()));
        ::chmod(script.c_str(), 0755);
    }
    const std::string ws = dir / "ws";
    supervisor::ChildSpec spec;
    spec.id = "blank-argv";
    spec.device_bin = script;
    spec.blank = true;
    spec.blank_size = 4096;
    spec.blank_dir = ws;
    spec.global_path = "/etc/g.json";
    auto child = supervisor::Child::spawn(spec);
    REQUIRE(child->pid() > 0);
    REQUIRE(reap_child(*child));
    ::close(child->release_control_fd());

    struct stat st {};
    REQUIRE(::stat(log.c_str(), &st) == 0);
    std::string argv(static_cast<size_t>(st.st_size), '\0');
    const int fd = ::open(log.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    REQUIRE(::read(fd, argv.data(), argv.size()) ==
            static_cast<ssize_t>(argv.size()));
    ::close(fd);
    REQUIRE(argv.find("--blank-size 4096") != std::string::npos);
    REQUIRE(argv.find("--blank-dir " + ws) != std::string::npos);
    REQUIRE(argv.find("--global /etc/g.json") != std::string::npos);
    REQUIRE(argv.find("--control-fd 3") != std::string::npos);
    REQUIRE(argv.find("--config") == std::string::npos);
}


#ifdef OBD_TEST_DEVICE_BIN
TEST_CASE("supervisor: obd-device rejects malformed blank flags",
          "[supervisor]") {
    // F9 regression: obd-device is a standalone entry point too, so its
    // --blank-size validation must be as strict as the supervisor's
    // parse_blank_spec (a negative value must not become 1.8e19 and slip
    // past the alignment check). Every malformed form is a usage error
    // (exit 2) BEFORE any device/ublk work.
    auto run_device = [](const std::vector<std::string>& args) {
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(OBD_TEST_DEVICE_BIN));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        const pid_t pid = ::fork();
        REQUIRE(pid >= 0);
        if (pid == 0) {
            ::execv(OBD_TEST_DEVICE_BIN, argv.data());
            _exit(127);
        }
        int wstatus = 0;
        REQUIRE(::waitpid(pid, &wstatus, 0) == pid);
        REQUIRE(WIFEXITED(wstatus));
        return WEXITSTATUS(wstatus);
    };
    const std::string ws = "/tmp/obd-never-used";
    REQUIRE(run_device({"--blank-size", "-512", "--blank-dir", ws}) == 2);
    REQUIRE(run_device({"--blank-size", "0", "--blank-dir", ws}) == 2);
    REQUIRE(run_device({"--blank-size", "100", "--blank-dir", ws}) == 2);
    REQUIRE(run_device({"--blank-size", "junk", "--blank-dir", ws}) == 2);
    REQUIRE(run_device({"--blank-size", "18446744073709551616", "--blank-dir",
                        ws}) == 2);
    // Above the 16 TiB sanity bound the supervisor enforces.
    REQUIRE(run_device({"--blank-size", "18014398509481984", "--blank-dir",
                        ws}) == 2);
    // Blank mode needs its workspace, and the modes are exclusive.
    REQUIRE(run_device({"--blank-size", "4096"}) == 2);
    REQUIRE(run_device({"--config", "/x.json", "--blank-size", "4096",
                        "--blank-dir", ws}) == 2);
}
#endif

#ifdef OBD_TEST_FAKE_DEVICE_BIN
TEST_CASE("supervisor: fake device rejects malformed blank flags like obd-device",
          "[supervisor]") {
    // Review finding: the fake device parsed --blank-size with std::stoull,
    // so "-512" wrapped to 1.8e19 and sailed past its zero/alignment check
    // — a test-only binary accepting what the production binary refuses.
    // The flags now validate identically, so a blank-mode integration test
    // can never be driven by a size production would have rejected.
    auto run_fake = [](const std::vector<std::string>& args) {
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(OBD_TEST_FAKE_DEVICE_BIN));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        const pid_t pid = ::fork();
        REQUIRE(pid >= 0);
        if (pid == 0) {
            ::execv(OBD_TEST_FAKE_DEVICE_BIN, argv.data());
            _exit(127);
        }
        // Bounded wait (the repo has no CTest timeouts yet, #11): if the
        // validator ever regresses, the fake gets past argument parsing and
        // serves until SIGTERM — kill it and return -1, so the regression
        // fails the assertion instead of hanging the job.
        int wstatus = 0;
        for (int i = 0; i < 50; ++i) {
            const pid_t r = ::waitpid(pid, &wstatus, WNOHANG);
            if (r == pid) {
                return WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
            }
            if (r < 0 && errno != EINTR) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &wstatus, 0);
        return -1;
    };
    // A workspace on a read-only filesystem: every case that (wrongly)
    // gets past validation still dies during assembly, so a regression is
    // an exit-code mismatch rather than a 5 s timeout.
    const std::string ws = "/proc/obd-never-used";
    REQUIRE(run_fake({"--blank-size", "-512", "--blank-dir", ws}) == 2);
    REQUIRE(run_fake({"--blank-size", "0", "--blank-dir", ws}) == 2);
    REQUIRE(run_fake({"--blank-size", "100", "--blank-dir", ws}) == 2);
    REQUIRE(run_fake({"--blank-size", "junk", "--blank-dir", ws}) == 2);
    REQUIRE(run_fake({"--blank-size", "18446744073709551616", "--blank-dir",
                      ws}) == 2);
    REQUIRE(run_fake({"--blank-size", "18014398509481984", "--blank-dir",
                      ws}) == 2);
    // Blank mode needs its workspace, and the modes are exclusive.
    REQUIRE(run_fake({"--blank-size", "4096"}) == 2);
    REQUIRE(run_fake({"--config", "/x.json", "--blank-size", "4096",
                      "--blank-dir", ws}) == 2);
    // A well-formed size is NOT a usage error: the read-only workspace
    // makes the blank assembly fail, so the fake reports a failed device
    // (exit 1) — but only after validation accepted the size.
    REQUIRE(run_fake({"--blank-size", "4096", "--blank-dir", ws}) == 1);
}
#endif

TEST_CASE("supervisor: blank upper type is explicit and validated", "[supervisor][hybrid]") {
    std::string error;
    for (const auto type : {"lsmt", "hybrid"}) {
        const auto spec = supervisor::parse_blank_spec(
            nlohmann::json{{"size", 32768}, {"upper_type", type}}, error);
        REQUIRE(spec);
        REQUIRE(spec->upper_type == type);
    }
    const auto implicit = supervisor::parse_blank_spec(nlohmann::json{{"size", 32768}}, error);
    REQUIRE(implicit);
    REQUIRE(implicit->upper_type == "lsmt");
    for (const auto& invalid : {nlohmann::json(42), nlohmann::json("sparse"), nlohmann::json("")}) {
        REQUIRE_FALSE(supervisor::parse_blank_spec(
            nlohmann::json{{"size", 32768}, {"upper_type", invalid}}, error));
        REQUIRE(error.find("upper_type") != std::string::npos);
    }
}
