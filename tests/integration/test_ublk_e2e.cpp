// Integration test: ublk end-to-end. Requires /dev/ublk-control and
// CAP_SYS_ADMIN; self-skips otherwise (CI/dev sandboxes usually lack both).
// The privileged CI job loads ublk_drv and runs exactly these ([ublk]).
#include "supervisor/protocol.hpp"
#include "ublk/device.hpp"

#include "../support.hpp"

#include <elio/runtime/spawn_blocking.hpp>
#include <elio/time/timer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

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

void stage(const char* msg) {
    std::fprintf(stderr, "[e2e-stage] %s\n", msg);
    std::fflush(stderr);
}

bool ublk_available() {
    return ::access("/dev/ublk-control", F_OK) == 0;
}

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

private:
    std::vector<uint8_t> data_;
    std::atomic<uint64_t> discards_{0};
};

}  // namespace

TEST_CASE("integration: ublk device serves sector reads from a blob",
          "[ublk]") {
    if (!ublk_available()) {
        SKIP("/dev/ublk-control unavailable (kernel ublk not enabled)");
    }
    const int rc = test::run_coro([]() -> elio::coro::task<int> {
        auto data = test::pattern_bytes(512 * 64, 81);
        source::BlobSourcePtr src =
            std::make_unique<test::VectorSource>(data);
        ublk::DeviceParams params;
        params.dev_sectors = data.size() / 512;
        stage("reads: create");
        auto dev = co_await ublk::Device::create(params, std::move(src));
        stage("reads: created, open bdev");
        const int fd = co_await bdev_io([&] {
            return ::open(dev->bdev_path().c_str(), O_RDONLY);
        });
        REQUIRE(fd >= 0);
        std::vector<uint8_t> buf(4096);
        stage("reads: pread");
        REQUIRE(co_await bdev_io([&] {
                    return ::pread(fd, buf.data(), buf.size(), 1024);
                }) == 4096);
        stage("reads: pread done");
        REQUIRE(buf == std::vector<uint8_t>(data.begin() + 1024,
                                            data.begin() + 1024 + 4096));
        // close() also belongs off-scheduler: closing a dirty bdev fd
        // issues writeback IO that our bridges must service.
        co_await bdev_io([&] { return ::close(fd); });
        dev->stop();
        dev.reset();
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: ublk writable device serves writes and discard",
          "[ublk]") {
    if (!ublk_available()) {
        SKIP("/dev/ublk-control unavailable (kernel ublk not enabled)");
    }
    const int rc = test::run_coro([]() -> elio::coro::task<int> {
        auto data = test::pattern_bytes(512 * 64, 82);
        auto src = std::make_unique<MemWritable>(data);
        MemWritable* raw = src.get();
        ublk::DeviceParams params;
        params.dev_sectors = data.size() / 512;
        params.read_only = false;  // advertises discard (ADR-0009)
        stage("writes: create");
        auto dev = co_await ublk::Device::create(
            params, source::BlobSourcePtr(std::move(src)));
        stage("writes: created");
        const int fd = co_await bdev_io([&] {
            return ::open(dev->bdev_path().c_str(), O_RDWR);
        });
        REQUIRE(fd >= 0);
        stage("writes: opened");

        // Write through the block device, read back through it.
        const auto patch = test::pattern_bytes(4096, 83);
        stage("writes: pwrite");
        REQUIRE(co_await bdev_io([&] {
                    return ::pwrite(fd, patch.data(), patch.size(), 1024);
                }) == 4096);
        stage("writes: pwrite done");
        std::vector<uint8_t> buf(4096);
        REQUIRE(co_await bdev_io([&] {
                    return ::pread(fd, buf.data(), buf.size(), 1024);
                }) == 4096);
        REQUIRE(buf == patch);

        // BLKDISCARD the range: the bridge maps it to root->discard, and
        // the range reads back as zeroes (ADR-0009).
        stage("writes: blkdiscard");
        uint64_t range[2] = {1024, 4096};
        REQUIRE(co_await bdev_io([&] {
                    return ::ioctl(fd, BLKDISCARD, &range);
                }) == 0);
        REQUIRE(raw->discards() >= 1);
        REQUIRE(co_await bdev_io([&] {
                    return ::pread(fd, buf.data(), buf.size(), 1024);
                }) == 4096);
        REQUIRE(buf == std::vector<uint8_t>(4096, 0));

        // Untouched prefix (before the discarded range) still reads
        // correctly — note [1024, 5120) is now zeroed by the discard,
        // so only [0, 1024) may be compared against the original.
        REQUIRE(co_await bdev_io([&] {
                    return ::pread(fd, buf.data(), 1024, 0);
                }) == 1024);
        REQUIRE(std::vector<uint8_t>(buf.begin(), buf.begin() + 1024) ==
                std::vector<uint8_t>(data.begin(), data.begin() + 1024));
        // close() flushes the dirty bdev page cache (writeback IO that
        // our bridges must service) — keep it off the scheduler.
        co_await bdev_io([&] { return ::close(fd); });
        dev->stop();
        dev.reset();
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("integration: ublk device grows online and serves the new capacity",
          "[ublk]") {
    // D3 grow-only online resize: Device::resize_blocking issues
    // UBLK_U_CMD_UPDATE_SIZE and the kernel gendisk grows. Self-skipping
    // twice: no /dev/ublk-control at all, or a kernel whose driver lacks
    // the UPDATE_SIZE command (it landed in the 6.16 development cycle) —
    // both degrade to SKIP, never a failure.
    if (!ublk_available()) {
        SKIP("/dev/ublk-control unavailable (kernel ublk not enabled)");
    }
    int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto data = test::pattern_bytes(512 * 64, 85);
        source::BlobSourcePtr src =
            std::make_unique<test::VectorSource>(data);
        ublk::DeviceParams params;
        params.dev_sectors = data.size() / 512;
        params.enable_recovery = false;
        stage("grow: create");
        auto dev = co_await ublk::Device::create(params, std::move(src));
        REQUIRE(dev->size_bytes() == data.size());
        const int fd = co_await bdev_io([&] {
            return ::open(dev->bdev_path().c_str(), O_RDONLY);
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

        // The real grow (kernel UPDATE_SIZE).
        uint64_t new_size = 0;
        try {
            new_size = co_await elio::spawn_blocking([&] {
                return dev->resize_blocking(data.size() * 2);
            });
        } catch (const std::exception&) {
            co_return 3;  // kernel without UBLK_U_CMD_UPDATE_SIZE
        }
        if (new_size != data.size() * 2) co_return 4;
        if (dev->size_bytes() != new_size) co_return 4;

        // The kernel gendisk reports the new capacity.
        unsigned long long cap = 0;
        const int ir = co_await bdev_io([&] {
            return ::ioctl(fd, BLKGETSIZE64, &cap);
        });
        if (ir != 0 || cap != new_size) co_return 5;

        // The grown device still serves the original content.
        std::vector<uint8_t> buf(4096);
        REQUIRE(co_await bdev_io([&] {
                    return ::pread(fd, buf.data(), buf.size(),
                                   data.size() / 2);
                }) == 4096);
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

        co_await bdev_io([&] { return ::close(fd); });
        dev->stop();
        dev.reset();
        co_return 0;
    });
    if (rc == 3) {
        SKIP("kernel driver lacks UBLK_U_CMD_UPDATE_SIZE (needs the 6.16 "
             "cycle update)");
    }
    REQUIRE(rc == 0);
}

TEST_CASE("integration: ublk device survives server death via USER_RECOVERY",
          "[ublk]") {
    if (!ublk_available()) {
        SKIP("/dev/ublk-control unavailable (kernel ublk not enabled)");
    }
    // A child process creates and serves the device; the parent kills it
    // and attaches a replacement server (the real kernel handshake,
    // ADR-0010). The block device must survive and keep serving reads.
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);
    stage("recovery: fork");
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        // Child: serve until killed. No cleanup on SIGKILL — the kernel
        // keeps the device QUIESCED thanks to UBLK_F_USER_RECOVERY.
        ::close(pipefd[0]);
        const int rc = elio::run([](int out) -> elio::coro::task<int> {
            auto data = test::pattern_bytes(512 * 64, 84);
            source::BlobSourcePtr src =
                std::make_unique<test::VectorSource>(data);
            ublk::DeviceParams params;
            params.dev_sectors = data.size() / 512;
            params.enable_recovery = true;
            auto dev = co_await ublk::Device::create(params, std::move(src));
            const std::string report = dev->bdev_path() + "\n";
            const ssize_t w = ::write(out, report.data(), report.size());
            (void)w;
            for (;;) {
                co_await elio::time::sleep_for(std::chrono::hours(1));
            }
        }, pipefd[1]);
        _exit(rc);
    }
    ::close(pipefd[1]);
    std::string bdev;
    char ch;
    while (::read(pipefd[0], &ch, 1) == 1 && ch != '\n') bdev += ch;
    ::close(pipefd[0]);
    REQUIRE(!bdev.empty());
    const int dev_id = supervisor::dev_id_from_bdev_path(bdev);
    REQUIRE(dev_id >= 0);

    // The device answers reads while the first server is alive.
    const auto expected = test::pattern_bytes(512 * 64, 84);
    int fd = ::open(bdev.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    std::vector<uint8_t> buf(4096);
    REQUIRE(::pread(fd, buf.data(), buf.size(), 2048) == 4096);
    REQUIRE(buf == std::vector<uint8_t>(expected.begin() + 2048,
                                        expected.begin() + 2048 + 4096));
    ::close(fd);

    // Kill the server. If the kernel rejected USER_RECOVERY at ADD_DEV
    // (fallback path), the device disappears — skip rather than fail.
    REQUIRE(::kill(child, SIGKILL) == 0);
    int wstatus = 0;
    REQUIRE(::waitpid(child, &wstatus, 0) == child);
    // Let the kernel settle (quiesce, or teardown without recovery).
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (::access(bdev.c_str(), F_OK) != 0) {
        SKIP("kernel without USER_RECOVERY support");
    }

    // Attach a replacement server (this process) and keep reading.
    stage("recovery: attach");
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto data = test::pattern_bytes(512 * 64, 84);
        source::BlobSourcePtr src =
            std::make_unique<test::VectorSource>(data);
        ublk::DeviceParams params;
        params.dev_sectors = data.size() / 512;
        auto dev = co_await ublk::Device::attach(
            static_cast<uint32_t>(dev_id), params, std::move(src));
        const int fd2 = co_await bdev_io([&] {
            return ::open(dev->bdev_path().c_str(), O_RDONLY);
        });
        REQUIRE(fd2 >= 0);
        std::vector<uint8_t> buf2(4096);
        REQUIRE(co_await bdev_io([&] {
                    return ::pread(fd2, buf2.data(), buf2.size(), 2048);
                }) == 4096);
        REQUIRE(buf2 == std::vector<uint8_t>(expected.begin() + 2048,
                                             expected.begin() + 2048 + 4096));
        co_await bdev_io([&] { return ::close(fd2); });
        dev->stop();
        dev.reset();
        co_return 0;
    });
    REQUIRE(rc == 0);
}
