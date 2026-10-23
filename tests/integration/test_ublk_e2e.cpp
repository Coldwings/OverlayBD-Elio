// Integration test: ublk end-to-end. Requires /dev/ublk-control and
// CAP_SYS_ADMIN; self-skips otherwise (CI/dev sandboxes usually lack both).
// The privileged CI job loads ublk_drv and runs exactly these ([ublk]).
#include "common/errors.hpp"
#include "ublk/device.hpp"

#include "../support.hpp"
#include "../ublk_prerequisites.hpp"

#include <elio/runtime/spawn_blocking.hpp>
#include <elio/time/timer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <system_error>
#include <thread>

#include <fcntl.h>

using namespace obd;

namespace {

/// Run a blocking bdev syscall off-scheduler: a synchronous read/write
/// on a worker stalls it, and the ublk request it issues is serviced by
/// bridge coroutines that may need that very worker (deadlock).
template <typename F>
auto bdev_io(F&& f) {
    return elio::spawn_blocking(std::forward<F>(f));
}

// Retain the Device across assertions, early returns (including unsupported
// resize), and exceptions. Closing a dirty bdev must precede the IO drain.
template <typename F>
elio::coro::task<int> with_device_cleanup(
    std::unique_ptr<ublk::Device>& dev, int& fd, F body) {
    int result = 0;
    std::exception_ptr failure;
    try {
        result = co_await body();
    } catch (...) {
        failure = std::current_exception();
    }
    if (fd >= 0) {
        co_await bdev_io([&] { return ::close(fd); });
        fd = -1;
    }
    co_await dev->stop_async();
    co_await bdev_io([&] { dev.reset(); });
    if (failure) std::rethrow_exception(failure);
    co_return result;
}

void stage(const char* msg) {
    std::fprintf(stderr, "[e2e-stage] %s\n", msg);
    std::fflush(stderr);
}

// The recovery test reports live kernel state, not the requested ADD_DEV
// flags. Both processes share this small, fork-local pipe message.
using RecoveryReport = std::array<uint64_t, 2>;  // dev_id, actual flags
constexpr uint64_t kRecoveryFlags =
    UBLK_F_USER_RECOVERY | UBLK_F_USER_RECOVERY_REISSUE;

bool write_recovery_report(int fd, const RecoveryReport& report) {
    const auto* p = reinterpret_cast<const uint8_t*>(report.data());
    size_t left = sizeof(report);
    while (left != 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        left -= static_cast<size_t>(n);
    }
    return true;
}

bool read_recovery_report(int fd, RecoveryReport& report) {
    auto* p = reinterpret_cast<uint8_t*>(report.data());
    size_t left = sizeof(report);
    while (left != 0) {
        const ssize_t n = ::read(fd, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        left -= static_cast<size_t>(n);
    }
    return true;
}

// Own every fixture resource across assertions and failed attach. The
// kernel device is released here only until a replacement adopts it.
struct RecoveryServer {
    RecoveryServer() = default;
    RecoveryServer(const RecoveryServer&) = delete;
    RecoveryServer& operator=(const RecoveryServer&) = delete;

    int pipefd[2] = {-1, -1};
    pid_t child = -1;
    uint32_t dev_id = UINT32_MAX;
    int client_fd = -1;

    bool reap(int& status) {
        if (child <= 0) return false;
        pid_t result;
        do {
            result = ::waitpid(child, &status, 0);
        } while (result < 0 && errno == EINTR);
        if (result != child) return false;
        child = -1;
        return true;
    }

    ~RecoveryServer() {
        for (int fd : pipefd) if (fd >= 0) ::close(fd);
        if (client_fd >= 0) ::close(client_fd);
        if (child > 0) {
            int status = 0;
            pid_t result;
            do {
                result = ::waitpid(child, &status, WNOHANG);
            } while (result < 0 && errno == EINTR);
            if (result == 0) {
                ::kill(child, SIGKILL);
                (void)reap(status);
            }
        }
        if (dev_id != UINT32_MAX) {
            try {
                ublk::Ctrl cleanup;
                cleanup.adopt_dev(dev_id);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "recovery fixture cleanup: %s\n", e.what());
            }
        }
    }
};

bool kernel_device_absent(uint32_t dev_id) {
    ublk::Ctrl probe;
    try {
        (void)probe.get_dev_info(dev_id);
    } catch (const std::system_error& e) {
        if (e.code() == std::errc::no_such_device) return true;
        throw;
    }
    return false;
}

/// Kernel ENOTSUPP: the pre-6.15 ublk control-dispatch default for an
/// unknown command (524). Not exposed by glibc's <errno.h>.
constexpr int kKernelEnNotSupp = 524;

/// In-memory writable root: pread/pwrite over a buffer, discard zeroes
/// the range (mask semantics at the root). Lets the E2E exercise the
/// kernel->bridge write/discard plumbing without an image on disk.
class MemWritable final : public source::WritableBlobSource {
public:
    explicit MemWritable(std::vector<uint8_t> data)
        : data_(std::move(data)) {}

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override {
        if (offset >= data_.size()) co_return 0;
        const size_t n =
            std::min(count, static_cast<size_t>(data_.size() - offset));
        std::memcpy(buf, data_.data() + offset, n);
        co_return static_cast<ssize_t>(n);
    }
    elio::coro::task<ssize_t> pwrite(const void* buf, size_t count,
                                     uint64_t offset) override {
        if (offset + count > data_.size()) co_return -EINVAL;
        std::memcpy(data_.data() + offset, buf, count);
        co_return static_cast<ssize_t>(count);
    }
    elio::coro::task<int> flush() override { co_return 0; }
    elio::coro::task<int> discard(uint64_t offset, uint64_t len) override {
        if (offset % 512 != 0 || len % 512 != 0 ||
            offset + len > data_.size()) {
            co_return -EINVAL;
        }
        std::memset(data_.data() + offset, 0, len);
        discards_.fetch_add(1, std::memory_order_relaxed);
        co_return 0;
    }
    uint64_t size() const noexcept override { return data_.size(); }
    std::string_view label() const noexcept override { return "mem-rw"; }

    uint64_t discards() const { return discards_.load(); }

    /// D3 data-plane grow (the test plays the merged-view grow the real
    /// device performs before the kernel resize): extends the buffer to
    /// `bytes`, zero-filling the added headroom.
    void grow(uint64_t bytes) {
        if (bytes <= data_.size()) return;
        data_.resize(static_cast<size_t>(bytes), 0);
    }

private:
    std::vector<uint8_t> data_;
    std::atomic<uint64_t> discards_{0};
};

}  // namespace

TEST_CASE("integration: ublk device serves sector reads from a blob",
          "[ublk]") {
    if (const auto reason = test::ublk_prerequisite_skip_reason();
        !reason.empty()) {
        SKIP(reason);
    }
    const uint16_t nr_queues = GENERATE(1, 2);
    INFO("nr_queues=" << nr_queues);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto data = test::pattern_bytes(512 * 64, 81);
        source::BlobSourcePtr src =
            std::make_unique<test::VectorSource>(data);
        ublk::DeviceParams params;
        params.dev_sectors = data.size() / 512;
        params.nr_queues = nr_queues;
        stage("reads: create");
        auto dev = co_await ublk::Device::create(params, std::move(src));
        int fd = -1;
        co_return co_await with_device_cleanup(dev, fd, [&]()
            -> elio::coro::task<int> {
            stage("reads: created, open bdev");
            fd = co_await bdev_io([&] {
                return ::open(dev->bdev_path().c_str(), O_RDONLY);
            });
            REQUIRE(fd >= 0);
            std::vector<uint8_t> buf(4096);
            stage("reads: pread");
            const ssize_t nread = co_await bdev_io([&] {
                return ::pread(fd, buf.data(), buf.size(), 1024);
            });
            REQUIRE(nread == 4096);
            stage("reads: pread done");
            REQUIRE(buf == std::vector<uint8_t>(data.begin() + 1024,
                                                data.begin() + 1024 + 4096));
            // close() also belongs off-scheduler: closing a dirty bdev fd
            // issues writeback IO that our bridges must service.
            co_return 0;
        });
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: ublk writable device serves writes and discard",
          "[ublk]") {
    if (const auto reason = test::ublk_prerequisite_skip_reason();
        !reason.empty()) {
        SKIP(reason);
    }
    const uint16_t nr_queues = GENERATE(1, 2);
    INFO("nr_queues=" << nr_queues);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto data = test::pattern_bytes(512 * 64, 82);
        auto src = std::make_unique<MemWritable>(data);
        MemWritable* raw = src.get();
        ublk::DeviceParams params;
        params.nr_queues = nr_queues;
        params.dev_sectors = data.size() / 512;
        params.read_only = false;  // advertises discard (ADR-0009)
        stage("writes: create");
        auto dev = co_await ublk::Device::create(
            params, source::BlobSourcePtr(std::move(src)));
        int fd = -1;
        co_return co_await with_device_cleanup(dev, fd, [&]()
            -> elio::coro::task<int> {
            stage("writes: created");
            fd = co_await bdev_io([&] {
                return ::open(dev->bdev_path().c_str(), O_RDWR);
            });
            REQUIRE(fd >= 0);
            stage("writes: opened");

            // Write through the block device, read back through it.
            const auto patch = test::pattern_bytes(4096, 83);
            stage("writes: pwrite");
            const ssize_t nwritten = co_await bdev_io([&] {
                return ::pwrite(fd, patch.data(), patch.size(), 1024);
            });
            REQUIRE(nwritten == 4096);
            stage("writes: pwrite done");
            std::vector<uint8_t> buf(4096);
            const ssize_t nread = co_await bdev_io([&] {
                return ::pread(fd, buf.data(), buf.size(), 1024);
            });
            REQUIRE(nread == 4096);
            REQUIRE(buf == patch);

            // BLKDISCARD the range: the bridge maps it to root->discard, and
            // the range reads back as zeroes (ADR-0009).
            stage("writes: blkdiscard");
            uint64_t range[2] = {1024, 4096};
            const int discard_rc = co_await bdev_io([&] {
                return ::ioctl(fd, BLKDISCARD, &range);
            });
            REQUIRE(discard_rc == 0);
            REQUIRE(raw->discards() >= 1);
            const ssize_t nread2 = co_await bdev_io([&] {
                return ::pread(fd, buf.data(), buf.size(), 1024);
            });
            REQUIRE(nread2 == 4096);
            REQUIRE(buf == std::vector<uint8_t>(4096, 0));

            // Untouched prefix (before the discarded range) still reads
            // correctly — note [1024, 5120) is now zeroed by the discard,
            // so only [0, 1024) may be compared against the original.
            const ssize_t prefix_rd = co_await bdev_io([&] {
                return ::pread(fd, buf.data(), 1024, 0);
            });
            REQUIRE(prefix_rd == 1024);
            REQUIRE(std::vector<uint8_t>(buf.begin(), buf.begin() + 1024) ==
                    std::vector<uint8_t>(data.begin(), data.begin() + 1024));
            // close() flushes the dirty bdev page cache (writeback IO that
            // our bridges must service) — keep it off the scheduler.
            co_return 0;
        });
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: ublk device grows online and serves the new capacity",
          "[ublk]") {
    // D3 grow-only online resize: the DATA PLANE is grown first (here the
    // in-memory writable root plays the merged-view grow the real device
    // performs), then Device::resize_blocking issues
    // UBLK_U_CMD_UPDATE_SIZE and the kernel gendisk grows — after which a
    // WRITE into the grown region lands and reads back (not just
    // BLKGETSIZE64). Self-skipping twice: missing access/capability, or
    // a kernel whose driver lacks the UPDATE_SIZE command (it landed in
    // the 6.16 development cycle) — both degrade to SKIP, never a
    // failure.
    if (const auto reason = test::ublk_prerequisite_skip_reason();
        !reason.empty()) {
        SKIP(reason);
    }
    int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto data = test::pattern_bytes(512 * 64, 85);
        auto src = std::make_unique<MemWritable>(data);
        MemWritable* raw = src.get();
        ublk::DeviceParams params;
        params.dev_sectors = data.size() / 512;
        params.read_only = false;  // writable: discard + headroom writes
        params.enable_recovery = false;
        stage("grow: create");
        auto dev = co_await ublk::Device::create(
            params, source::BlobSourcePtr(std::move(src)));
        int fd = -1;
        co_return co_await with_device_cleanup(dev, fd, [&]()
            -> elio::coro::task<int> {
            REQUIRE(dev->size_bytes() == data.size());
            fd = co_await bdev_io([&] {
                return ::open(dev->bdev_path().c_str(), O_RDWR);
            });
            REQUIRE(fd >= 0);

            // Grow-only is enforced device-side BEFORE any kernel IO: a
            // shrink (or no-op) attempt throws EINVAL even on kernels without
            // UPDATE_SIZE support.
            bool shrink_rejected = false;
            try {
                co_await elio::spawn_blocking([&] {
                    return dev->resize_blocking(data.size() / 2);
                });
            } catch (const std::exception&) {
                shrink_rejected = true;
            }
            if (!shrink_rejected) co_return 2;

            // Data-plane grow FIRST (the real device grows the writable
            // merged view before touching the kernel), then the kernel grow.
            const uint64_t grown = data.size() * 2;
            raw->grow(grown);
            uint64_t new_size = 0;
            try {
                new_size = co_await elio::spawn_blocking([&] {
                    return dev->resize_blocking(grown);
                });
            } catch (const std::system_error& e) {
                // ONLY a missing command means "kernel too old" (kernel
                // ENOTSUPP/524 on pre-6.15 control dispatch, EOPNOTSUPP/95
                // on 6.15+). Any other failure — a malformed request, an
                // ABI/driver regression, an I/O error — must FAIL the test,
                // not silently skip it.
                const int ev = e.code().value();
                if (ev == kKernelEnNotSupp || ev == EOPNOTSUPP) co_return 3;
                co_return 7;
            }
            if (new_size != grown) co_return 4;
            if (dev->size_bytes() != new_size) co_return 4;

            // The kernel gendisk reports the new capacity.
            unsigned long long cap = 0;
            const int ir = co_await bdev_io([&] {
                return ::ioctl(fd, BLKGETSIZE64, &cap);
            });
            if (ir != 0 || cap != new_size) co_return 5;

            // WRITE into the grown region (beyond the original size) and read
            // it back through the block device: before the grow both the data
            // plane and the kernel rejected these offsets.
            const auto patch = test::pattern_bytes(4096, 86);
            stage("grow: write into grown region");
            const auto grown_written = co_await bdev_io([&] {
                return ::pwrite(fd, patch.data(), patch.size(),
                                data.size() + 1024);
            });
            REQUIRE(grown_written == 4096);
            std::vector<uint8_t> buf(4096);
            const auto grown_read = co_await bdev_io([&] {
                return ::pread(fd, buf.data(), buf.size(), data.size() + 1024);
            });
            REQUIRE(grown_read == 4096);
            REQUIRE(buf == patch);

            // The grown device still serves the original content.
            const auto original_read = co_await bdev_io([&] {
                return ::pread(fd, buf.data(), buf.size(), data.size() / 2);
            });
            REQUIRE(original_read == 4096);
            REQUIRE(buf ==
                    std::vector<uint8_t>(data.begin() + static_cast<long>(
                                                              data.size() / 2),
                                         data.begin() +
                                             static_cast<long>(data.size() / 2) +
                                             4096));

            // A shrink after the grow is rejected too (no-op <= current).
            bool post_shrink_rejected = false;
            try {
                co_await elio::spawn_blocking([&] {
                    return dev->resize_blocking(data.size());
                });
            } catch (const std::exception&) {
                post_shrink_rejected = true;
            }
            if (!post_shrink_rejected) co_return 6;

            co_return 0;
        });
    });
    if (rc == 3) {
        SKIP("kernel driver lacks UBLK_U_CMD_UPDATE_SIZE (needs the 6.16 "
             "cycle update: ENOTSUPP/524 on pre-6.15, EOPNOTSUPP/95 on "
             "6.15+)");
    }
    // rc == 7 is an UNEXPECTED resize failure (anything but "command not
    // supported"): fail loudly instead of skipping an ABI regression.
    REQUIRE(rc == 0);
}

namespace {

void check_recovery(bool request_recovery, uint16_t nr_queues) {
    // A child process creates and serves the device; the parent kills it
    // and attaches a replacement server (the real kernel handshake,
    // ADR-0010). The block device must survive and keep serving reads.
    RecoveryServer server;
    REQUIRE(::pipe(server.pipefd) == 0);
    stage("recovery: fork");
    server.child = ::fork();
    REQUIRE(server.child >= 0);
    if (server.child == 0) {
        // Child: serve until killed. No cleanup on SIGKILL — the kernel
        // keeps a recoverable device QUIESCED. Without negotiated recovery,
        // drain and delete normally BEFORE exiting instead.
        ::close(server.pipefd[0]);
        int rc = 1;
        try {
            rc = elio::run([](int out, bool requested, uint16_t queue_count)
                -> elio::coro::task<int> {
                auto data = test::pattern_bytes(512 * 64, 84);
                source::BlobSourcePtr src =
                    std::make_unique<test::VectorSource>(data);
                ublk::DeviceParams params;
                params.dev_sectors = data.size() / 512;
                params.enable_recovery = requested;
                params.nr_queues = queue_count;
                auto dev = co_await ublk::Device::create(params, std::move(src));
                int unused_fd = -1;
                co_return co_await with_device_cleanup(dev, unused_fd, [&]()
                    -> elio::coro::task<int> {
                    const auto info = co_await bdev_io([&] {
                        ublk::Ctrl probe;
                        return probe.get_dev_info(dev->dev_id());
                    });
                    verify(info.dev_id == dev->dev_id() &&
                               info.nr_hw_queues == params.nr_queues &&
                               info.queue_depth == params.queue_depth,
                           "GET_DEV_INFO returned unexpected device geometry");
                    const RecoveryReport report{dev->dev_id(), info.flags};
                    const bool sent = co_await bdev_io([&] {
                        return write_recovery_report(out, report);
                    });
                    if (!sent) co_return 2;
                    if (!requested ||
                        (info.flags & kRecoveryFlags) != kRecoveryFlags) {
                        co_return 0;
                    }
                    for (;;) {
                        co_await elio::time::sleep_for(std::chrono::hours(1));
                    }
                });
            }, server.pipefd[1], request_recovery, nr_queues);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "recovery child failed: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "recovery child failed\n");
        }
        ::close(server.pipefd[1]);
        _exit(rc);
    }
    ::close(server.pipefd[1]);
    server.pipefd[1] = -1;
    RecoveryReport report{};
    REQUIRE(read_recovery_report(server.pipefd[0], report));
    ::close(server.pipefd[0]);
    server.pipefd[0] = -1;
    REQUIRE(report[0] < UINT32_MAX);
    const uint32_t dev_id = static_cast<uint32_t>(report[0]);
    const auto bdev = ublk::Ctrl::bdev_path(dev_id);

    const bool negotiated = (report[1] & kRecoveryFlags) == kRecoveryFlags;
    if (!request_recovery || !negotiated) {
        int status = 0;
        REQUIRE(server.reap(status));
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == 0);
        REQUIRE(kernel_device_absent(dev_id));
        if (request_recovery) {
            SKIP("kernel did not negotiate USER_RECOVERY and REISSUE");
        }
        REQUIRE((report[1] & kRecoveryFlags) == 0);
        return;
    }
    // The negotiated-recovery child remains alive until we kill it. Arm
    // the fallback cleanup only here; graceful child teardown already owns
    // DEL and may have freed its id before the parent receives its report.
    server.dev_id = dev_id;

    // The device answers reads while the first server is alive.
    const auto expected = test::pattern_bytes(512 * 64, 84);
    server.client_fd = ::open(bdev.c_str(), O_RDONLY);
    REQUIRE(server.client_fd >= 0);
    std::vector<uint8_t> buf(4096);
    REQUIRE(::pread(server.client_fd, buf.data(), buf.size(), 2048) == 4096);
    REQUIRE(buf == std::vector<uint8_t>(expected.begin() + 2048,
                                        expected.begin() + 2048 + 4096));
    ::close(server.client_fd);
    server.client_fd = -1;

    // Only negotiated recovery is crash-tested. Node existence after
    // process death cannot distinguish recovery from asynchronous stop.
    REQUIRE(::kill(server.child, SIGKILL) == 0);
    int wstatus = 0;
    REQUIRE(server.reap(wstatus));
    REQUIRE(WIFSIGNALED(wstatus));
    REQUIRE(WTERMSIG(wstatus) == SIGKILL);

    // Attach immediately: waitpid does not wait for the driver's delayed
    // quiesce work (Linux 6.6 can take about five seconds). Device::attach
    // must wait out EBUSY before opening replacement queue cdevs itself.
    stage("recovery: attach");
    ublk::StartupControl startup(1);
    // This fixture owns the old device and explicitly authorizes one release
    // attempt if replacement startup fails. Keep the mailbox reachable while
    // attach is suspended; a private controller inside attach cannot do this.
    std::jthread release_observer([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            const auto state = startup.snapshot();
            if (state.phase == ublk::StartupPhase::retained) {
                (void)startup.request_release(state.generation, 1);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    int rc = -1;
    try {
        rc = test::run_coro([&]() -> elio::coro::task<int> {
            auto data = test::pattern_bytes(512 * 64, 84);
            source::BlobSourcePtr src =
                std::make_unique<test::VectorSource>(data);
            ublk::DeviceParams params;
            params.dev_sectors = data.size() / 512;
            params.nr_queues = nr_queues;
            auto dev = co_await ublk::Device::attach(
                dev_id, params, std::move(src), startup);
            server.dev_id = UINT32_MAX;  // The replacement now owns cleanup.
            int fd2 = -1;
            co_return co_await with_device_cleanup(dev, fd2, [&]()
                -> elio::coro::task<int> {
                fd2 = co_await bdev_io([&] {
                    return ::open(dev->bdev_path().c_str(), O_RDONLY);
                });
                REQUIRE(fd2 >= 0);
                std::vector<uint8_t> buf2(4096);
                const ssize_t nread2 = co_await bdev_io([&] {
                    return ::pread(fd2, buf2.data(), buf2.size(), 2048);
                });
                REQUIRE(nread2 == 4096);
                REQUIRE(buf2 == std::vector<uint8_t>(expected.begin() + 2048,
                                                     expected.begin() + 2048 + 4096));
                co_return 0;
            });
        });
    } catch (...) {
        // END may have succeeded before a later startup check failed. The
        // replacement then owns automatic cleanup even without an explicit
        // release request. Require positive deletion/absence evidence: a
        // locally released pre-publication frame alone says nothing about
        // whether the old device still exists.
        const auto final = startup.snapshot();
        const bool absent = final.original_known &&
            ((final.operation == ublk::StartupOperation::remove &&
              (final.original_result == 0 || final.original_result == -ENODEV)) ||
             (final.operation == ublk::StartupOperation::stop &&
              final.original_result == -ENODEV));
        if (final.phase == ublk::StartupPhase::released && absent)
            server.dev_id = UINT32_MAX;
        throw;
    }
    release_observer.request_stop();
    release_observer.join();
    REQUIRE(rc == 0);
    REQUIRE(kernel_device_absent(dev_id));
}

}  // namespace

TEST_CASE("integration: ublk device survives server death via USER_RECOVERY",
          "[ublk]") {
    if (const auto reason = test::ublk_prerequisite_skip_reason();
        !reason.empty()) {
        SKIP(reason);
    }
    const uint16_t nr_queues = GENERATE(1, 2);
    INFO("nr_queues=" << nr_queues);
    SECTION("explicitly disabled recovery exits and cleans up normally") {
        check_recovery(false, nr_queues);
    }
    SECTION("recover a device that negotiated USER_RECOVERY") {
        check_recovery(true, nr_queues);
    }
}
