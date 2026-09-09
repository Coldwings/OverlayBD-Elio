// Supervisor wire protocol (docs/supervisor.md; wire contract — changes
// need an ADR, T1).
//
// Two channels, both JSON-lines (one message per line, UTF-8, <= 64KiB):
//
//   obdctl --UDS--> supervisor:    {"cmd":"hello"|"create"|"destroy"|"list"|"status"|"commit"|"trace_start"|"trace_stop"|"resize", ...}
//   supervisor --> obdctl:         {"ok":true,...} | {"ok":false,"error":"..."}
//   (create is one command with two modes: image via "config", or blank
//    raw via the additive "blank" object — ADR-0014 modes 2/3:
//    {"cmd":"create","id":"...","blank":{"size":<bytes>[,"mkfs":"<type>"]}})
//
//   obd-device --socketpair--> supervisor: {"state":"starting"|"ready"|"failed"|"stopped", ...}
//                                        | {"reply":"trace_start"|"trace_stop"|"trace_event"|"resize", ...}
//   supervisor --> obd-device:     signals (SIGTERM = shutdown), plus
//                                  JSON-lines commands on the same
//                                  socketpair: {"cmd":"trace_start"|"trace_stop"|"resize", ...}
//                                  (ADR-0013 record path + D3 resize;
//                                  devices that predate command control
//                                  never read the channel, so old
//                                  pairings degrade to "device control
//                                  channel timeout")
//
// Additive-only evolution rule (current law; governing decision ADR-0014):
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
///   4 — adds the resize command and the optional create/commit
///       `virtual_size` fields (D3 grow-only resize chain; feature
///       "resize"), and the additive create "blank" object: blank (raw)
///       device creation modes 2/3 of ADR-0014 (feature "blank").
inline constexpr int kProtocolVersion = 4;

/// Project version string, wired from CMake `project(... VERSION ...)` so
/// it cannot drift; "dev" is the fallback for non-CMake builds.
#ifdef OBD_VERSION_STRING
inline constexpr std::string_view kProjectVersion = OBD_VERSION_STRING;
#else
inline constexpr std::string_view kProjectVersion = "dev";
#endif

// --- obdctl → supervisor commands ------------------------------------------

/// ADR-0014 blank (raw) device spec — the additive "blank" object that
/// turns `create` into blank-device creation (no image config):
///
///   {"cmd":"create","id":"<name>","blank":{"size":<bytes>}}
///   {"cmd":"create","id":"<name>","blank":{"size":<bytes>,"mkfs":"<type>"}}
///
/// Mode 2 (`size` only): the device serves a zeroed block device of `size`
/// bytes — a sealed EMPTY LSMT layer as the zero base with a writable
/// LSMT-RW upper from birth (docs/format.md, image.md). Mode 3 adds `mkfs`:
/// after the device is up, the supervisor runs host `mkfs.<type>` on the
/// new block device before create replies — a RUNTIME-ONLY convenience
/// whose output is never an image-build input (non-deterministic;
/// ADR-0014, docs/operations.md).
struct BlankSpec {
    uint64_t size = 0;  // bytes; > 0, multiple of 512
    std::string mkfs;   // "" = no mkfs (mode 2); else mkfs.<type> (mode 3)
};

/// Operator sanity bound for blank device sizes (bytes, 16 TiB). Blank
/// devices allocate no data up front (the zero base is an empty layer), so
/// this only guards against a typo'd size; value-level validation lives in
/// parse_blank_spec (docs/supervisor.md).
inline constexpr uint64_t kMaxBlankSizeBytes = uint64_t{1} << 44;

/// True when `type` is a safe `mkfs.<type>` suffix (lowercase alphanumeric
/// plus '_', 1..16 chars). The supervisor execs `mkfs.<type>` by name, so
/// the charset is the injection boundary; a type that fails this check is
/// rejected at parse time.
bool valid_mkfs_type(const std::string& type);

/// Parses the additive "blank" object (value level): validates size > 0,
/// 512-aligned, within kMaxBlankSizeBytes, and an optional mkfs type via
/// valid_mkfs_type. Returns nullopt with `error` set when `blank` is absent
/// or invalid. Field TYPES are validated earlier, in parse_command.
std::optional<BlankSpec> parse_blank_spec(const nlohmann::json& blank,
                                          std::string& error);

struct CreateCommand {
    std::string id;          // unique device name within the supervisor
    std::string config;      // per-image config.json path (required in
                             // image mode; blank mode carries BlankSpec
                             // instead — see docs/supervisor.md)
    std::string global;      // overlaybd.json path ("" = supervisor default)
    std::string device_bin;  // obd-device path ("" = supervisor default)
    int dev_id = -1;         // requested ublk dev id (-1 = auto)
    /// D3 create-time headroom: optional dev_size override in bytes
    /// (0 = absent; the device is sized to the image's virtual size).
    /// Positive + 512-aligned, and grow-only vs the image size (checked
    /// device-side after assembly, where the image size is known).
    uint64_t virtual_size = 0;
};

struct IdCommand {  // destroy / status
    std::string cmd;
    std::string id;
};

struct CommitCommand {  // commit (ADR-0014: offline seal of the upper)
    std::string id;
    std::string user_tag;  // optional; recorded in the sealed header
    /// D3 commit re-baseline: optional sealed virtual size in bytes
    /// (0 = keep the checkpointed size). Grow-only: must be >= the
    /// layer's declared size and its content extent (validated in the
    /// seal path).
    uint64_t virtual_size = 0;
};

// Trace recording (ADR-0013 record path): wire shapes.
//
//   obdctl -> supervisor:
//     {"cmd":"trace_start","id":"<device>","path":"<abs output file>",
//      "duration_sec":<1..3600>}
//     {"cmd":"trace_stop","id":"<device>"}
//   supervisor -> device (control socketpair):
//     {"cmd":"trace_start","path":"...","duration_sec":N,"seq":N}
//     {"cmd":"trace_stop","seq":N}
//     ("seq" is the supervisor's per-command correlation token, fresh
//      per forwarded command; additive — older supervisors omit it)
//   device -> supervisor (same socketpair, "reply" discriminator):
//     {"reply":"trace_start","ok":true,"path":"...","duration_sec":N,
//      "seq":N}
//     {"reply":"trace_stop","ok":true,"path":"...","sha256":"<hex>",
//      "size":N,"records":N,"dropped":N,"seq":N}
//     (replies echo the command's "seq" when present; the supervisor
//      drops a reply whose seq does not match the pending command — a
//      late reply to a timed-out command never completes the next one)
//     {"reply":"trace_event","event":"expired","path":"...",
//      "sha256":"<hex>","size":N,"records":N,"dropped":N}
//     (unsolicited, no seq; applied only while the entry's trace state
//      is "recording", so a stale expiry cannot overwrite a NEW
//      recording's status)
//     (error shape for the two replies: {"reply":"<cmd>","ok":false,
//      "error":"..."})
//   obdctl <- supervisor: the device reply fields plus "id"; an additive
//     "trace" object in status/list replies carries the latest state:
//     {"state":"recording"|"stopped"|"lost","path":...,
//      "duration_sec":...,
//      "reason":"stopped"|"expired"|"device_exit","sha256":...,"size":...,
//      "records":...,"dropped":...}

// D3 grow-only online resize (ADR-0014 dev_size model): wire shapes.
//
//   obdctl -> supervisor:
//     {"cmd":"resize","id":"<device>","size":<bytes>}
//     (size must be a positive multiple of 512 — validated supervisor-
//      side before forwarding; whether a request GROWS is decided
//      device-side, where the current size is known)
//   supervisor -> device (control socketpair):
//     {"cmd":"resize","size":<bytes>,"seq":N}
//   device -> supervisor (same socketpair, "reply" discriminator):
//     {"reply":"resize","ok":true,"size":<new bytes>,"seq":N}
//     {"reply":"resize","ok":false,"error":"...","seq":N}
//     (grow-only: the device rejects a request <= its current size with
//      ok:false BEFORE issuing any kernel command)
//   obdctl <- supervisor: the device reply fields plus "id":
//     {"ok":true,"size":<new bytes>,"id":"<device>"}
//     (additive-only: older supervisors reject "resize" as an unknown
//      cmd — clients gate on the "resize" feature from hello)

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
