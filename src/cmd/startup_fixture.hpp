#pragma once

#ifdef OBD_TEST_HOOKS
#include "ublk/startup_test_hooks.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <new>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

namespace obd::cmd {

// Explicit opt-in for the real-kernel production fixture. The directory and
// its fixed marker are created by the test controller, and retained on failure.
// Files are exclusive per invocation; this class never unlinks evidence.
class StartupFixture final : public ublk::detail::StartupTestHooks {
public:
    StartupFixture(const std::string& directory, uint64_t generation, bool recovery) {
        const int dir = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (dir < 0) throw std::system_error(errno, std::generic_category(), "startup fixture directory");
        int fd = -1;
        try {
            const int marker = ::openat(dir, "partial-recovery-v1", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (marker < 0) throw std::runtime_error("startup fixture marker is missing");
            ::close(marker);
            const auto filename = std::to_string(::getpid()) + "-" +
                                  std::to_string(generation) + ".events";
            fd = ::openat(dir, filename.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd < 0) throw std::system_error(errno, std::generic_category(), "startup fixture log");
            if (::ftruncate(fd, sizeof(ublk::detail::StartupFixtureLog)) != 0)
                throw std::system_error(errno, std::generic_category(), "startup fixture log size");
            void* memory = ::mmap(nullptr, sizeof(ublk::detail::StartupFixtureLog),
                                  PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (memory == MAP_FAILED)
                throw std::system_error(errno, std::generic_category(), "startup fixture mmap");
            log_ = new (memory) ublk::detail::StartupFixtureLog{};
            log_->generation = generation;
            log_->pid = static_cast<uint64_t>(::getpid());
            std::ifstream process("/proc/self/stat");
            std::string stat;
            std::getline(process, stat);
            const auto command_end = stat.rfind(')');
            if (command_end == std::string::npos)
                throw std::runtime_error("startup fixture process identity unavailable");
            std::istringstream fields(stat.substr(command_end + 1));
            std::string ignored;
            for (unsigned field = 3; field < 22; ++field) fields >> ignored;
            if (!(fields >> log_->process_start_ticks) || log_->process_start_ticks == 0)
                throw std::runtime_error("startup fixture process identity invalid");
            log_->recovery = recovery ? 1 : 0;
            log_->initialized.store(1, std::memory_order_release);
            ::close(fd);
            ::close(dir);
        } catch (...) {
            if (log_) { ::munmap(log_, sizeof(*log_)); log_ = nullptr; }
            if (fd >= 0) ::close(fd);
            ::close(dir);
            throw;
        }
    }
    ~StartupFixture() override {
        if (log_) ::munmap(log_, sizeof(*log_));
    }
    StartupFixture(const StartupFixture&) = delete;
    StartupFixture& operator=(const StartupFixture&) = delete;

    void observe(ublk::detail::StartupTestEvent event) noexcept override {
        const auto index = log_->allocated.fetch_add(1, std::memory_order_relaxed);
        if (index >= log_->records.size()) {
            log_->overflow.store(1, std::memory_order_release);
            return;
        }
        auto& record = log_->records[index];
        timespec now{};
        (void)::clock_gettime(CLOCK_MONOTONIC, &now);
        record.monotonic_ns = static_cast<uint64_t>(now.tv_sec) * 1000000000ULL +
                              static_cast<uint64_t>(now.tv_nsec);
        record.event = event;
        record.committed.store(index + 1, std::memory_order_release);
    }
    void before_initial_submit(uint16_t queue) noexcept override {
        if (!log_->recovery || queue != 1) return;
        observe({.kind = ublk::detail::StartupTestKind::initial_gate_enter, .queue = queue});
        // Deliberately blocks only this queue owner at its pre-existing test
        // seam. Controller loss or a test timeout cannot fabricate issuance.
        while (!log_->queue_one_open.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        observe({.kind = ublk::detail::StartupTestKind::initial_gate_leave, .queue = queue});
    }
private:
    ublk::detail::StartupFixtureLog* log_ = nullptr;
};

} // namespace obd::cmd
#endif
