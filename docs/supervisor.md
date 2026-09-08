# supervisor — daemon, per-device child lifecycle, control protocol

## Overview

The `supervisor` module implements the project's process model (ADR-0004:
**one isolated process per block device**). A single long-lived daemon,
`obd-supervisor`, owns no images and touches no ublk devices itself; it
spawns, monitors, and reaps one `obd-device` child process per block device.
A device crash never takes down its siblings — the supervisor reaps the
corpse, records the exit status, and keeps serving.

The module has three parts:

- **Wire protocol** (`src/supervisor/protocol.hpp`) — two JSON-lines
  channels: the `obdctl` → supervisor control channel (Unix domain socket)
  and the `obd-device` → supervisor status channel (socketpair on fd 3).
- **Child lifecycle** (`src/supervisor/child.hpp`) — spawn
  (socketpair + fork + execve), signal, reap, and publish per-child status.
- **Daemon** (`src/supervisor/daemon.hpp`) — the accept loop, the children
  registry, the SIGCHLD reaper, and the hello/create/destroy/list/status
  command handlers with bounded ready/exit waits.

Both protocols are **wire contracts between independently upgradeable
binaries**: changes require an ADR (T1, per AGENTS.md).

## Concepts

### Channel 1: obdctl → supervisor (control socket)

A Unix domain stream socket (default
`/run/overlaybd-elio/supervisor.sock`). Framing is **JSON-lines**: one
UTF-8 JSON object per line, maximum line length **64 KiB**
(`kMaxMessageBytes`) on both channels. Connections are **one-shot**: the
client sends exactly one command line, the supervisor replies with exactly
one line and closes.

Requests (validated by `parse_command` in src/supervisor/protocol.cpp —
these are the real field names):

```json
{"cmd":"hello"}

{"cmd":"create","id":"<name>","config":"<config.json path>",
 "global":"<overlaybd.json path, optional>",
 "device_bin":"<obd-device path, optional>",
 "dev_id":<int, optional, -1/absent = auto>}

{"cmd":"destroy","id":"<name>"}
{"cmd":"status","id":"<name>"}
{"cmd":"list"}
```

Validation: the message must be a JSON object with a string `cmd`;
`create` requires `id` and `config`; `destroy`/`status` require `id`;
`hello` and `list` take no fields; anything else is "unknown cmd".
Malformed input is answered, not dropped.

Replies (`reply_ok` / `reply_error`): success is `{"ok":true,...}` with
command-specific fields merged in; failure is
`{"ok":false,"error":"<human-readable reason>"}`. The success shapes by
command (`src/supervisor/daemon.cpp`):

- **hello**: `{"ok":true,"protocol":<int>,"version":"<project version>",
  "features":[...]}` — the handshake. `protocol` is the control-protocol
  revision (`kProtocolVersion`, starts at 1); `version` is the project
  version string wired from CMake (`kProjectVersion`); `features` is a
  JSON array of strings, initially empty, reserved as the extension point
  for optional capabilities.
- **create**: `{"ok":true,"id","pid","device"}` — `device` is the child's
  reported `/dev/ublkb<N>`.
- **destroy**: `{"ok":true,"id"}`.
- **list**: `{"ok":true,"devices":[{"id","pid","state","device","error"}, ...]}`.
- **status**: `{"ok":true,"id","pid","state","device","error","exit_code"}`
  (`exit_code` is -1 until the child is reaped).

### Additive-only evolution rule

The control protocol evolves **additively only** (governing decision:
ADR-0014, currently proposed). Concretely:

- New commands and new reply fields **may be added**; existing field names
  and meanings **never change**.
- Servers **ignore unknown request fields**; clients **must ignore unknown
  reply fields**.
- `protocol` increments only for additive batches and, together with the
  `features` list from the `hello` reply, is the client's capability gate:
  a client that needs a capability checks `protocol`/`features` once at
  handshake time instead of discovering breakage at runtime.

This rule is what allows obdctl, obd-supervisor, and any external
(non-C++) CLI to be upgraded independently.

### Channel 2: obd-device → supervisor (status channel, fd 3)

A `socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC)` created before fork.
The child end is dup2'd to **fd 3**, cleared of CLOEXEC, and survives
`execve`; the device binary receives `--control-fd 3` on its argv. Same
JSON-lines framing, same 64 KiB cap. The channel is **one-directional**:
the device writes status lines, the supervisor only reads; the supervisor
controls the child via **signals** (SIGTERM = shutdown request, SIGKILL =
force).

Status lines (`DeviceStatus` / `make_device_status`):

```json
{"state":"starting"}
{"state":"ready","device":"/dev/ublkb<N>"}
{"state":"failed","error":"<reason>"}
{"state":"stopped"}
```

`state` is required (string); `device` and `error` are optional and
omitted when empty. The supervisor additionally maintains one internal
state, `exited`, which the child never sends: it is assigned when the
status channel hits EOF or the reaper collects the process
(`Child::note_reaped`). The externally visible lifecycle is therefore:

```
starting → ready → stopped → exited
starting → failed ─────────→ exited   (exec failure: exit code 127)
starting ──────(crash)─────→ exited   (reaper fills exit_code)
```

### Spawning contract

`Child::spawn` (`src/supervisor/child.cpp`) builds **argv entirely before
fork**, then: socketpair → fork → in the child, only async-signal-safe
calls (`close`, `dup2`, `fcntl`, `execve`, `_exit`) because the parent
hosts a live Elio runtime and anything else is UB. The child argv is:

```
<device_bin> --config <config.json> [--global <overlaybd.json>] \
             --control-fd 3 [--dev-id <N>]
```

`--global` is omitted when empty; `--dev-id` is omitted when negative.
There is no PATH lookup — the supervisor resolves the binary itself. On
`execve` failure the child `_exit(127)`s; the parent observes EOF on the
channel plus exit code 127 and reports "exec failed (obd-device not found
or not executable)" (`Child::note_reaped`).

### Daemon model

`run_daemon` runs three concurrent activities on the Elio scheduler:
the **accept loop** (one coroutine per one-shot client), the **reaper** (a
`signalfd` on SIGCHLD draining `waitpid(-1, …, WNOHANG)` and matching pids
to children), and one **monitor coroutine per child** (a `LineReader` over
the child's status fd feeding `Child::update_status`). The children
registry (`std::map<id, std::shared_ptr<Child>>`) is guarded by an
`elio::sync::mutex` — coroutine-aware, never held across heavy work.
Signals must be blocked process-wide by the caller (signalfd model).

Bounded waits: `create` waits up to `ready_timeout_sec` (default 60 s) for
the child's first terminal-for-creation state (`ready`/`failed`/`exited`)
via `Child::ready_event`; on timeout or failure the child is SIGTERMed and
an error reply returned. `destroy` SIGTERMs, waits up to
`stop_timeout_sec` (default 10 s) for `exit_event`, escalates to SIGKILL
with a further 2 s grace, then erases the child from the registry.
Shutdown (SIGTERM/SIGINT to the daemon) stops accepting, SIGTERMs every
child, and waits up to `stop_timeout_sec` per child; stragglers are
SIGKILLed+reaped by `~Child`.

## Public API

### `src/supervisor/protocol.hpp`

- `inline constexpr size_t kMaxMessageBytes = 64 * 1024` — maximum JSON-line
  length on both channels.
- `inline constexpr int kProtocolVersion = 1` — control-protocol revision;
  increments only for additive batches (see the additive-only rule above).
- `inline constexpr std::string_view kProjectVersion` — the project version
  string, wired from CMake `project(... VERSION ...)` via the
  `OBD_VERSION_STRING` compile definition so it cannot drift; `"dev"` is
  the fallback for non-CMake builds.
- `struct CreateCommand` — `id`, `config` (required), `global` (`""` =
  supervisor default), `device_bin` (`""` = supervisor default), `dev_id`
  (`-1` = auto). Descriptive mirror of the create command's fields.
- `struct IdCommand` — `cmd`, `id`; shape of `destroy` / `status`.
- `std::optional<nlohmann::json> parse_command(std::string_view line,
  std::string& error)` — parses and validates one command line; `nullopt`
  with a human-readable `error` on malformed JSON, missing/extra-invalid
  fields, or unknown `cmd`.
- `std::string reply_ok(const nlohmann::json& fields = {})` — the fields
  object (non-object input is replaced by `{}`) plus `"ok":true`, dumped
  with a trailing newline.
- `std::string reply_error(const std::string& error)` —
  `{"ok":false,"error":...}` plus newline.
- `std::string reply_hello()` — the `hello` handshake reply via the
  `reply_ok` envelope: `protocol` (`kProtocolVersion`), `version`
  (`kProjectVersion`), and `features` (an initially empty JSON array of
  strings, the capability extension point).
- `struct DeviceStatus` — `state` (`starting` | `ready` | `failed` |
  `stopped`), `device` (`/dev/ublkb<N>` when ready), `error` (when failed).
- `std::optional<DeviceStatus> parse_device_status(std::string_view)` —
  `nullopt` unless the line is a JSON object with a string `state`;
  `device`/`error` default to `""`.
- `std::string make_device_status(const DeviceStatus&)` — the obd-device
  side encoder; omits empty `device`/`error`. Round-trips with
  `parse_device_status`.

### `src/supervisor/child.hpp`

- `struct ChildSpec` — `id`, `device_bin` (absolute path), `config_path`,
  `global_path` (may be empty), `dev_id_request` (`-1` = auto).
- `static std::unique_ptr<Child> Child::spawn(const ChildSpec&)` — forks
  and execs per the spawning contract above; the parent end of the channel
  is set `O_NONBLOCK`. Throws `obd::error` on socketpair/fork failure.
- `~Child()` — closes the control fd if held; if the process is still
  tracked: best-effort `SIGKILL` + blocking `waitpid`. Never throws.
- `const std::string& id() const`, `pid_t pid() const`.
- `int control_fd() const` — parent end of the status channel.
- `int release_control_fd() noexcept` — detaches the fd (the monitor
  coroutine takes ownership; `~Child` then won't close it).
- `struct Child::Status` — `state` (starts as `"starting"`; supervisor-side
  vocabulary adds `"exited"`), `device`, `error`, `exit_code` (`-1` until
  reaped).
- `Status status() const` — mutex-guarded snapshot.
- `void update_status(const Status&)` — publishes a status; fires
  `ready_event` the first time the state becomes `ready` / `failed` /
  `exited`, and `exit_event` on `exited`. Each event fires at most once.
- `elio::sync::event& ready_event()`, `elio::sync::event& exit_event()`.
- `void terminate() noexcept` — SIGTERM. `void kill() noexcept` — SIGKILL.
  Both are no-ops on an untracked pid.
- `void note_reaped(int wait_status)` — records a `waitpid` result:
  `exit_code` = `WEXITSTATUS`, or `128 + WTERMSIG` when signaled; state
  becomes `"exited"` (a clean `stopped`/`ready` history is deliberately not
  preserved — the state is bookkeeping, the detail lives in `exit_code`);
  exit code 127 with no prior error gets the "exec failed" message.

### `src/supervisor/daemon.hpp`

- `struct DaemonConfig` — `socket_path` (default
  `/run/overlaybd-elio/supervisor.sock`), `global_config` (default
  `/etc/overlaybd-elio/overlaybd.json`; handed to children whose create
  command has no `global`), `device_bin` (empty = auto-resolved as the
  sibling `obd-device` of `/proc/self/exe`; falls back to the literal
  `"obd-device"` if the readlink fails), `ready_timeout_sec` (60),
  `stop_timeout_sec` (10).
- `elio::coro::task<int> run_daemon(const DaemonConfig&)` — binds the
  socket, runs accept loop + reaper + monitors until SIGTERM/SIGINT, then
  shuts down gracefully (children terminated first). Returns the process
  exit code (1 if the socket cannot be bound, 0 otherwise). Caller must
  have blocked the handled signals process-wide (signalfd model).

Command handlers are internal to `src/supervisor/daemon.cpp` but define the
observable semantics: `create` rejects empty ids, ids containing `/`,
missing config files, missing device binaries, and duplicate ids;
`destroy`/`status` reject unknown ids with `{"ok":false,"error":"no such
device: <id>"}`.

## Invariants & Guarantees

- **Isolation**: every device is a separate process; a child crash, hang,
  or exec failure is detected (EOF on fd 3 and/or SIGCHLD), recorded, and
  reported — siblings and the daemon are unaffected (ADR-0004).
- **Reaping completeness**: every spawned child is reaped exactly once —
  by the SIGCHLD reaper during normal operation, by `~Child` (SIGKILL +
  blocking `waitpid`) on teardown paths. No zombies survive the daemon.
- **One-shot control channel**: one command line in, one reply line out,
  connection closed. Malformed commands always receive a `{"ok":false,...}`
  reply — the socket never hangs silently on bad input.
- **Bounded waits**: `create` never blocks longer than `ready_timeout_sec`
  waiting for readiness; `destroy` never blocks longer than
  `stop_timeout_sec` + 2 s SIGKILL grace.
- **Line discipline**: both channels cap a line at 64 KiB; a peer that
  exceeds the cap is treated as disconnected (`LineReader` returns EOF).
  Anchor: `src/supervisor/protocol.hpp::kMaxMessageBytes`.
- **Fork safety**: between fork and execve the child executes only
  async-signal-safe calls. Anchor: `src/supervisor/child.cpp::Child::spawn`.
- **Status monotonicity**: `ready_event` fires at most once, on the first
  creation-terminal state; `exit_event` fires at most once, on `exited`.
  Anchor: `src/supervisor/child.cpp::Child::update_status`.
- **Exit-code convention**: 127 means exec failure and is translated into a
  human-readable error; signaled exits are reported as `128 + signo`.
  Anchor: `src/supervisor/child.cpp::Child::note_reaped`.

## Concurrency & Call Permissions

- `run_daemon` and all command handlers are **Elio coroutines** — they must
  run on the Elio scheduler. The registry mutex is `elio::sync::mutex`
  (coroutine-aware); it is never held across child IO.
- `Child::spawn` is a **plain blocking function** containing `fork`; call
  it from the daemon coroutine (as `cmd_create` does) but treat it as a
  cold path. After spawn, all interaction with the child is coroutine-based
  (monitor reads, event waits) except `terminate`/`kill`, which are
  signal-only and safe from any thread.
- `Child`'s mutable state (`st_`, event-fired flags) is protected by a
  plain `std::mutex`; `status()` / `update_status()` / `note_reaped()` are
  safe to call from the monitor coroutine, the reaper coroutine, and
  command handlers concurrently. Event publication happens outside the
  mutex.
- The control fd passes ownership exactly once via `release_control_fd()`
  (to the monitor coroutine, which closes it at EOF); double-close is
  impossible by construction.
- The reaper is the only `waitpid(-1, …)` caller in the process; it
  matches pids against the registry and ignores unknown ones.
- Inputs are not mutated: `ChildSpec` and `DaemonConfig` are read at
  spawn/run time; `parse_command` returns an owning `nlohmann::json`.

## Stability Contract

- **Both wire protocols are T1 wire contracts** (AGENTS.md): the control
  channel's command/reply shapes (`cmd`, `id`, `config`, `global`,
  `device_bin`, `dev_id`; `ok`/`error` envelope; the `hello` handshake
  fields `protocol`/`version`/`features`; per-command success
  fields) and the status channel's shapes (`state` plus optional `device`/
  `error`; the state vocabulary `starting`/`ready`/`failed`/`stopped`) may
  only change with an ADR — and then only **additively** (see the
  additive-only evolution rule above; ADR-0014, proposed). obdctl,
  obd-supervisor, and obd-device may be upgraded independently.
- **The fd-3 + argv contract** between supervisor and obd-device
  (`--control-fd 3`, `--config`, optional `--global`, optional `--dev-id`,
  CLOEXEC-cleared fd 3) is part of the same wire contract.
- **Exit-code semantics** (127 = exec failure, 128+sig = killed) are part
  of the status surface consumed by operators and tooling.
- **Default paths** (`/run/overlaybd-elio/supervisor.sock`,
  `/etc/overlaybd-elio/overlaybd.json`, sibling-of-exe `obd-device`) are
  operational contract; changing defaults is breaking.
- **Timeout defaults** (60 s ready, 10 s stop) are behavioral contract;
  the fields exist to override them without a protocol change.
- Internal and free to change: `LineReader` buffering, registry container
  choice, monitor/reaper coroutine structure.

## Testing

Unit tests live in `tests/unit/test_supervisor.cpp`; they exercise the
protocol codecs and the child lifecycle with fake device binaries, without
needing a real ublk device or root.

- `supervisor: protocol commands parse and reject garbage` — guards the
  command validator and the status codec: a well-formed `create`, `list`,
  and `status` parse; a `create` missing `config`, an unknown `cmd`, and
  non-JSON input are rejected; a `ready` status line with a `device` field
  parses; `{}` is rejected; and `make_device_status` → `parse_device_status`
  round-trips a `failed` status with its `error` field. This pins the exact
  wire shapes documented above.
- `supervisor: hello handshake replies with protocol version and features` —
  pins the documented `hello` reply shape (`ok`, integer `protocol` ≥ 1,
  non-empty string `version`, array `features`), that `hello` parses with
  no required fields and ignores extras, and that unknown cmds and
  malformed JSON are still rejected with an answerable reason.
- `supervisor: child spawn execs and reports through the channel` — spawns
  a fake `obd-device` (a `/bin/sh` script that echoes a `ready` JSON line
  to **fd 3**) and guards: the child is exec'd, the parent receives the
  line on the control fd, it parses as `ready` with the right `device`,
  and `note_reaped` transitions the child to `exited` with `exit_code` 0.
  This pins the fd-3 spawning contract end to end.
- `supervisor: exec failure surfaces as exit 127` — spawns a child whose
  binary does not exist and guards that the reaped `exit_code` is exactly
  127 and the status error contains "exec failed". This pins the exec-fail
  convention that operators and `cmd_create` rely on.

Run: `ctest --test-dir build --output-on-failure` (no privileges needed;
the spawn tests create their fake binaries under a temporary directory).

### Crash recovery (ADR-0010)

Each device is supervised for its whole lifetime by one coroutine
(`supervise_entry`) that reads the child's status channel and, on an
**unexpected** exit (not a requested `destroy`, not daemon shutdown),
respawns the child with `--recover --dev-id N`: the ublk device was
created with `UBLK_F_USER_RECOVERY` and survives serverless in the kernel,
so the replacement *attaches* instead of re-creating. The dev id is parsed
from the ready status's bdev path by the supervising coroutine itself, so
an instant crash can never be observed before the id is recorded.
Respawns are bounded by `DaemonConfig::max_recovery_attempts` (default 3);
`list`/`status` report the `recoveries` count. Intentional destroys set a
flag first and never respawn.

### Daemon shutdown

`run()` wakes and joins its detached tasks before returning: the accept
loop (a dummy connection, because `close()` does not cancel an in-flight
accept SQE) and the reaper (a synthetic SIGCHLD, because a parked signalfd
wait has no cancel path). The reaper constructs its `signal_fd` itself and
is pinned with `go_to(0)`: `signal_fd` caches the creating worker's
`io_context`, and `sync::mutex` wakeups could otherwise migrate the
coroutine to a worker where that context is invalid.

## Limitations & TODO

- **No recovery backoff**: respawn is immediate with a fixed bound
  (ADR-0010); exponential backoff / flapping detection can be revisited if
  operations show the need.
- **No output capture**: child stdout/stderr are inherited, not piped
  through the supervisor; log aggregation is the operator's job.
- **One command per connection**: clients needing many operations pay a
  connect per command; acceptable for a control plane, worth revisiting if
  tooling starts polling `status` at high frequency.
- **Unknown reaped pids are ignored**: the reaper only matches registered
  children; direct grandchildren double-forked by a device process are not
  tracked (device processes are expected to stay single).
- **No id reuse guard after destroy**: a destroyed id can be immediately
  re-created; clients that cache `device` paths must re-read `status`.
- **Socket permissions**: the control socket inherits the daemon's umask;
  a dedicated ownership/ACL story (root-only vs. group access) is
  deployment policy, documented in `docs/operations.md`.
