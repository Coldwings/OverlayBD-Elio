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
 "dev_id":<int, optional, -1/absent = auto>,
 "virtual_size":<bytes, optional — D3 headroom override>}
{"cmd":"create","id":"<name>",
 "blank":{"size":<bytes>}}                          # mode 2: blank raw
{"cmd":"create","id":"<name>",
 "blank":{"size":<bytes>,"mkfs":"<type>"}}          # mode 3: + mkfs

{"cmd":"destroy","id":"<name>"}
{"cmd":"status","id":"<name>"}
{"cmd":"list"}
{"cmd":"commit","id":"<name>","user_tag":"<optional string>",
 "virtual_size":<bytes, optional — D3 re-baseline override>}

{"cmd":"trace_start","id":"<name>","path":"<absolute output file>",
 "duration_sec":<int 1..3600>}
{"cmd":"trace_stop","id":"<name>"}

{"cmd":"resize","id":"<name>","size":<bytes>}
```

Validation: the message must be a JSON object with a string `cmd`;
`create` requires a string `id` and exactly one of a string `config`
(image mode) or an object `blank` (blank raw mode, ADR-0014); a `blank`
object requires an integer `size` and an optional string `mkfs`.
`destroy`/`status`/`commit` require a string `id`; `commit`'s optional
`user_tag` must be a string; `trace_start` requires a string `id`, a
string `path`, and an integer `duration_sec`; `trace_stop` requires a
string `id`; `resize` requires a string `id` and a non-negative integer
`size`; `create` and `commit` accept optional non-negative integer
`virtual_size` fields; `hello` and `list` take no fields; anything else
is "unknown cmd". Field TYPES are validated at parse time (value-level
blank rules — positive, 512-aligned, within the sanity bound, safe
`mkfs` type — are enforced by `parse_blank_spec` when the handler
runs): a wrong-typed field is answered with a clean protocol error,
never an exception escaping the handler. Malformed input is answered,
not dropped.

Replies (`reply_ok` / `reply_error`): success is `{"ok":true,...}` with
command-specific fields merged in; failure is
`{"ok":false,"error":"<human-readable reason>"}`. The success shapes by
command (`src/supervisor/daemon.cpp`):

- **hello**: `{"ok":true,"protocol":<int>,"version":"<project version>",
  "features":[...]}` — the handshake. `protocol` is the control-protocol
  revision (`kProtocolVersion`, currently 4; 1 = initial command set,
  2 = added `commit`, 3 = added `trace_start`/`trace_stop` and the
  additive `trace` status field, 4 = added `resize` and the
  `virtual_size` create/commit fields (D3) and the `blank` create mode
  (ADR-0014)); `version` is the project version string wired from CMake
  (`kProjectVersion`); `features` is a JSON array of strings — the
  capability gate for optional commands, currently
  `["commit","trace","resize","blank"]`.
- **create**: `{"ok":true,"id","pid","device"}` — `device` is the child's
  reported `/dev/ublkb<N>`. With an optional `virtual_size` (bytes > 0)
  the device is created with that capacity instead of the image's
  declared size (D3 headroom, "Online resize" below). A blank create's
  reply adds `"mode":"blank"`, `"size":<bytes>`, and — only when the
  request carried an `mkfs` — `"mkfs":"<type>"` (the mode-3 format ran
  on the new device before the reply).
- **destroy**: `{"ok":true,"id"}`.
- **list**: `{"ok":true,"devices":[{"id","pid","state","device","error"}, ...]}`.
- **status**: `{"ok":true,"id","pid","state","device","error","exit_code"}`
  (`exit_code` is -1 until the child is reaped). Both `status` and
  `list` replies carry the additive **`trace`** object once a recording
  was started on the device (see "Trace recording" below).
- **commit** (ADR-0014): `{"ok":true,"id","path","sha256","size"}`
  — the sealed upper's file path, the hex sha256 of the sealed file, and
  its byte size. With an optional `virtual_size` the sealed layer's
  declared size is re-baselined to that value (D3, "Offline commit"
  below).
- **trace_start** (ADR-0013): `{"ok":true,"id","path","duration_sec"}` —
  the device is recording. See "Trace recording" below.
- **trace_stop** (ADR-0013): `{"ok":true,"id","path","sha256","size",
  "records","dropped"}` — the finalized trace blob's path, hex sha256,
  byte size, written record count, and dropped-record count.
- **resize** (D3): `{"ok":true,"id","size"}` — `size` is the device's
  new capacity in bytes. See "Online resize" below.

### Additive-only evolution rule

The control protocol evolves **additively only** (governing decision:
ADR-0014). Concretely:

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

### Offline commit (ADR-0014)

`commit` seals a device's writable LSMT-RW upper into a standard sealed
LSMT layer **offline** — the device process is never alive while its upper
is being sealed. Contract:

- **Stopped-device contract.** If the device is live (any state other than
  `exited`), commit stops it first: SIGTERM plus a bounded reap
  (`stop_timeout_sec`, escalating to SIGKILL with a 2 s grace) — the
  destroy-path mechanics *without* removing the entry from the registry,
  so `status` keeps working afterwards. Only when the child is reaped does
  sealing begin. A device that will not stop is answered with an error and
  its upper is never touched. Rationale for stopping rather than refusing:
  the protocol has no separate stop command, so a refuse-when-live
  contract would make commit unreachable without a destroy (which drops
  the entry); ADR-0014 explicitly allows "requires the device stopped (or
  stops it)".
- **How the supervisor finds the upper.** The device process owns the
  upper's files; the supervisor records the upper path and kind
  (`<upper.dir>/overlaybd.rw`, docs/config.md) **from the image config at
  create time** — provenance. A later edit of the config file does not
  redirect commit; if the config could not be parsed at create, commit
  reports "image config unreadable at create; upper unknown". The
  device-status protocol is unchanged.
- **The shutdown checkpoint.** An unsealed LSMT-RW file's segment index is
  memory-only (docs/format.md); a graceful obd-device shutdown therefore
  **checkpoints** the index into the file (unsealed trailer) before
  exiting, which is what the supervisor's offline seal consumes. A device
  that crashed or was SIGKILLed has no checkpoint and its upper is lost —
  commit then fails with a precise error.
- **Sealing** runs in the supervisor process via
  `src/format/lsmt_rw.hpp::LsmtRwLayer::seal_file` over the async IO
  backend (no blocking work on the Elio workers). The sealed uuid is
  content-derived — identical upper content seals to identical bytes
  (docs/format.md, seal determinism invariant).
- **Concurrency.** One commit per device at a time: a second commit of
  the same device while one is in flight is rejected ("commit already in
  progress"). The stop-and-seal critical section is serialized against a
  crash-recovery respawn (ADR-0010) by a per-entry mutex: either the
  respawn completes first and commit stops the recovery child too, or the
  respawn aborts on the `destroying` flag — a seal never runs while a
  device child is booting or alive.
- **Re-baseline (D3).** An optional `virtual_size` (bytes, 0 = keep the
  checkpointed size) overrides the virtual size written into the sealed
  header/trailer — and therefore the size a next `create` from this
  layer derives — so a commit can declare the larger device. Validated
  in the seal path (grow-only): a positive multiple of 512, and at
  least both the layer's declared size and its content extent; a
  smaller override is rejected with a precise reason BEFORE any
  compaction, so the upper stays committable at its declared size.
- **Errors** (via the `{"ok":false,"error"}` envelope, precise reasons):
  unknown id ("no such device"), a config without `upper` ("no writable
  upper"), a sparse upper ("sparse uppers cannot be sealed" — upstream
  parity, ADR-0014), a concurrent commit ("commit already in progress"),
  a config unreadable at create time, a missing upper file, an
  already-sealed upper, a missing or invalid shutdown checkpoint (device
  crashed), a rejected `virtual_size` re-baseline (grow-only or
  misaligned), and stop-timeout.

### Online resize (D3, ADR-0014 dev_size model)

`resize` grows a live device: the supervisor forwards the byte count to
the device process, whose command-loop executor (the same control
channel trace commands ride) enforces the **grow-only** rule and — for a
WRITABLE device — first grows the DATA PLANE (the merged view and its
writable top; `MergedWritable::grow`, see docs/format.md) so writes into
the headroom land in the upper, then issues the ublk
`UBLK_U_CMD_UPDATE_SIZE` (docs/ublk.md) through `elio::spawn_blocking`
(a kernel control call may sleep on our own data plane — never run on an
Elio worker). Wire shapes:

- obdctl → supervisor: `{"cmd":"resize","id":"<name>","size":<bytes>}`.
  `size` must be a positive multiple of 512 (validated supervisor-side
  before forwarding — ublk sector granularity).
- supervisor → device (channel 2): `{"cmd":"resize","size":...,"seq":N}`,
  using the same bounded (30 s) forward-and-await with `seq`
  correlation as `trace_start`/`trace_stop`.
- device → supervisor: `{"reply":"resize","ok":true,"size":<new bytes>,
  "seq":N}` or `{"reply":"resize","ok":false,"error":"...","seq":N}`.
- obdctl ← supervisor: the device reply fields plus `id`:
  `{"ok":true,"id","size"}`.

Contract:

- **Grow-only.** A request at or below the device's current size (a
  no-op or a shrink) is rejected by the DEVICE with
  `ok:false` + a "grow-only" message BEFORE any kernel IO — the current
  size is known only there. Shrink is rejected cleanly at every layer
  that can compare sizes: the resize executor, the writable layers'
  `grow()` (-EINVAL below current; equal is an idempotent no-op, so a
  retried grow after a partial kernel failure still succeeds),
  `create --virtual-size` against the assembled image size, and
  `commit --virtual-size` in the seal path.
- **The data plane grows with the device (writable).** A resize of a
  writable device grows the merged view AND its writable top to the new
  size before the kernel command runs, so the guest can write into the
  headroom and the data lands in the upper. For LsmtRwLayer the grow
  rewrites the file's on-disk declared-size header (uuid preserved), so
  a later graceful-shutdown checkpoint and a plain `commit` are
  consistent at the grown size — the growth is DURABLE at commit time
  (no `--virtual-size` needed; `commit --virtual-size` remains the
  explicit re-baseline for layers whose header was not grown). A
  read-only image has no data plane to grow: its headroom is
  dev-size-only (reads past the image's end are zero-filled by the
  bridge).
- **Recovery (ADR-0010).** A grown device that crashes keeps its kernel
  capacity across USER_RECOVERY (the driver never resets it, and the
  replacement re-attaches without SET_PARAMS/UPDATE_SIZE). The
  replacement obd-device therefore seeds its grow-only baseline from the
  KERNEL's real capacity (`Ctrl::get_params`, not the create-time
  params) so a post-recovery resize can never silently shrink the
  gendisk; an unsealed LSMT-RW upper is truncated on recovery
  (ADR-0008), so the fresh upper and data plane start at the image's
  declared size — resize (grow) is how the operator restores the larger
  window.
- **Headroom at create.** `create --virtual-size <bytes>` sizes the
  device to the override (sanctioned headroom); for a WRITABLE image
  the writable upper — and hence the merged data plane — is assembled
  at the override too (`open_image`'s override parameter); for a
  read-only image it is dev-size-only. The default remains the image's
  declared size. Grow-only vs the assembled image size is validated
  with the single rule `image::device_capacity_bytes` (inside
  `open_image` for writable images, in obd-device otherwise), so a
  create that would shrink the device below its content fails cleanly.
- **Resize vs commit (no daemon-side lock; documented).** `resize` is
  NOT serialized against `commit` in the daemon: `commit` stops the
  device over signals and the entry's `op_mu`, while `resize` rides the
  device command channel. Every ordering is nevertheless safe because
  the DEVICE arbitrates: a grow that completes before the device's
  graceful shutdown is followed by a checkpoint at the grown size
  (header and trailer agree, so commit seals the grown declared size),
  and from the moment the shutdown begins the device rejects resizes
  with `ok:false` "device is shutting down; resize ignored" — so a
  header rewrite can never land after the checkpoint wrote its trailer
  (which would make `open_checkpointed` reject the pair and leave the
  upper uncommittable). The device also DRAINS: the resize executor
  holds a gate for the whole grow, and the shutdown path sets the
  stopping flag and then takes/releases that gate once — waiting out a
  grow that was already in flight — before it stops the device and
  checkpoints, so no grow can interleave with the checkpoint at all. A
  resize racing a stop may instead see the channel close ("device
  control channel unavailable"/timeout), which is also a clean error;
  the CLI retries after the commit.
- **Errors**: unknown id, no live device control channel (a stopped/
  dead device: "device control channel unavailable"), a device whose
  executor has no resize seam ("resize unsupported on this device"),
  misaligned/zero size, a grow-only rejection, a failed data-plane
  grow, and a kernel rejection of `UBLK_U_CMD_UPDATE_SIZE` (drivers
  without the command — added in the 6.16 cycle — answer
  `ENOTSUPP`/524 on pre-6.15 kernels and `EOPNOTSUPP`/95 on 6.15+;
  surfaced as the command's error. The data plane is already grown and
  the retry succeeds once the kernel accepts it).

### Blank (raw) device creation (ADR-0014)

`create` with the additive `blank` object makes a **blank raw device**
instead of opening an image: no `config`, no lowers, no registry. The
three creation modes of ADR-0014 are:

1. **From an image** — `create` with `config`, as above.
2. **Blank raw disk, mandatory size** — `blank:{"size":<bytes>}`: the
   device serves a **zeroed** block device of exactly `size` bytes with a
   writable upper from birth. Reads of never-written ranges return zeroes
   through the ordinary LSMT merge path; writes land in the LSMT-RW upper
   and `commit` seals it like any other upper. The zero base beneath the
   upper is a **sealed empty LSMT layer** (no segments, virtual size =
   `size`; byte-deterministic, see docs/format.md), assembled by
   `obd::image::open_blank_device` inside a per-device workspace
   `<blank_dir>/<id>/` (`overlaybd.zero` + `overlaybd.rw`).
3. **Blank + mkfs convenience** — `blank:{"size":<bytes>,"mkfs":"<type>"}`:
   mode 2 plus a host `mkfs.<type>` run on the new block device by the
   **supervisor** (it owns the device lifecycle and the reported
   `/dev/ublkb<N>` path) before `create` replies. **Runtime convenience
   ONLY**: host mkfs output is non-deterministic (UUIDs, hash seeds,
   timestamps) and must never be used as an image-build input — see
   docs/operations.md. The daemon runs `mkfs.<type>` only when the create
   explicitly asked for it; a failing mkfs answers with a clean error and
   removes the freshly created device entry. The boundary is enforced:
   a mode-3 device is marked with the create-time INTENT (before the
   entry is published and before mkfs runs), and `commit` refuses to seal
   its upper ("host mkfs ... cannot be sealed") in every ordering —
   including a commit that lands while the mkfs step is still running and
   the device is already reachable. The supervisor never turns its own
   non-deterministic convenience output into an image layer.

Two workspace caveats for blank devices (both inherited from the
writable-upper model, documented in docs/operations.md):

- **Respawn truncates the upper.** ADR-0010 crash recovery re-assembles
  the blank stack, and assembly recreates `overlaybd.rw` with `O_TRUNC`:
  unsealed writes do not survive a device crash. Commit (or a graceful
  stop) first.
- **The committed artifact lives in the workspace.** `commit` seals
  `<blank_dir>/<id>/overlaybd.rw` in place; `destroy` + re-create of the
  same id truncates it. Copy the sealed layer out first.

Size rules (enforced at parse/handler time): positive, multiple of 512
bytes, at most `kMaxBlankSizeBytes` (16 TiB sanity bound — the zero base
allocates no data, so this only guards a typo'd size). The `mkfs` type is
validated to a safe `mkfs.<type>` suffix charset (`valid_mkfs_type`:
`[a-z0-9_]`, 1..16 chars), the injection boundary for the fork/exec
runner.

The mode-3 runner is bounded twice over: it polls its helper with
`waitpid(pid, …, WNOHANG)` for at most `mkfs_timeout_sec`, then SIGKILLs
it and polls for at most ~2 s more — a create never parks on a helper
wedged in uninterruptible IO. If the helper is still unreaped by then, a
detached blocking `waitpid` finishes the job, so it cannot outlive the
daemon as a zombie (the daemon's SIGCHLD reaper deliberately sweeps only
its own registered device pids).

### Trace recording (ADR-0013)

`trace_start` / `trace_stop` control the record path of ADR-0013: the
device process records the layer-blob access pattern of its remote reads
into an upstream-compatible prefetch trace blob (docs/trace-format.md;
the tap and recorder live in `src/image/trace_record.hpp`, the access
surface is `OpenedImage::recorder`). Unlike `commit`, these commands are
**device-executed**: the supervisor only forwards them to the live
device over channel 2 and relays the reply. Contract:

- **Forwarding.** The supervisor sends
  `{"cmd":"trace_start","path":...,"duration_sec":N,"seq":N}` /
  `{"cmd":"trace_stop","seq":N}` to the device and waits (bounded, 30 s)
  for the device's `{"reply":"trace_start"|"trace_stop", ...}` line.
  `seq` is a per-command correlation token, fresh for every forwarded
  command and echoed by the device in its reply (additive: older
  supervisors omit it, older devices do not echo); a reply whose `seq`
  does not match the pending command is dropped and logged, so a LATE
  reply to a timed-out command can never complete the next command with
  the wrong fields. One device command is outstanding per device at a
  time; a wedged or ancient device answers as "device control channel
  timeout". A dead device (channel EOF) fails a pending command
  immediately.
- **Server-side duration bound.** The duration timer lives in the
  DEVICE process: expiry finalizes the recording exactly like an
  explicit stop, so a dead, crashed, or disconnected CLI can never leak
  a recording device. `duration_sec` is bounded to 1..3600 s (the
  device rejects out-of-bounds values).
- **Finalize = the conforming-writer contract.** Stop, expiry, and
  device shutdown all finalize identically: queued records drain into
  the codec's `format::trace::TraceWriter` (24×N framing, raw-chaining
  CRC-32C, ≤ 1 MiB counts, zero padding) whose `finalize()` rewrites the
  header checksum; the blob is written and fsynced; the reply reports
  `{path, sha256, size, records, dropped}`.
- **Expiry surface.** After a duration expiry the device emits an
  unsolicited `{"reply":"trace_event","event":"expired", ...stats...}`
  line; the supervisor records it and both `status` and `list` replies
  carry the additive **`trace`** object from then on:
  `{"state":"recording"|"stopped"|"lost","path","duration_sec"?,
  "reason":"stopped"|"expired"|"device_exit","sha256"?,"size"?,
  "records"?,"dropped"?}`. Expiry events apply ONLY while the entry's
  trace state is "recording" — a stale expiry landing after a new
  recording started is logged and ignored, never overwriting the fresh
  recording's status. **Crash mid-record** (device exit with a
  recording open) marks the trace `"state":"lost",
  "reason":"device_exit"`: queued records are memory-only and die with
  the device, and the mark — not a stale "recording" — is what a
  recovery respawn starts from.
- **Idempotent stop.** A `trace_stop` that races (or follows) an expiry
  returns the same finalized stats, never an error — a CLI can always
  learn the outcome of its recording. A stop with no recording ever
  started is the error "no trace recording in progress".
- **Errors** (precise reasons, via the error envelope): unknown id,
  missing/wrong-typed fields (protocol parse), a concurrent device
  command ("another device command is in flight"), an unavailable
  channel ("device control channel unavailable"), a double start
  ("trace recording already in progress"), an out-of-bounds or
  non-absolute path/duration (device-rejected), a wedged device
  ("device control channel timeout").
- **Recording captures REMOTE reads only.** Records fire per
  fully-satisfied pread on a lower's remote source — LayerStore local
  hits produce no record. Structural warm-up, trace replay, and
  background-fill traffic ARE remote reads and are recorded when they
  fall inside the window (the runbook records with prefetch/download
  off; docs/operations.md).

### Channel 2: obd-device → supervisor (status channel, fd 3)

A `socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC)` created before fork.
The child end is dup2'd to **fd 3**, cleared of CLOEXEC, and survives
`execve`; the device binary receives `--control-fd 3` on its argv. Same
JSON-lines framing, same 64 KiB cap. Device → supervisor traffic is
lifecycle **status lines** plus, since protocol 3, `reply`-discriminated
command replies/events (see "Trace recording" above; the supervisor
routes on the `reply` field and treats everything else as a status
line). Supervisor → device traffic is **signals** (SIGTERM = shutdown
request, SIGKILL = force) plus, since protocol 3, JSON-lines trace
commands on the same socketpair. Devices that predate trace control
never read the channel, so a new-supervisor/old-device pairing degrades
to "device control channel timeout" on trace commands; the reverse
pairing simply never receives them. Device shutdown must not leave the
device's command reader parked (obd-device `shutdown(2)`s the channel on
its way out — a parked coroutine would stall the scheduler's teardown
drain). All device-side WRITERS (status reports, trace replies, the
expiry event — different coroutines on different workers) go through
one shared serialized writer (`ControlChannelWriter`,
src/supervisor/device_control.hpp): AF_UNIX SOCK_STREAM has no PIPE_BUF
atomicity, so without serialization two concurrent small writes could
interleave into a corrupted line — and every write loops until the
whole line is out, since a short write would fuse lines just as well.
The device's control fd is `O_NONBLOCK` and the writer DROPS a line
(never blocks a scheduler worker) if the supervisor stalls long enough
to fill the socket buffer. Framing consequence is bounded and
self-healing: a partial prefix fuses with the next complete line and is
dropped by the reader as one malformed (non-JSON) line, after which
framing is clean again — the channel tolerates the loss of at most one
line per stall episode (command replies carry a 30 s timeout; status is
re-queryable).

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
<device_bin> --blank-size <bytes> --blank-dir <workspace> \
             [--global <overlaybd.json>] --control-fd 3 [--dev-id <N>]
```

The blank form (ADR-0014) replaces `--config`; `--global` is omitted when
empty; `--dev-id` is omitted when negative.
There is no PATH lookup — the supervisor resolves the binary itself. On
`execve` failure the child `_exit(127)`s; the parent observes EOF on the
channel plus exit code 127 and reports "exec failed (obd-device not found
or not executable)" (`Child::note_reaped`).

### Daemon model

`run_daemon` runs three concurrent activities on the Elio scheduler:
the **accept loop** (one coroutine per one-shot client), the **reaper** (a
`signalfd` on SIGCHLD that reaps its registered device children one pid at
a time with `waitpid(pid, …, WNOHANG)` — never `waitpid(-1)`, so a helper
child owned by another component, e.g. the mode-3 mkfs runner, keeps its
exit status), and one **monitor coroutine per child** (a `LineReader` over
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
- `inline constexpr int kProtocolVersion = 4` — control-protocol revision;
  increments only for additive batches (see the additive-only rule above).
- `inline constexpr std::string_view kProjectVersion` — the project version
  string, wired from CMake `project(... VERSION ...)` via the
  `OBD_VERSION_STRING` compile definition so it cannot drift; `"dev"` is
  the fallback for non-CMake builds.
- `struct CreateCommand` — `id`, `config`, `global` (`""` = supervisor
  default), `device_bin` (`""` = supervisor default), `dev_id` (`-1` =
  auto). Descriptive mirror of the image-mode create command's fields
  (blank creates carry `BlankSpec` instead).
- `struct BlankSpec` — `size` (bytes) + optional `mkfs` type; the
  additive `blank` object of create (ADR-0014 modes 2/3).
- `inline constexpr uint64_t kMaxBlankSizeBytes` — operator sanity bound
  for blank device sizes (16 TiB).
- `bool valid_mkfs_type(const std::string&)` — charset check for a safe
  `mkfs.<type>` suffix.
- `std::optional<BlankSpec> parse_blank_spec(const nlohmann::json&,
  std::string& error)` — value-level blank validation (positive,
  512-aligned, within the size bound, safe mkfs type).
- `struct IdCommand` — `cmd`, `id`; shape of `destroy` / `status`.
- `struct CommitCommand` — `id`, `user_tag` (optional); shape of `commit`
  (ADR-0014).
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
  (`kProjectVersion`), and `features` (a JSON array of capability strings,
  currently `["commit","trace","resize","blank"]`).
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
  `global_path` (may be empty), `dev_id_request` (`-1` = auto), `recover`,
  and — for blank devices — `blank` + `blank_size` + `blank_dir`
  (ADR-0014; a blank spec carries no `config_path`).
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
  `stop_timeout_sec` (10), `max_recovery_attempts` (3), and the ADR-0014
  blank-device knobs: `blank_dir` (default
  `/var/lib/overlaybd-elio/devices` — each blank device owns
  `<blank_dir>/<id>/`), `mkfs_runner` (the mode-3 `MkfsRunner`;
  empty = the default fork/exec runner bounded by `mkfs_timeout_sec`,
  default 300) and `mkfs_timeout_sec`.
- `class MkfsRunner` (abstract) + `MkfsRunnerPtr` — injectable host
  `mkfs.<type>` runner (ADR-0014 mode 3); tests install a mock so the
  suite never executes host mkfs.
- `elio::coro::task<int> run_daemon(const DaemonConfig&)` — binds the
  socket, runs accept loop + reaper + monitors until SIGTERM/SIGINT, then
  shuts down gracefully (children terminated first). Returns the process
  exit code (1 if the socket cannot be bound, 0 otherwise). Caller must
  have blocked the handled signals process-wide (signalfd model).

Command handlers are internal to `src/supervisor/daemon.cpp` but define the
observable semantics: `create` rejects empty ids, ids containing `/` (and
`.` / `..` — the id becomes a workspace path for blank devices), missing
config files, missing device binaries, and duplicate ids; a blank create
additionally validates the blank spec (size + optional mkfs, see
`parse_blank_spec`) and, when mode 3 is requested, runs the mkfs runner
against the ready device's block path before replying;
`destroy`/`status`/`commit` reject unknown ids with `{"ok":false,"error":"no such
device: <id>"}`; `commit` additionally rejects upper-less and sparse-upper
devices and stops a live device before sealing (see "Offline commit").
Blank-born devices commit exactly like image-born ones: their upper path
and kind are recorded at create time (from the workspace layout, no config
pre-parse needed).

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
- The reaper never calls the wildcard `waitpid(-1, …)`: it sweeps the
  registry's device pids only, so it can neither steal a helper child's
  exit status (the ADR-0014 mode-3 mkfs runner owns its own child) nor
  wait on a pid it does not manage. Its registry snapshot is taken under
  `mu_` and each pid is reaped with `waitpid(pid, …, WNOHANG)`; a child
  replaced meanwhile is simply already reaped and skipped.
- Inputs are not mutated: `ChildSpec` and `DaemonConfig` are read at
  spawn/run time; `parse_command` returns an owning `nlohmann::json`.

## Stability Contract

- **Both wire protocols are T1 wire contracts** (AGENTS.md): the control
  channel's command/reply shapes (`cmd`, `id`, `config`, `blank` with
  `size`/`mkfs`, `global`, `device_bin`, `dev_id`; `ok`/`error` envelope;
  the `hello` handshake fields `protocol`/`version`/`features`;
  per-command success fields — including the blank create reply's
  additive `mode`/`size`/`mkfs`) and the status channel's shapes (`state`
  plus optional `device`/`error`; the state vocabulary
  `starting`/`ready`/`failed`/`stopped`) may only change with an ADR —
  and then only **additively** (see the additive-only evolution rule
  above; ADR-0014). obdctl, obd-supervisor, and obd-device may be
  upgraded independently.
- **The fd-3 + argv contract** between supervisor and obd-device
  (`--control-fd 3`, `--config` for image mode, `--blank-size
  <bytes> --blank-dir <workspace>` for blank mode (ADR-0014), optional
  `--global`, optional `--dev-id`, CLOEXEC-cleared fd 3) is part of the
  same wire contract.
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
  and `status` parse; a `create` with neither `config` nor `blank`, an
  unknown `cmd`, and non-JSON input are rejected; a `ready` status line
  with a `device` field parses; `{}` is rejected; and
  `make_device_status` → `parse_device_status` round-trips a `failed`
  status with its `error` field. This pins the exact wire shapes
  documented above.
- `supervisor: mkfs runner maps exit codes and bounds the timeout` — the
  REAL default mkfs runner (no daemon) against throwaway PATH shims:
  success, a nonzero exit code, the exec-not-found 127 mapping, an unsafe
  fs type refused before it reaches argv, and a shim sleeping past the
  bound reported as `-ETIMEDOUT` after SIGKILL (the runner owns its
  child).
- `supervisor: blank spawn argv carries the blank flags and global` — the
  ADR-0014 spawn contract: `--blank-size N --blank-dir D [--global G]
  --control-fd 3` and never `--config`, asserted on the child's real argv.
- `supervisor: default mkfs runner completes without the reaper stealing
  it` (integration, `tests/integration/test_commit.cpp`) — regression for
  the reaper/mkfs-helper interaction: the daemon runs its REAL runner
  against a PATH shim, the mode-3 create succeeds with `mkfs:"ext4"`, the
  shim saw the device path, and the daemon keeps serving afterwards. A
  wildcard `waitpid(-1)` reaper steals the helper's status and makes this
  fail.
- `supervisor: stale blank-create failure leaves a newer device alone`
  (integration, `tests/integration/test_commit.cpp`) — a create held
  inside its mkfs step while the id is destroyed and re-created: the
  stale mkfs failure must clean up only ITS OWN entry, leaving the newer
  device (and its pid) alive.
- `supervisor: obd-device rejects malformed blank flags` — the real
  obd-device binary's `--blank-size`/`--blank-dir` validation: negative,
  zero, unaligned, junk, overflowing and above-bound sizes, the missing
  workspace, and `--config` combined with blank are all usage errors
  (exit 2) before any device work.
- `supervisor: create blank spec parses and validates size and mkfs` —
  pins the ADR-0014 blank create grammar: `create` with `blank` (mode 2,
  and mode 3 with `mkfs`) parses; `config` and `blank` are mutually
  exclusive and one is mandatory; wrong-typed `blank`/`size`/`mkfs`
  fields are parse errors; and `parse_blank_spec` rejects zero, unaligned,
  oversized sizes and unsafe `mkfs` types (`valid_mkfs_type` charset).
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
- `supervisor: commit command parses and validates its fields` — pins the
  additive `commit` grammar (requires `id`, optional `user_tag`, unknown
  fields ignored) and that the `hello` handshake advertises the `commit`
  feature gate (ADR-0014).
- `supervisor: commit stops the device and seals its upper offline`
  (integration, `tests/integration/test_commit.cpp`) — a real daemon with
  a fake obd-device: commit on an unknown id / sparse upper / upper-less
  device are precise errors; commit on a **live** LSMT-upper device stops
  it (bounded reap) and seals its checkpointed upper, replying with
  `path`/`sha256`/`size`; a second commit fails with "already sealed";
  the sealed file re-opens as a valid LSMT RO layer with the
  checkpointed content (ADR-0014). Runs without privileges.
- `supervisor: blank create serves a writable zero base and commit seals its upper` (integration, `tests/integration/test_commit.cpp`) — the
  ADR-0014 mode-2 path end to end with the fake device: `create` with
  `blank` (no config) builds the workspace (`overlaybd.zero` + the
  writable `overlaybd.rw`) via `open_blank_device`, the fake round-trips
  a payload through the merged stack with unwritten regions reading zero,
  commit stops it and seals the blank-born upper, and a second commit is
  "already sealed". A recording mkfs mock asserts host mkfs is never
  invoked in mode 2.
- `supervisor: mode-3 mkfs runs only when the blank spec requests it` (integration, `tests/integration/test_commit.cpp`) — the mode-3 gate
  and error path: a plain blank create never invokes the (mock) mkfs
  runner; `blank.mkfs` invokes it exactly once with the requested type
  and the reported device path; commit of a formatted (mode-3) upper is
  refused with the "host mkfs" boundary error; a failing mkfs is a clean
  create error and the half-created device entry is removed. Host mkfs
  is never executed by the suite.
- `supervisor: device trace control answers malformed-typed fields with clean errors` —
  the device-side trace command loop over a real socketpair:
  wrong-typed `trace_start` fields get a clean error reply (seq echoed)
  and the loop keeps serving (never-throws contract, ADR-0013).
- Trace recording (ADR-0013; integration,
  `tests/integration/test_trace_record.cpp`; the fake device opens a
  REAL image against a mock registry and serves the real device-side
  trace protocol): `integration: trace recording captures remote reads
  end to end` — full start → scripted workload → stop flow, reply
  fields, additive `trace` status field, and a blob the codec reader
  accepts with exactly the workload's coalesced record; `integration:
  trace recording duration expiry finalizes without a client call` —
  the device-side timer finalizes on its own and a late stop returns
  the same stats; `integration: trace recording survives client
  disconnect mid-record` — a CLI that vanishes mid-command cannot leak
  a recording device; `integration: trace recording crash mid-record
  marks the trace lost` — a SIGKILLed device's trace status flips to
  "lost"/"device_exit" and the never-finalized output file stays a
  0-byte non-blob; `integration: trace recording rejects bad requests
  cleanly` — protocol validation, unknown ids, idle stops,
  out-of-bounds durations, and double starts are precise errors that
  leave the daemon and the active recording unaffected.

Run: `ctest --test-dir build --output-on-failure` (no privileges needed;
the spawn tests create their fake binaries under a temporary directory).

### Crash recovery (ADR-0010)

Each device is supervised for its whole lifetime by one coroutine
(`supervise_entry`) that reads the child's status channel and, on an
**unexpected** exit (not a requested `destroy`, not daemon shutdown),
respawns the child with `--recover --dev-id N`: the ublk device was
created with `UBLK_F_USER_RECOVERY` and survives serverless in the kernel,
so the replacement *attaches* instead of re-creating.
**Blank-device caveat:** the respawn re-runs the blank assembly, which
recreates the writable upper (`overlaybd.rw`) with `O_TRUNC` — unsealed
writes are lost across a device crash, exactly as for an image-mode
writable upper. Commit (or stop gracefully) before letting a valuable
blank device crash-recover. The dev id is parsed
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
