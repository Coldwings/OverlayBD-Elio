// Supervisor wire protocol (docs/supervisor.md; wire contract — changes
// need an ADR, T1).
//
// Two channels, both JSON-lines (one message per line, UTF-8, <= 64KiB):
//
//   obdctl ──UDS──▶ supervisor:    {"cmd":"create"|"destroy"|"list"|"status", ...}
//   supervisor ──▶ obdctl:         {"ok":true,...} | {"ok":false,"error":"..."}
//
//   obd-device ──socketpair──▶ supervisor: {"state":"starting"|"ready"|"failed"|"stopped", ...}
//   supervisor ──▶ obd-device:     signals only (SIGTERM = shutdown)
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace obd::supervisor {

/// Maximum JSON-line length on both channels.
inline constexpr size_t kMaxMessageBytes = 64 * 1024;

// --- obdctl → supervisor commands ------------------------------------------

struct CreateCommand {
    std::string id;          // unique device name within the supervisor
    std::string config;      // per-image config.json path (required)
    std::string global;      // overlaybd.json path ("" = supervisor default)
    std::string device_bin;  // obd-device path ("" = supervisor default)
    int dev_id = -1;         // requested ublk dev id (-1 = auto)
};

struct IdCommand {  // destroy / status
    std::string cmd;
    std::string id;
};

/// Parses one command line. Returns nullopt when the message is not a
/// valid command object; `error` receives a human-readable reason.
std::optional<nlohmann::json> parse_command(std::string_view line,
                                            std::string& error);

// --- supervisor → obdctl replies -------------------------------------------

std::string reply_ok(const nlohmann::json& fields = nlohmann::json::object());
std::string reply_error(const std::string& error);

// --- obd-device → supervisor status ----------------------------------------

struct DeviceStatus {
    std::string state;   // starting | ready | failed | stopped
    std::string device;  // /dev/ublkb<N> when ready
    std::string error;   // when failed
};

/// Parses a status line from a device child; nullopt on malformed input.
std::optional<DeviceStatus> parse_device_status(std::string_view line);

/// Builds a status line (obd-device side).
std::string make_device_status(const DeviceStatus& st);

/// Parses the device id out of a bdev path ("/dev/ublkb7" -> 7);
/// returns -1 when the path is not a ublk bdev path.
int dev_id_from_bdev_path(const std::string& path);

}  // namespace obd::supervisor
