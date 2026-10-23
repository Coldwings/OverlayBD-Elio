#pragma once

#ifdef OBD_TEST_HOOKS
#include "ublk/startup_test_hooks.hpp"
#include "source/blob_source.hpp"
#include "common/errors.hpp"
#include <elio/time/timer.hpp>

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
            bool adopted_source = false;
            bool report_refusal = false;
            int marker = ::openat(dir, "adopted-source-v1", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (marker >= 0) adopted_source = true;
            else if (errno == ENOENT) {
                marker = ::openat(dir, "report-refusal-v1", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
                if (marker >= 0) report_refusal = true;
                else if (errno == ENOENT)
                    marker = ::openat(dir, "partial-recovery-v1", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            }
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
            log_->adopted_source = adopted_source ? 1 : 0;
            log_->report_refusal = report_refusal ? 1 : 0;
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
        (void)queue;
    }
    elio::coro::task<void> before_initial_submit_async(uint16_t queue) override {
        if (!log_->recovery || log_->adopted_source || queue != 1) co_return;
        observe({.kind = ublk::detail::StartupTestKind::initial_gate_enter, .queue = queue});
        // Hold prepared FETCH debt without blocking its fixed Elio worker.
        // Controller loss or a test timeout cannot fabricate issuance.
        while (!log_->queue_one_open.load(std::memory_order_acquire)) {
            try { co_await elio::time::sleep_for(std::chrono::milliseconds(1)); }
            catch (...) {} // Cancellation cannot bypass the issuance fence.
        }
        observe({.kind = ublk::detail::StartupTestKind::initial_gate_leave, .queue = queue});
        co_return;
    }
    void before_failure_report(uint64_t revision) {
        if (!log_->recovery || !log_->report_refusal || report_thrown_.exchange(true)) return;
        report_catch_pending_.store(true);
        observe({.kind = ublk::detail::StartupTestKind::failure_report_throw, .result = -ENOMEM, .value = revision});
        throw std::bad_alloc();
    }
    void failure_report_caught() noexcept {
        if (report_catch_pending_.exchange(false))
            observe({.kind = ublk::detail::StartupTestKind::failure_report_caught, .result = -ENOMEM});
    }
    void failure_report_constructed(uint64_t revision) noexcept {
        if (report_thrown_.load() && !report_constructed_.exchange(true))
            observe({.kind = ublk::detail::StartupTestKind::failure_report_constructed, .value = revision});
    }
    void before_release_attempt(uint64_t sequence) override {
        if (!log_->recovery || !log_->report_refusal || sequence != 1 || release_thrown_.exchange(true)) return;
        observe({.kind = ublk::detail::StartupTestKind::release_setup_throw, .result = -ENOMEM,
                 .release_sequence = sequence});
        // Local controller setup failure before release_once and any STOP SQE.
        // Its caller's existing exception boundary settles this attempt.
        throw std::bad_alloc();
    }
    bool holds_source() const noexcept { return log_->recovery && log_->adopted_source; }
    elio::coro::task<void> after_start_control() override {
        if (!holds_source()) co_return;
        observe({.kind = ublk::detail::StartupTestKind::post_control_enter});
        while (!log_->post_control_open.load(std::memory_order_acquire)) {
            try { co_await elio::time::sleep_for(std::chrono::milliseconds(1)); }
            catch (...) {} // A cancellation cannot silently release the checkpoint.
        }
        observe({.kind = ublk::detail::StartupTestKind::post_control_fail, .result = -EIO});
        throw obd::error(EIO, "fixture local failure after successful END");
    }
    elio::coro::task<void> wait_source() {
        while (!log_->source_open.load(std::memory_order_acquire)) {
            try { co_await elio::time::sleep_for(std::chrono::milliseconds(1)); }
            catch (...) {} // Preserve the actual buffer loan until controller release.
        }
    }
private:
    ublk::detail::StartupFixtureLog* log_ = nullptr;
    std::atomic<bool> report_thrown_{false}, report_catch_pending_{false};
    std::atomic<bool> report_constructed_{false}, release_thrown_{false};
};

// Installed only around the replacement's real, read-only assembled root.
// The real source operation owns the actual queue buffer across this async gate.
class StartupBorrowedSource final : public source::BlobSource {
public:
    StartupBorrowedSource(source::BlobSourcePtr inner, StartupFixture& fixture)
        : inner_(std::move(inner)), fixture_(fixture) {}
    ~StartupBorrowedSource() override {
        fixture_.observe({.kind = ublk::detail::StartupTestKind::source_destroyed});
    }
    elio::coro::task<ssize_t> pread(void* buffer, size_t count, uint64_t offset) override {
        const uint64_t loan = next_loan_.fetch_add(1);
        ublk::detail::StartupTestEvent event{.kind = ublk::detail::StartupTestKind::source_enter,
            .cookie = loan, .buffer = reinterpret_cast<uintptr_t>(buffer), .offset = offset, .length = count};
        fixture_.observe(event);
        // This destructor proves lexical loan-scope exit, not final coroutine
        // frame destruction. The Device's queue-owner destruction is separate.
        struct Scope {
            StartupFixture& fixture;
            ublk::detail::StartupTestEvent event;
            ~Scope() { event.kind = ublk::detail::StartupTestKind::source_scope_exit; fixture.observe(event); }
        } scope{fixture_, event};
        ssize_t result = -EIO;
        try {
            co_await fixture_.wait_source();
            result = co_await inner_->pread(buffer, count, offset);
        } catch (...) { result = -EIO; }
        event.kind = ublk::detail::StartupTestKind::source_result;
        event.result = static_cast<int32_t>(result);
        if (result > 0) {
            uint64_t checksum = 14695981039346656037ULL;
            const auto* bytes = static_cast<const uint8_t*>(buffer);
            for (ssize_t index = 0; index < result; ++index) {
                checksum ^= bytes[index];
                checksum *= 1099511628211ULL;
            }
            event.value = checksum;
        }
        fixture_.observe(event);
        co_return result;
    }
    uint64_t size() const noexcept override { return inner_->size(); }
    std::string_view label() const noexcept override { return inner_->label(); }
private:
    source::BlobSourcePtr inner_;
    StartupFixture& fixture_;
    std::atomic<uint64_t> next_loan_{1};
};

} // namespace obd::cmd
#endif
