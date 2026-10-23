#include "common/device_topology.hpp"

#include "common/errors.hpp"

#include <elio/runtime/scheduler.hpp>

#include <sched.h>

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace obd {

static_assert(kMaxDeviceWorkers == elio::runtime::scheduler::MAX_THREADS,
              "review the production topology limit when Elio's limit changes");

void validate_device_topology_config(const DeviceTopologyConfig& config) {
    if (config.queues > kMaxDeviceWorkers ||
        config.workers > kMaxDeviceWorkers) {
        throw DeviceTopologyError("ublkConfig queues/workers must be in range 0..256");
    }
    if (config.queues && config.workers && config.workers < config.queues) {
        throw DeviceTopologyError("ublkConfig.workers must be at least queues");
    }
}

DeviceTopology resolve_device_topology(
    const DeviceTopologyConfig& config, DeviceCpuBudget cpu,
    std::optional<uint32_t> recovery_queues) {
    validate_device_topology_config(config);
    uint32_t queues = config.queues;
    uint32_t workers = config.workers;
    if (recovery_queues) {
        if (*recovery_queues == 0 || *recovery_queues > kMaxDeviceWorkers) {
            throw DeviceTopologyError("existing ublk queue count exceeds runtime limits");
        }
        if (queues && queues != *recovery_queues) {
            throw DeviceTopologyError("ublkConfig.queues differs from existing device");
        }
        queues = *recovery_queues;
        if (!workers) workers = queues;
    } else if (queues || workers) {
        if (!queues) queues = workers;
        if (!workers) workers = queues;
    } else {
        queues = workers = std::min(cpu.available_cpus(), kAutoDeviceQueueLimit);
    }
    if (workers < queues) {
        throw DeviceTopologyError("ublkConfig.workers is smaller than existing queue count");
    }
    return {static_cast<uint16_t>(queues), workers, cpu,
            recovery_queues.has_value()};
}

namespace {

std::optional<uint64_t> unsigned_number(std::string_view text) {
    uint64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

std::string decode_mount_path(std::string_view text) {
    std::string result;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 3 < text.size() &&
            text[i + 1] >= '0' && text[i + 1] <= '7' &&
            text[i + 2] >= '0' && text[i + 2] <= '7' &&
            text[i + 3] >= '0' && text[i + 3] <= '7') {
            result += static_cast<char>((text[i + 1] - '0') * 64 +
                                       (text[i + 2] - '0') * 8 + text[i + 3] - '0');
            i += 3;
        } else {
            result += text[i];
        }
    }
    return result;
}

bool has_controller(std::string_view list, std::string_view wanted) {
    while (!list.empty()) {
        const auto comma = list.find(',');
        if (list.substr(0, comma) == wanted) return true;
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

struct Membership {
    std::filesystem::path path;
    bool unified;
};

bool namespace_local_path(const std::filesystem::path& path) {
    // cgroup namespaces can render outside roots as /.. or /../sibling.
    // Filesystem normalization would incorrectly turn these into local paths.
    return path.is_absolute() && std::none_of(path.begin(), path.end(),
        [](const auto& component) { return component == ".."; });
}

std::optional<uint32_t> quota_slots(std::string_view quota_text,
                                    std::string_view period_text) {
    const auto quota = unsigned_number(quota_text);
    const auto period = unsigned_number(period_text);
    if (!quota || !period || *quota == 0 || *period == 0) return std::nullopt;
    const auto slots = *quota / *period + (*quota % *period != 0);
    return static_cast<uint32_t>(std::min<uint64_t>(
        slots, std::numeric_limits<uint32_t>::max()));
}

std::optional<std::string> read_cpu_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) return std::nullopt;
    std::ostringstream text;
    text << file.rdbuf();
    if (file.bad()) return std::nullopt;
    return text.str();
}

uint32_t affinity_cpu_count() {
    // A fixed cpu_set_t fails on hosts whose possible CPU IDs exceed its
    // size, even when this process is allowed only a small subset of them.
    for (size_t bits = CPU_SETSIZE; bits <= (1u << 20); bits *= 2) {
        std::unique_ptr<cpu_set_t, decltype(&std::free)> mask(CPU_ALLOC(bits), &std::free);
        if (!mask) break;
        const auto bytes = CPU_ALLOC_SIZE(bits);
        if (::sched_getaffinity(0, bytes, mask.get()) == 0) {
            const auto count = CPU_COUNT_S(bytes, mask.get());
            if (count > 0) return static_cast<uint32_t>(count);
            break;
        }
        if (errno != EINVAL) break;
    }
    return std::max(std::thread::hardware_concurrency(), 1u);
}

}  // namespace

DeviceCpuBudget detail::device_cpu_budget_from_cgroups(
    uint32_t affinity_cpus, std::string_view membership,
    std::string_view mounts, const ReadCpuFile& read_file) {
    DeviceCpuBudget result{std::max(affinity_cpus, 1u), 0};
    std::vector<Membership> groups;
    std::istringstream membership_lines{std::string(membership)};
    for (std::string line; std::getline(membership_lines, line);) {
        const auto first = line.find(':');
        const auto second = first == std::string::npos ? first : line.find(':', first + 1);
        if (second == std::string::npos) continue;
        const auto controllers = std::string_view(line).substr(first + 1, second - first - 1);
        if (!controllers.empty() && !has_controller(controllers, "cpu")) continue;
        auto path = std::filesystem::path(line.substr(second + 1));
        if (namespace_local_path(path)) {
            groups.push_back({path.lexically_normal(), controllers.empty()});
        }
    }
    std::istringstream mount_lines{std::string(mounts)};
    for (std::string line; std::getline(mount_lines, line);) {
        const auto separator = line.find(" - ");
        if (separator == std::string::npos) continue;
        std::istringstream left(line.substr(0, separator));
        std::istringstream right(line.substr(separator + 3));
        std::string id, parent, device, root_text, mount_text;
        std::string type, source, options;
        if (!(left >> id >> parent >> device >> root_text >> mount_text) ||
            !(right >> type >> source >> options)) continue;
        const bool unified = type == "cgroup2";
        if (!unified && (type != "cgroup" || !has_controller(options, "cpu"))) continue;
        const auto raw_root = std::filesystem::path(decode_mount_path(root_text));
        const auto raw_mount = std::filesystem::path(decode_mount_path(mount_text));
        if (!namespace_local_path(raw_root) || !namespace_local_path(raw_mount)) continue;
        const auto root = raw_root.lexically_normal();
        const auto mount = raw_mount.lexically_normal();
        for (const auto& group : groups) {
            if (group.unified != unified) continue;
            const auto relative = group.path.lexically_relative(root);
            if (relative.empty() || relative.is_absolute() ||
                *relative.begin() == "..") continue;
            auto current = relative == "." ? mount : (mount / relative).lexically_normal();
            for (;;) {
                std::optional<uint32_t> slots;
                if (unified) {
                    if (const auto text = read_file(current / "cpu.max")) {
                        std::istringstream values(*text);
                        std::string quota, period;
                        if (values >> quota >> period) slots = quota_slots(quota, period);
                    }
                } else {
                    const auto quota = read_file(current / "cpu.cfs_quota_us");
                    const auto period = read_file(current / "cpu.cfs_period_us");
                    if (quota && period) {
                        std::istringstream quota_value(*quota), period_value(*period);
                        std::string q, p;
                        if (quota_value >> q && period_value >> p) slots = quota_slots(q, p);
                    }
                }
                if (slots) {
                    result.quota_cpus = result.quota_cpus
                        ? std::min(result.quota_cpus, *slots) : *slots;
                }
                if (current == mount) break;
                const auto next = current.parent_path();
                if (next == current) break;
                current = next;
            }
        }
    }
    return result;
}

DeviceCpuBudget probe_device_cpu_budget() {
    return detail::device_cpu_budget_from_cgroups(
        affinity_cpu_count(), read_cpu_file("/proc/self/cgroup").value_or(""),
        read_cpu_file("/proc/self/mountinfo").value_or(""), read_cpu_file);
}

}  // namespace obd
