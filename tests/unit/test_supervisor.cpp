// Unit tests: supervisor module — protocol and child lifecycle with fake
// device binaries.
#include "supervisor/child.hpp"
#include "supervisor/daemon.hpp"
#include "supervisor/protocol.hpp"

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <csignal>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
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
