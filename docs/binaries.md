# Binaries: obd-supervisor, obd-device, obdctl, obd-mkimage

## Overview

The repository builds four executables:

- **obd-supervisor** — the per-node daemon. It owns the control plane: a
  Unix domain socket accepting JSON-lines commands, and one isolated child
  process per block device (ADR-0004). A device crash never takes down its
  siblings or the daemon.
- **obd-device** — the per-device server. Spawned by obd-supervisor (once per
  `create` request), it assembles one OverlayBD image from a per-image
  `config.json` and serves it as one ublk block device (`/dev/ublkb<N>`).
  It is not meant to be run by hand, but it can be, for debugging.
- **obdctl** — the control CLI. One invocation sends one JSON-lines command
  to the supervisor over its UDS and pretty-prints the reply. It uses plain
  blocking IO; the Elio runtime is not involved.
- **obd-mkimage** — a test-image generator. It converts a raw disk image
  into a sealed single-layer LSMT file, optionally ZFile-compressed, and
  prints a ready-to-use `lowers[]` config snippet on stdout. It exists to
  produce fixtures for tests and local bring-up, not for production image
  builds.

Interactions: `obdctl → obd-supervisor` over the supervisor UDS (default
`/run/overlaybd-elio/supervisor.sock`); `obd-supervisor → obd-device` by
fork/exec per `create` request, with a socketpair as the lifecycle status
channel; `obd-device → kernel` over ublk. The wire protocols are specified
in `src/supervisor/protocol.hpp` and documented in
[supervisor.md](./supervisor.md); configuration files are documented in
[config.md](./config.md). Operational procedures live in
[operations.md](./operations.md).

## Usage

### obd-supervisor

```
obd-supervisor [--socket PATH] [--global PATH] [--device-bin PATH]
               [--ready-timeout SEC] [--stop-timeout SEC]
```

Runs in the foreground until SIGTERM/SIGINT. Options (defaults from
`src/supervisor/daemon.hpp::DaemonConfig`):

| Option | Default | Meaning |
|---|---|---|
| `--socket PATH` | `/run/overlaybd-elio/supervisor.sock` | Control UDS path listened on for obdctl commands. |
| `--global PATH` | `/etc/overlaybd-elio/overlaybd.json` | Global config handed to children when a `create` command does not carry its own `--global`. |
| `--device-bin PATH` | empty = sibling of the supervisor executable | obd-device binary to exec for each device. |
| `--ready-timeout SEC` | `60` | How long `create` waits for the child's `ready` status before failing the request. |
| `--stop-timeout SEC` | `10` | `destroy` grace period: SIGTERM first, SIGKILL after this many seconds. |
| `--help`, `-h` | — | Print usage and exit 0. |

### obd-device

```
obd-device --config PATH [--global PATH] [--control-fd N] [--dev-id N] [--recover]
```

| Option | Default | Meaning |
|---|---|---|
| `--config PATH` | required | Per-image `config.json` (overlaybd-snapshotter format). Missing → usage error, exit 2. |
| `--global PATH` | empty = built-in defaults | Global `overlaybd.json`; when omitted, a default-constructed `GlobalConfig` is used. |
| `--control-fd N` | `-1` (no reporting) | Inherited fd for JSON-lines lifecycle status reports (the socketpair end installed by the supervisor). |
| `--dev-id N` | `-1` (auto-assign) | Requested ublk device id; the kernel picks a free id when negative. |
| `--recover` | off | ADR-0010: attach to the existing `--dev-id` device via `START_USER_RECOVERY` instead of creating a new one (requires `--dev-id`). Used by the supervisor when respawning a crashed device. |
| `--help`, `-h` | — | Print usage and exit 0. |

Requires a build with `OBD_ENABLE_UBLK=ON` (the only supported block-device
backend; the source does not compile otherwise).

### obdctl

```
obdctl [--socket PATH] hello
obdctl [--socket PATH] create <id> <config.json> [--global PATH] [--dev-id N]
obdctl [--socket PATH] destroy <id>
obdctl [--socket PATH] list
obdctl [--socket PATH] status <id>
```

`--socket` defaults to `/run/overlaybd-elio/supervisor.sock` and, when
present, must precede the command word. Commands:

- `hello` — the protocol handshake: the reply carries the control-protocol
  revision (`protocol`), the supervisor's version string (`version`), and
  the capability list (`features`); see [supervisor.md](./supervisor.md)
  for the additive-only evolution rule.
- `create <id> <config.json>` — ask the supervisor to spawn an obd-device
  child serving that image config. Optional `--global PATH` overrides the
  supervisor's default global config for this device; optional `--dev-id N`
  requests a specific ublk id. The reply blocks until the child reports
  `ready` (carrying the `/dev/ublkb<N>` path) or fails/times out.
- `destroy <id>` — stop the child (SIGTERM, then SIGKILL after the
  supervisor's stop timeout) and remove the device.
- `list` — list known devices and their states.
- `status <id>` — query one device.

The supervisor's JSON reply is pretty-printed to stdout.

### obd-mkimage

```
obd-mkimage --input <raw.img> --out-dir <dir> [--name base]
            [--zfile] [--bs N] [--zstd [level]] [--no-verify]
```

| Option | Default | Meaning |
|---|---|---|
| `--input PATH` | required | Raw disk image to convert. |
| `--out-dir DIR` | required | Output directory (must exist). |
| `--name STR` | `layer` | Basename of the produced files. |
| `--zfile` | off | Also ZFile-compress the LSMT file into `<name>.zfile`; the snippet then points at the compressed blob. |
| `--bs N` | format default | ZFile block size (only meaningful with `--zfile`). |
| `--zstd [level]` | off | Use Zstd (OverlayBD algo 2) instead of the default compression; an optional level may follow. |
| `--no-verify` | verify on | Skip the read-back verification pass after writing. |

Produces `<out-dir>/<name>.lsmt` (a sealed single-layer LSMT) and, with
`--zfile`, `<out-dir>/<name>.zfile`. On success it prints a JSON snippet on
stdout:

```json
{
  "lowers": [
    { "digest": "sha256:<hex>", "size": <bytes>, "file": "<blob path>" }
  ],
  "repoBlobUrl": ""
}
```

which can be dropped into a per-image `config.json` for tests (see
[config.md](./config.md)).

## Behavior & guarantees

### Exit behavior

- All four binaries: exit `0` on success, `1` on runtime failure (message on
  stderr, and for obd-device additionally a `failed` status report), `2` on
  usage errors (unknown argument, missing option value, missing required
  option). obdctl exits `0` exactly when the supervisor's reply carries
  `"ok": true`, `1` otherwise (including connection failures and malformed
  replies).
- obd-device exit `0` means a *clean stop*: the device served, received
  SIGTERM/SIGINT, tore the ublk device down, and reported `stopped`. Exit
  `1` covers every setup or runtime failure, including an image whose
  virtual size is zero or not a multiple of 512 bytes (rejected up front as
  "virtual size not sector aligned").

### Signal handling

Both daemons use the signalfd model. Before the Elio scheduler starts,
obd-supervisor blocks SIGTERM, SIGINT and SIGCHLD, and obd-device blocks
SIGTERM and SIGINT, **on every thread** (failure to block → exit `1`
before any work begins). Signals are then consumed via
`elio::signal::signal_fd` inside coroutines — there are no signal handlers
running on arbitrary threads.

- obd-device: after reporting `ready`, it waits on the signal fd; the first
  SIGTERM or SIGINT breaks the loop, stops and destroys the ublk device,
  reports `stopped`, and returns exit code 0.
- obd-supervisor: SIGTERM/SIGINT triggers a graceful shutdown — children
  are terminated first (SIGTERM with the stop-timeout grace, then SIGKILL)
  before the daemon exits.

### Lifecycle status reporting

obd-device reports `starting` → `ready` (with the `/dev/ublkb<N>` path) →
`stopped`, or `failed` with an error string, as JSON-lines on the inherited
`--control-fd` (`src/supervisor/protocol.hpp::DeviceStatus`, built via
`src/supervisor/protocol.hpp::make_device_status`). Reports are best-effort
single short writes; the supervisor tolerates a lost report as EOF on the
channel and falls back to waitpid-derived state. Without `--control-fd`
(standalone runs) reporting is silently disabled.

### Supervisor control socket

The supervisor listens on a stream UDS (default
`/run/overlaybd-elio/supervisor.sock`), one JSON-lines command per
connection, one JSON-lines reply, max 64 KiB per message
(`src/supervisor/protocol.hpp::kMaxMessageBytes`). `create` spawns one
obd-device child per request — process isolation per device is the ADR-0004
guarantee. `destroy` sends SIGTERM and escalates to SIGKILL after
`--stop-timeout` seconds.

### Concurrency and stability notes

- obdctl and obd-mkimage are single-threaded, synchronous tools; they hold
  no state between invocations.
- The supervisor↔obdctl command protocol and the supervisor↔obd-device
  status protocol are wire contracts between binaries that may be upgraded
  independently; changes require an ADR (trigger T1, see
  [adr/README.md](./adr/README.md)).
- Command-line parsing is exact-token matching: `--socket=PATH` style
  combined forms are *not* accepted by any of the four binaries.

## Testing

- `supervisor: protocol commands parse and reject garbage` — pins the
  obdctl↔supervisor command grammar: valid `create`/`destroy`/`list`/
  `status` lines parse; malformed JSON, unknown commands and bad fields are
  rejected with an error.
- `supervisor: daemon answers hello and never drops bad input` — the
  `obdctl hello` path end to end: a real daemon answers the handshake with
  `protocol`/`version`/`features` and never drops malformed input.
- `supervisor: child spawn execs and reports through the channel` — guards
  the fork/exec path the supervisor uses to start obd-device and the
  JSON-lines status channel back.
- `supervisor: exec failure surfaces as exit 127` — guards that a missing
  or non-executable device binary is reported as a spawn failure (exit
  127), not mistaken for a device-level error.
- `integration: ublk device serves sector reads from a blob` — end-to-end
  obd-device behavior: a real ublk device serves reads assembled from a
  local blob. Requires privileges and `ublk_drv`; self-skips otherwise (by
  design — see [testing.md](./testing.md)).
- `integration: registry pipeline serves a zfile-compressed image` —
  exercises the full image-assembly path obd-device runs, over a mock
  registry with a ZFile-compressed layer.
- obd-mkimage's writers are pinned by round-trip tests:
  `format: lsmt round-trip and multi-layer merge semantics` (the LSMT
  single-layer writer output reads back and merges correctly) and
  `format: zfile round-trip reads back the original content` (the ZFile
  writer output decompresses byte-identically).

Run with:

```bash
cmake -S . -B build -DOBD_BUILD_TESTS=ON && cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

## Limitations & TODO

- Recovery respawns are bounded (`max_recovery_attempts`, default 3);
  beyond the bound the device stays down and re-creating it is the
  operator's job (`destroy` + `create`). See
  [operations.md](./operations.md) and ADR-0010.
- No combined `--opt=value` form and no short-option aliases (other than
  `-h`) on any binary.
- obdctl is one-shot: one command per invocation, no interactive or batch
  mode.
- obd-mkimage builds single-layer images only, on a synchronous cold path;
  it is a fixture generator, not a replacement for the upstream
  `overlaybd-*` image toolchain.
- obd-device supports exactly one image per process by design (ADR-0004);
  multi-device serving will not be added to it.
