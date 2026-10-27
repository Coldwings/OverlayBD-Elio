// Local integrity metadata; never changes the upstream Hybrid byte format.
#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace obd::format::hybrid {
inline constexpr char kLogStateAttribute[] = "user.overlaybd.hybrid.log";
struct LogStamp {
    std::array<uint8_t, 16> generation{};
    uint64_t bytes = 4096;
    uint32_t crc = 0;
    bool operator==(const LogStamp&) const = default;
};
// Blocking metadata operations: callers run these off scheduler workers.
// Inspect is read-only. It may select an earlier verified prefix after an
// interrupted append. Activation happens only after semantic replay succeeds.
std::optional<LogStamp> inspect_log_state(int data_fd, int index_fd);
LogStamp activate_log_state(int data_fd, int index_fd, int directory_fd,
                           std::optional<LogStamp> recovered);
LogStamp new_log_stamp(int index_fd);
void publish_log_progress(int index_fd, const LogStamp& stamp);
// Caller must sync data, index (including xattrs), and index directory first.
void publish_log_commit(int data_fd, const LogStamp& stamp);
// Both descriptors remain locked; new index must already have its progress.
void prepare_log_replacement(int data_fd, int old_index_fd, int new_index_fd,
                             const LogStamp& old_stamp, const LogStamp& next_stamp);
#ifdef OBD_TEST_HOOKS
// 1=installing anchor synced, 2=index progress written,
// 3=stable anchor written before sync. Reset with -1.
void log_state_fault_for_test(int stage);
#endif
}  // namespace obd::format::hybrid
