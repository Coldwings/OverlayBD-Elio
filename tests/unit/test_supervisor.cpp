// Unit tests: supervisor module — protocol and child lifecycle with fake
// device binaries.
#include "supervisor/child.hpp"
#include "supervisor/protocol.hpp"

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>

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
