// Unit tests: supervisor module — protocol and child lifecycle with fake
// device binaries.
#include "supervisor/child.hpp"
#include "supervisor/protocol.hpp"

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace obd;

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

    int wstatus = 0;
    REQUIRE(::waitpid(child->pid(), &wstatus, 0) == child->pid());
    child->note_reaped(wstatus);
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
    int wstatus = 0;
    REQUIRE(::waitpid(child->pid(), &wstatus, 0) == child->pid());
    child->note_reaped(wstatus);
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
    int wstatus = 0;
    REQUIRE(::waitpid(child->pid(), &wstatus, 0) == child->pid());
    child->note_reaped(wstatus);
    REQUIRE(child->status().exit_code == 127);
    REQUIRE(child->status().error.find("exec failed") !=
            std::string::npos);
    ::close(child->release_control_fd());
}
