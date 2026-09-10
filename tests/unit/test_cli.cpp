// Unit tests: the obdctl binary's CLI → wire translation. These tests
// EXECUTE the real obdctl against a test-owned Unix-domain socket and
// assert the exact JSON line it sends, so a CLI that builds a request the
// supervisor cannot dispatch (e.g. `create-blank` must send
// cmd:"create") fails here instead of at an operator's terminal.
#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>

using namespace obd;

#ifndef OBD_TEST_OBDCTL_BIN
#define OBD_TEST_OBDCTL_BIN "obdctl"
#endif

namespace {

/// One-shot UDS server: accepts exactly one connection, captures the
/// request line, and answers with the given reply line. Blocking, test
/// only — obdctl is a blocking CLI, and this harness deliberately keeps
/// everything on the test thread (no Catch2 assertions off-thread).
class OneShotServer {
public:
    explicit OneShotServer(const std::string& path) : path_(path) {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        REQUIRE(fd_ >= 0);
        sockaddr_un sa {};
        sa.sun_family = AF_UNIX;
        // sun_path is a fixed 108-byte field: a truncated path would bind a
        // DIFFERENT address than the one the CLI is told to connect to, so
        // assert the fit instead of silently testing the wrong socket.
        const int wrote = std::snprintf(sa.sun_path, sizeof(sa.sun_path), "%s",
                                        path.c_str());
        REQUIRE(wrote > 0);
        REQUIRE(static_cast<size_t>(wrote) < sizeof(sa.sun_path));
        // A leftover socket from an aborted earlier run would make bind()
        // fail with EADDRINUSE; the destructor unlinks, so make the
        // constructor equally idempotent.
        ::unlink(path.c_str());
        REQUIRE(::bind(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) ==
                0);
        REQUIRE(::listen(fd_, 1) == 0);
    }
    ~OneShotServer() {
        if (conn_ >= 0) ::close(conn_);
        if (fd_ >= 0) ::close(fd_);
        ::unlink(path_.c_str());
    }
    OneShotServer(const OneShotServer&) = delete;
    OneShotServer& operator=(const OneShotServer&) = delete;

    /// Waits for the client's single request line (returned without the
    /// trailing newline), then answers with `reply` and closes.
    std::string receive_and_reply(const std::string& reply) {
        conn_ = ::accept(fd_, nullptr, nullptr);
        REQUIRE(conn_ >= 0);
        std::string line;
        char buf[512];
        for (;;) {
            const ssize_t r = ::read(conn_, buf, sizeof(buf));
            REQUIRE(r > 0);
            line.append(buf, static_cast<size_t>(r));
            if (line.find('\n') != std::string::npos) break;
        }
        // A stream socket may accept only part of the reply; loop so a
        // short write cannot truncate the JSON and flake the test.
        size_t sent = 0;
        while (sent < reply.size()) {
            const ssize_t w = ::write(conn_, reply.data() + sent,
                                      reply.size() - sent);
            if (w <= 0) {
                if (w < 0 && errno == EINTR) continue;
                break;
            }
            sent += static_cast<size_t>(w);
        }
        REQUIRE(sent == reply.size());
        ::close(conn_);
        conn_ = -1;
        const auto nl = line.find('\n');
        return nl == std::string::npos ? line : line.substr(0, nl);
    }

private:
    std::string path_;
    int fd_ = -1;
    int conn_ = -1;
};

/// Forks and execs obdctl with `args`; returns the pid (the caller drives
/// the socket and then waits). Exit 127 means the exec itself failed.
/// `--socket` MUST precede the command word (obdctl's parser contract), so
/// it is inserted here rather than repeated in every call.
pid_t spawn_obdctl(const std::string& sock,
                   const std::vector<std::string>& args) {
    std::vector<std::string> full = {"--socket", sock};
    full.insert(full.end(), args.begin(), args.end());
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(OBD_TEST_OBDCTL_BIN));
    for (const auto& a : full) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::execv(OBD_TEST_OBDCTL_BIN, argv.data());
        _exit(127);
    }
    return pid;
}

/// Waits for the spawned obdctl and returns its exit code.
int wait_obdctl(pid_t pid) {
    int wstatus = 0;
    REQUIRE(::waitpid(pid, &wstatus, 0) == pid);
    REQUIRE(WIFEXITED(wstatus));
    return WEXITSTATUS(wstatus);
}

}  // namespace

// NOTE: name on one source line (check-docs extracts names line-wise).
TEST_CASE("cli: obdctl create-blank sends a create command with the blank object",
          "[cli]") {
    test::TempDir dir;
    const std::string sock = dir / "ctl.sock";

    // Mode 3: --size + --mkfs + --global + --dev-id all land in the wire
    // request, and the command word is `create` (the supervisor has no
    // `create-blank` command — it answers "unknown cmd").
    {
        OneShotServer server(sock);
        const pid_t pid = spawn_obdctl(
            sock, {"create-blank", "d1", "--size", "1048576", "--mkfs",
                   "ext4", "--global", "/etc/g.json", "--dev-id", "7"});
        const std::string line =
            server.receive_and_reply("{\"ok\":true,\"id\":\"d1\"}\n");
        REQUIRE(wait_obdctl(pid) == 0);
        const auto j = nlohmann::json::parse(line);
        REQUIRE(j["cmd"].get<std::string>() == "create");
        REQUIRE(j["id"].get<std::string>() == "d1");
        REQUIRE(!j.contains("config"));
        REQUIRE(j["blank"]["size"].get<uint64_t>() == 1048576);
        REQUIRE(j["blank"]["mkfs"].get<std::string>() == "ext4");
        REQUIRE(j["global"].get<std::string>() == "/etc/g.json");
        REQUIRE(j["dev_id"].get<int>() == 7);
    }

    // Mode 2: no mkfs field at all (the daemon must not run host mkfs).
    {
        OneShotServer server(sock);
        const pid_t pid =
            spawn_obdctl(sock, {"create-blank", "d2", "--size", "1048576"});
        const std::string line =
            server.receive_and_reply("{\"ok\":true,\"id\":\"d2\"}\n");
        REQUIRE(wait_obdctl(pid) == 0);
        const auto j = nlohmann::json::parse(line);
        REQUIRE(j["cmd"].get<std::string>() == "create");
        REQUIRE(j["blank"]["size"].get<uint64_t>() == 1048576);
        REQUIRE(!j["blank"].contains("mkfs"));
    }

    // A rejected request (ok:false) is exit code 1, not 0.
    {
        OneShotServer server(sock);
        const pid_t pid =
            spawn_obdctl(sock, {"create-blank", "d3", "--size", "512"});
        server.receive_and_reply("{\"ok\":false,\"error\":\"nope\"}\n");
        REQUIRE(wait_obdctl(pid) == 1);
    }

    // The image-mode create still sends `config` (no blank object).
    {
        OneShotServer server(sock);
        const pid_t pid =
            spawn_obdctl(sock, {"create", "d4", "/tmp/config.json"});
        const std::string line =
            server.receive_and_reply("{\"ok\":true,\"id\":\"d4\"}\n");
        REQUIRE(wait_obdctl(pid) == 0);
        const auto j = nlohmann::json::parse(line);
        REQUIRE(j["cmd"].get<std::string>() == "create");
        REQUIRE(j["config"].get<std::string>() == "/tmp/config.json");
        REQUIRE(!j.contains("blank"));
    }

    // Usage errors exit 2 WITHOUT connecting: the socket path below does
    // not exist, so a CLI that tried to connect anyway would fail with
    // exit 1 (connect error) instead of 2 — the assertions have teeth
    // without any hang risk.
    const std::string absent = dir / "never-created.sock";
    REQUIRE(wait_obdctl(spawn_obdctl(
                absent, {"create-blank", "d5", "--size", "-512"})) == 2);
    REQUIRE(wait_obdctl(spawn_obdctl(
                absent, {"create-blank", "d6", "--size", "100"})) == 2);
    REQUIRE(wait_obdctl(spawn_obdctl(absent, {"create-blank", "d7"})) == 2);
    REQUIRE(wait_obdctl(spawn_obdctl(
                absent, {"create-blank", "d8", "--size", "512", "--mkfs",
                         "EXT4"})) == 2);
    REQUIRE(wait_obdctl(spawn_obdctl(
                absent, {"create-blank", "d9", "--size", "512", "--bogus"})) ==
            2);
    // `--dev-id` is validated identically in BOTH create forms: `std::stoi`
    // would abort the CLI (uncaught exception, so WIFEXITED is false) on
    // junk or overflow instead of exiting 2 — the regression is caught here
    // because wait_obdctl asserts a normal exit.
    REQUIRE(wait_obdctl(spawn_obdctl(
                absent, {"create-blank", "d10", "--size", "512", "--dev-id",
                         "abc"})) == 2);
    REQUIRE(wait_obdctl(spawn_obdctl(
                absent, {"create-blank", "d11", "--size", "512", "--dev-id",
                         "99999999999"})) == 2);
    REQUIRE(wait_obdctl(spawn_obdctl(
                absent, {"create", "d12", "/tmp/config.json", "--dev-id",
                         "abc"})) == 2);
}
