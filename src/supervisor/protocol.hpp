// Supervisor wire protocol (docs/supervisor.md; wire contract — changes
// need an ADR, T1).
//
// Two channels, both JSON-lines (one message per line, UTF-8, <= 64KiB):
//
//   obdctl --UDS--> supervisor:    {"cmd":"hello"|"create"|"destroy"|"list"|"status"|"commit"|"trace_start"|"trace_stop", ...}
//   supervisor --> obdctl:         {"ok":true,...} | {"ok":false,"error":"..."}
//
//   obd-device --socketpair--> supervisor: {"state":"starting"|"ready"|"failed"|"stopped", ...}
//                                        | {"reply":"trace_start"|"trace_stop"|"trace_event", ...}
//   supervisor --> obd-device:     signals (SIGTERM = shutdown), plus
//                                  JSON-lines commands on the same
//                                  socketpair: {"cmd":"trace_start"|"trace_stop", ...}
//                                  (ADR-0013 record path; devices that
//                                  predate trace control never read the
//                                  channel, so old pairings degrade to
//                                  "device control channel timeout")
//
// Additive-only evolution rule (current law; governing decision ADR-0014,
// currently proposed):
//   - New commands and new reply fields may be added; existing field names
//     and meanings never change.
//   - Servers ignore unknown request fields; clients must ignore unknown
//     reply fields.
//   - kProtocolVersion increments only for additive batches and, together
//     with the `features` list from the `hello` reply, is the client's
//     capability gate.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace obd::supervisor {

/// Maximum JSON-line length on both channels.
inline constexpr size_t kMaxMessageBytes = 64 * 1024;

/// Control-protocol revision. Starts at 1 and increments only for additive
/// batches (see the additive-only rule above); clients gate on it together
/// with the `features` list from the `hello` reply.
///   1 — hello/create/destroy/list/status.
///   2 — adds commit (ADR-0014 offline seal; feature "commit").
///   3 — adds trace_start/trace_stop (ADR-0013 record path; feature
///       "trace"), the bidirectional supervisor-to-device command
///       channel, and the additive "trace" field in status/list replies.
inline constexpr int kProtocolVersion = 3;

/// Project version string, wired from CMake `project(... VERSION ...)` so
/// it cannot drift; "dev" is the fallback for non-CMake builds.
#ifdef OBD_VERSION_STRING
inline constexpr std::string_view kProjectVersion = OBD_VERSION_STRING;
#else
inline constexpr std::string_view kProjectVersion = "dev";
#endif

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

struct CommitCommand {  // commit (ADR-0014: offline seal of the upper)
    std::string id;
    std::string user_tag;  // optional; recorded in the sealed header
};

// Trace recording (ADR-0013 record path): wire shapes.
//
//   obdctl -> supervisor:
//     {"cmd":"trace_start","id":"<device>","path":"<abs output file>",
//      "duration_sec":<1..3600>}
//     {"cmd":"trace_stop","id":"<device>"}
//   supervisor -> device (control socketpair):
//     {"cmd":"trace_start","path":"...","duration_sec":N}
//     {"cmd":"trace_stop"}
//   device -> supervisor (same socketpair, "reply" discriminator):
//     {"reply":"trace_start","ok":true,"path":"...","duration_sec":N}
//     {"reply":"trace_stop","ok":true,"path":"...","sha256":"<hex>",
//      "size":N,"records":N,"dropped":N}
//     {"reply":"trace_event","event":"expired","path":"...",
//      "sha256":"<hex>","size":N,"records":N,"dropped":N}
//     (error shape for the two replies: {"reply":"<cmd>","ok":false,
//      "error":"..."})
//   obdctl <- supervisor: the device reply fields plus "id"; an additive
//     "trace" object in status/list replies carries the latest state:
//     {"state":"recording"|"stopped","path":...,"duration_sec":...,
//      "reason":"stopped"|"expired","sha256":...,"size":...,
//      "records":...,"dropped":...}

/// True when a device-to-supervisor line is a command reply/event (the
/// "reply" discriminator) rather than a lifecycle status.
bool is_device_reply_line(const nlohmann::json& j);

/// Parses one command line. Returns nullopt when the message is not a
/// valid command object; `error` receives a human-readable reason.
std::optional<nlohmann::json> parse_command(std::string_view line,
                                            std::string& error);

// --- supervisor → obdctl replies -------------------------------------------

std::string reply_ok(const nlohmann::json& fields = nlohmann::json::object());
std::string reply_error(const std::string& error);

/// The `hello` handshake reply: protocol revision, project version, and
/// the (initially empty) feature list, via the reply_ok envelope.
std::string reply_hello();

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
