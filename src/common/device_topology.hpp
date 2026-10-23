// Production device startup topology (ADR-0025). Counts are resolved before
// scheduler construction and remain fixed throughout the device lifetime.
#pragma once

#include "common/errors.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace obd {

inline constexpr uint32_t kAutoDeviceQueueLimit = 8;
inline constexpr uint32_t kMaxDeviceWorkers = 256;

/// Invalid topology settings are usage errors. Operational preflight failures
/// retain the device entry point's existing owned startup/error-report path.
class DeviceTopologyError : public error {
public:
    explicit DeviceTopologyError(std::string context)
        : error(EINVAL, std::move(context)) {}
};

struct DeviceTopologyConfig {
    uint32_t queues = 0;   // 0 = automatic / match the explicit counterpart
    uint32_t workers = 0;  // 0 = automatic / match the explicit counterpart
};

struct DeviceCpuBudget {
    uint32_t affinity_cpus = 1;
    uint32_t quota_cpus = 0;  // 0 = no finite visible cgroup quota

    uint32_t available_cpus() const noexcept {
        const auto allowed = std::max(affinity_cpus, 1u);
        return quota_cpus ? std::min(allowed, quota_cpus) : allowed;
    }
};

struct DeviceTopology {
    uint16_t queues = 1;
    uint32_t workers = 1;
    DeviceCpuBudget cpu;
    bool recovery = false;
};

/// Reject unsupported counts and an explicitly smaller worker count.
/// Zero is the automatic sentinel; explicit values are 1..256.
void validate_device_topology_config(const DeviceTopologyConfig& config);

/// Both automatic: min(available CPU budget, 8), with at least one pair.
/// One explicit count: the other matches it. Both explicit: workers >= queues.
/// Recovery instead preserves the kernel queue count; an explicit queue count
/// must match it, and automatic workers match that existing count.
DeviceTopology resolve_device_topology(
    const DeviceTopologyConfig& config, DeviceCpuBudget cpu,
    std::optional<uint32_t> recovery_queues = std::nullopt);

/// Cold, synchronous probe; call before starting the Elio scheduler. Combines
/// process affinity with finite cgroup v1/v2 quotas visible through its mount
/// namespace. Fractional quotas round up to a positive number of workers.
DeviceCpuBudget probe_device_cpu_budget();

namespace detail {
using ReadCpuFile = std::function<
    std::optional<std::string>(const std::filesystem::path&)>;

// Shared by the real cold probe and filesystem-free cgroup fixtures.
DeviceCpuBudget device_cpu_budget_from_cgroups(
    uint32_t affinity_cpus, std::string_view membership,
    std::string_view mounts, const ReadCpuFile& read_file);
}  // namespace detail

}  // namespace obd
