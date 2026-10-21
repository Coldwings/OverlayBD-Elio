#pragma once

#include "supervisor/reap_test_evidence.hpp"

#include <sys/stat.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

namespace obd::test {

struct ChildReapSnapshot {
    uint64_t magic;
    uint64_t generation;
    uint64_t reaper_pid;
    uint64_t pid;
    int32_t wait_status;
    uint32_t version;
};

// A null result means no actual waitpid observation has been committed yet.
// Readers acquire the marker before touching the immutable published payload.
inline std::optional<ChildReapSnapshot> read_child_reap(
    const std::string& directory, uint64_t generation) {
    using Record = supervisor::detail::ChildReapRecord;
    const auto path = directory + "/child-" + std::to_string(generation) + ".reap";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) return std::nullopt;
        throw std::system_error(errno, std::generic_category(), "open child reap evidence");
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size != static_cast<off_t>(sizeof(Record))) {
        ::close(fd);
        throw std::runtime_error("child reap evidence has wrong size");
    }
    void* memory = ::mmap(nullptr, sizeof(Record), PROT_READ, MAP_SHARED, fd, 0);
    const int map_error = errno;
    ::close(fd);
    if (memory == MAP_FAILED)
        throw std::system_error(map_error, std::generic_category(), "map child reap evidence");
    const auto* record = static_cast<const Record*>(memory);
    const auto committed = record->committed.load(std::memory_order_acquire);
    std::optional<ChildReapSnapshot> result;
    if (committed == 1) {
        result = ChildReapSnapshot{record->magic, record->generation, record->reaper_pid,
                                  record->pid, record->wait_status, record->version};
    }
    ::munmap(memory, sizeof(Record));
    if (committed > 1 || (result &&
        (result->magic != supervisor::detail::kChildReapMagic ||
         result->version != supervisor::detail::kChildReapVersion ||
         result->generation != generation || result->pid == 0 || result->reaper_pid == 0))) {
        throw std::runtime_error("child reap evidence has invalid identity/header");
    }
    return result;
}

} // namespace obd::test
