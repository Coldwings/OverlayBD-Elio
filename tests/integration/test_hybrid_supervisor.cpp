// Real supervisor/device recovery; admitted only with kernel ublk prerequisites.
#include "startup_kernel_support.hpp"
#include "format/hybrid_rw.hpp"
#include "format/lsmt_rw.hpp"
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <cstdlib>
#include <cstring>

namespace {
using namespace obd::test::startup_kernel;

class WritableSupervisorFixture {
public:
    explicit WritableSupervisorFixture(bool lsmt = false) : lsmt_(lsmt) {
        auto pattern = (std::filesystem::temp_directory_path() / "obd-writable-supervisor-XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        const auto made = ::mkdtemp(name.data());
        if (!made) throw std::system_error(errno, std::generic_category(), "fixture directory");
        directory_ = made;
        socket_ = directory_ + "/s.sock";
        write_text(directory_ + "/global.json",
                   R"({"ublkConfig":{"enableRecovery":true,"queues":2,"workers":3}})");
    }
    ~WritableSupervisorFixture() {
        if (fd_ >= 0) ::close(fd_);
        // Preserve unresolved ownership and evidence; no forced teardown.
        if (!complete_) std::fprintf(stderr, "retained writable fixture: supervisor=%ld directory=%s\n",
                                    static_cast<long>(pid_), directory_.c_str());
    }
    void run() {
        start();
        wait_for([&] {
            try { return call({{"cmd", "hello"}}).value("ok", false); }
            catch (...) { return false; }
        }, 5s, "supervisor did not listen");
        const auto created = call({{"cmd", "create"}, {"id", "writable"},
            {"blank", {{"size", capacity}, {"upper_type", lsmt_ ? "lsmt" : "hybrid"}}}});
        require_fixture(created.value("ok", false), "writable blank create failed: " + created.dump());
        const auto original = call({{"cmd", "status"}, {"id", "writable"}});
        require_fixture(original.at("state") == "ready", "original not ready");
        const auto original_pid = original.at("pid").get<pid_t>();
        PinnedProcess child(original_pid, process_start_ticks(original_pid));
        const auto id = original.at("startup_owner").at("device_id").get<uint32_t>();
        obd::ublk::Ctrl inspect;
        const auto geometry = inspect.get_dev_info(id);
        constexpr auto flags = UBLK_F_USER_RECOVERY | UBLK_F_USER_RECOVERY_REISSUE;
        require_fixture((geometry.flags & flags) == flags && geometry.nr_hw_queues == 2,
                        "kernel did not negotiate two queues and recovery/reissue");
        const auto path = "/dev/ublkb" + std::to_string(id);
        fd_ = ::open(path.c_str(), O_RDWR | O_DIRECT | O_CLOEXEC);
        require_fixture(fd_ >= 0, "cannot open owned block device");
        void* allocation = nullptr;
        require_fixture(::posix_memalign(&allocation, 4096, capacity) == 0, "aligned buffer allocation");
        std::unique_ptr<void, decltype(&std::free)> buffer(allocation, &std::free);
        auto* bytes = static_cast<uint8_t*>(buffer.get());
        std::vector<uint8_t> expected(capacity, 0);
        std::memset(bytes, 0x61, 8192);
        require_fixture(::pwrite(fd_, bytes, 8192, 0) == 8192, "initial block write");
        std::fill_n(expected.begin(), 4096, uint8_t{0x61});
        uint64_t discard[2] = {4096, 4096};
        require_fixture(::ioctl(fd_, BLKDISCARD, discard) == 0, "block discard");
        // The LSMT variant proves completed direct IO survives process death
        // without an explicit FLUSH or graceful checkpoint.
        if (!lsmt_) require_fixture(::fsync(fd_) == 0, "durable pre-crash flush");
        child.signal(SIGKILL); // Only the pinned child created above is targeted.
        Json recovered;
        wait_for([&] {
            recovered = call({{"cmd", "status"}, {"id", "writable"}});
            return recovered.value("ok", false) && recovered.value("state", "") == "ready" &&
                   recovered.value("pid", pid_t{-1}) != original_pid &&
                   recovered.value("recoveries", 0) == 1;
        }, 40s, "supervisor did not recover its writable child");
        require_fixture(child.reaped(), "replacement became ready before original reap");
        require_fixture(recovered.at("startup_owner").at("device_id") == id,
                        "recovery replaced the kernel device");
        require_fixture(recovered.at("startup_owner").at("generation") !=
                        original.at("startup_owner").at("generation"), "generation did not advance");
        const auto replacement_pid = recovered.at("pid").get<pid_t>();
        PinnedProcess replacement(replacement_pid, process_start_ticks(replacement_pid));
        require_fixture(::pread(fd_, bytes, capacity, 0) == static_cast<ssize_t>(capacity),
                        "post-recovery direct read");
        require_fixture(std::equal(expected.begin(), expected.end(), bytes),
                        "post-recovery DATA/ZERO/INHERIT bytes differ");
        std::memset(bytes, 0x37, 4096);
        require_fixture(::pwrite(fd_, bytes, 4096, 8192) == 4096, "replacement write");
        std::fill_n(expected.begin() + 8192, 4096, uint8_t{0x37});
        require_fixture(::fsync(fd_) == 0, "replacement flush");
        require_fixture(::close(fd_) == 0, "close block device");
        fd_ = -1;
        require_fixture(call({{"cmd", "destroy"}, {"id", "writable"}}).value("ok", false),
                        "destroy recovered device");
        require_fixture(call({{"cmd", "list"}}).at("devices").empty(), "device remains registered");
        require_fixture(replacement.reaped(), "destroy returned before replacement reap");
        bool absent = false;
        try { (void)inspect.get_dev_info(id); }
        catch (const std::system_error& error) {
            if (error.code() != std::errc::no_such_device) throw;
            absent = true;
        }
        require_fixture(absent, "destroy left the kernel device registered");
        supervisor_->signal(SIGTERM);
        int status = 0;
        wait_for([&] {
            const auto result = ::waitpid(pid_, &status, WNOHANG);
            if (result < 0 && errno != EINTR) throw std::system_error(errno, std::generic_category());
            return result == pid_;
        }, 10s, "supervisor did not exit");
        require_fixture(WIFEXITED(status) && WEXITSTATUS(status) == 0, "supervisor exit failed");
        const auto workspace = directory_ + "/devices/writable/";
        const int rc = obd::test::run_coro([&]() -> elio::coro::task<int> {
            std::unique_ptr<obd::format::WritableLayer> upper;
            if (lsmt_)
                upper = co_await obd::format::LsmtRwLayer::open_or_create(
                    workspace + "overlaybd.rw", capacity, true);
            else
                upper = co_await obd::format::HybridRwLayer::open_existing(
                    workspace + "overlaybd.hybrid/data", workspace + "overlaybd.hybrid/index");
            std::vector<uint8_t> actual(capacity);
            const auto count = co_await upper->pread(actual.data(), actual.size(), 0);
            require_fixture(count == static_cast<ssize_t>(capacity) && actual == expected,
                            "final reopened writable bytes differ");
            const auto mappings = upper->segments_snapshot();
            for (uint64_t sector = 0; sector != capacity / 512; ++sector) {
                int state = 0; // INHERIT=0, DATA=1, ZERO=2.
                for (const auto& mapping : mappings) {
                    if (mapping.offset <= sector && sector < mapping.end()) {
                        require_fixture(state == 0, "overlapping recovered ownership");
                        state = mapping.zeroed ? 2 : 1;
                    }
                }
                const int wanted = sector < 8 ? 1 : sector < 16 ? 2 : sector < 24 ? 1 : 0;
                require_fixture(state == wanted, "final recovered ownership differs");
            }
            co_return 0;
        });
        require_fixture(rc == 0, "final reopen failed");
        write_text(directory_ + "/result.json", Json{{"passed", true}, {"original", original},
            {"recovered", recovered}, {"verified_bytes", capacity}}.dump(2));
        std::fprintf(stderr, "%s_SUPERVISOR verified_bytes=%zu recoveries=1 directory=%s\n",
                     lsmt_ ? "LSMT" : "HYBRID", capacity, directory_.c_str());
        complete_ = true;
    }
private:
    Json call(const Json& command) const { return rpc(socket_, command); }
    void start() {
        std::vector<std::string> args{OBD_TEST_SUPERVISOR_BIN, "--socket", socket_,
            "--global", directory_ + "/global.json", "--device-bin", OBD_TEST_DEVICE_BIN,
            "--blank-dir", directory_ + "/devices", "--ready-timeout", "40"};
        std::vector<char*> argv;
        for (auto& item : args) argv.push_back(item.data());
        argv.push_back(nullptr);
        // Do not inherit another test's startup fault-injection environment.
        std::vector<std::string> environment;
        for (char** item = environ; *item; ++item)
            if (!std::string_view(*item).starts_with("OBD_TEST_STARTUP_FIXTURE_DIR="))
                environment.emplace_back(*item);
        std::vector<char*> env;
        for (auto& item : environment) env.push_back(item.data());
        env.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        require_fixture(::posix_spawn_file_actions_init(&actions) == 0, "spawn actions");
        posix_spawnattr_t attributes;
        int error = ::posix_spawnattr_init(&attributes);
        if (error) {
            ::posix_spawn_file_actions_destroy(&actions);
            throw std::system_error(error, std::generic_category(), "spawn attributes");
        }
        sigset_t empty, defaults;
        ::sigemptyset(&empty);
        ::sigemptyset(&defaults);
        for (const int signal : {SIGTERM, SIGINT, SIGCHLD}) ::sigaddset(&defaults, signal);
        error = ::posix_spawnattr_setsigmask(&attributes, &empty);
        if (!error) error = ::posix_spawnattr_setsigdefault(&attributes, &defaults);
        if (!error) error = ::posix_spawnattr_setflags(&attributes,
            POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
        if (!error) error = ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
            (directory_ + "/supervisor.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (!error) error = ::posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        if (!error) error = ::posix_spawn(&pid_, OBD_TEST_SUPERVISOR_BIN, &actions, &attributes,
                                         argv.data(), env.data());
        ::posix_spawnattr_destroy(&attributes);
        ::posix_spawn_file_actions_destroy(&actions);
        if (error) throw std::system_error(error, std::generic_category(), "spawn supervisor");
        supervisor_ = std::make_unique<PinnedProcess>(pid_, process_start_ticks(pid_));
    }
    static constexpr size_t capacity = 1 << 20;
    std::string directory_, socket_;
    pid_t pid_ = -1;
    int fd_ = -1;
    bool complete_ = false;
    bool lsmt_ = false;
    std::unique_ptr<PinnedProcess> supervisor_;
};
} // namespace

TEST_CASE("integration: real supervisor recovers a Hybrid device and durable bytes",
          "[ublk][hybrid][supervisor]") {
    const auto skip = obd::test::ublk_prerequisite_skip_reason();
    if (!skip.empty()) SKIP(skip);
    WritableSupervisorFixture fixture;
    REQUIRE_NOTHROW(fixture.run());
}

TEST_CASE("integration: real supervisor recovers LSMT completed unflushed writes",
          "[ublk][lsmt][supervisor]") {
    const auto skip = obd::test::ublk_prerequisite_skip_reason();
    if (!skip.empty()) SKIP(skip);
    WritableSupervisorFixture fixture(true);
    REQUIRE_NOTHROW(fixture.run());
}
