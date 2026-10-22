#pragma once

#ifdef OBD_TEST_HOOKS
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>

namespace obd::supervisor::detail {

// Internal fixture format, not a supervisor protocol or a child self-report.
// Only Child's successful owned-PID waitpid publishes the committed payload.
inline constexpr uint64_t kChildReapMagic = 0x4f42445245415031ULL;
inline constexpr uint32_t kChildReapVersion = 1;
struct ChildReapRecord {
    uint64_t magic = kChildReapMagic;
    uint64_t generation = 0;
    uint64_t reaper_pid = 0;
    uint64_t pid = 0;
    int32_t wait_status = 0;
    uint32_t version = kChildReapVersion;
    std::atomic<uint32_t> committed{0};
};
static_assert(std::atomic<uint32_t>::is_always_lock_free);

// Per-Child owned storage, prepared before fork. Preparation failure only
// removes test evidence: it must not change spawn, ownership or collection.
// The controller rejects missing/incomplete evidence. No borrowed spawn hook
// survives into this object, and publication performs no I/O or allocation.
class ChildReapEvidence {
public:
    ChildReapEvidence() = default;
    ChildReapEvidence(const ChildReapEvidence&) = delete;
    ChildReapEvidence& operator=(const ChildReapEvidence&) = delete;
    ~ChildReapEvidence() {
        if (record_) ::munmap(record_, sizeof(*record_));
    }

    void prepare(const char* directory, uint64_t generation) noexcept {
        struct RestoreErrno {
            int saved = errno;
            ~RestoreErrno() { errno = saved; }
        } restore_errno;
        if (!directory || !*directory || generation == 0) return;
        struct Fd {
            int value;
            ~Fd() { if (value >= 0) ::close(value); }
        };
        const Fd dir{::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
        if (dir.value < 0) return;
        Fd marker{::openat(dir.value, "partial-recovery-v1",
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
        if (marker.value < 0 && errno == ENOENT)
            marker.value = ::openat(dir.value, "adopted-source-v1",
                                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (marker.value < 0 && errno == ENOENT)
            marker.value = ::openat(dir.value, "report-refusal-v1",
                                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (marker.value < 0) return;
        char filename[64];
        const int length = std::snprintf(filename, sizeof(filename), "child-%llu.reap",
                                        static_cast<unsigned long long>(generation));
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(filename)) return;
        const Fd file{::openat(dir.value, filename,
            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
        if (file.value < 0 || ::ftruncate(file.value, sizeof(ChildReapRecord)) != 0) return;
        void* memory = ::mmap(nullptr, sizeof(ChildReapRecord),
                              PROT_READ | PROT_WRITE, MAP_SHARED, file.value, 0);
        if (memory == MAP_FAILED) return;
        record_ = new (memory) ChildReapRecord{};
        record_->generation = generation;
        record_->reaper_pid = static_cast<uint64_t>(::getpid());
    }

    void publish(pid_t pid, int wait_status) noexcept {
        if (!record_) return;
        record_->pid = static_cast<uint64_t>(pid);
        record_->wait_status = static_cast<int32_t>(wait_status);
        record_->committed.store(1, std::memory_order_release);
    }

private:
    ChildReapRecord* record_ = nullptr;
};

} // namespace obd::supervisor::detail
#endif
