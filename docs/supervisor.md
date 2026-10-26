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
one line and closes during an ordinary exchange. Shutdown can interrupt the
exchange with EOF or a connection reset before a reply is delivered. A
missing reply does not establish whether a mutating command executed.

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
`virtual_size` fields; `create`'s optional `dev_id` must be an integer
in `[-1, INT32_MAX]`. Unsigned values are range-checked before conversion,
so an oversized explicit ID cannot wrap to the `-1` automatic-allocation
sentinel. Invalid IDs return a protocol error; `hello` and `list` take
no fields; anything else is "unknown cmd". Field TYPES are validated at
parse time (value-level
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
  revision (`kProtocolVersion`, currently 6; 1 = initial command set,
  2 = added `commit`, 3 = added `trace_start`/`trace_stop` and the
  additive `trace` status field, 4 = added `resize` and the
  `virtual_size` create/commit fields (D3) and the `blank` create mode
  (ADR-0014), 5 = retained startup ownership (ADR-0022),
  6 = Sparse export and parent identity (ADR-0027)); `version` is the
  project version string wired from CMake
  (`kProjectVersion`); `features` is a JSON array of strings — the
  capability gate for optional commands, currently
  `["commit","trace","resize","blank","startup_owner","sparse_export"]`.
- **create**: `{"ok":true,"id","pid","device"}` — `device` is the child's
  reported `/dev/ublkb<N>`. With an optional `virtual_size` (bytes > 0)
  the device is created with that capacity instead of the image's
  declared size (D3 headroom, "Online resize" below). A blank create's
  reply adds `"mode":"blank"`, `"size":<bytes>`, and — only when the
  request carried an `mkfs` — `"mkfs":"<type>"` (the mode-3 format ran
  on the new device before the reply).
- **destroy**: `{"ok":true,"id"}` only after eligible teardown and actual reap.
  Protected startup uses explicit release; timeout/refusal keeps its entry.
- **list**: `{"ok":true,"devices":[{"id","pid","state","device","error"}, ...]}`.
- **status**: `{"ok":true,"id","pid","state","device","error","exit_code"}`
  (`exit_code` is -1 until the child is reaped). Both `status` and
  `list` replies also expose `shutdown_pending`, `startup_protected`, and
  the current `startup_owner` snapshot when available (see below). They carry
  the additive **`trace`** object once a recording
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

This rule permits independent client upgrades. The private supervisor/device
pair additionally requires the versioned startup grant in ADR-0022; supported
old/new pairs reject startup before publication instead of silently degrading.

### Offline commit (ADR-0014)

`commit` seals a device's writable LSMT-RW upper into a standard sealed
LSMT layer **offline** — the device process is never alive while its upper
is being sealed. Contract:

- **Stopped-device contract.** Protected/retained startup generations are
  refused, including an externally reaped owner without retirement evidence.
  For an eligible live device, commit stops it first: SIGTERM plus a bounded reap
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
  ready status additionally reports `upper_parent_uuid`: empty means no parent,
  while an omitted field means unknown. Sparse export requires the current
  child and all observed ready generations to agree on this identity.
- **The shutdown checkpoint.** An unsealed LSMT-RW file's segment index is
  memory-only (docs/format.md); a graceful obd-device shutdown therefore
  **checkpoints** the index into the file (unsealed trailer) before
  exiting, which is what the supervisor's offline seal consumes. A device
  that crashes or is SIGKILLed before that checkpoint completes has no
  checkpoint and its upper is lost; if the kill races after the checkpoint
  has already been written, offline `commit` can still consume it. Missing
  checkpoints fail with a precise error.
- **Sealing** runs in the supervisor process via
  `src/format/lsmt_rw.hpp::LsmtRwLayer::seal_file` over the async IO
  backend (no blocking work on the Elio workers). The sealed uuid is
  content-derived — identical upper content seals to identical bytes
  (docs/format.md, seal determinism invariant).
- **Concurrency.** One commit per device at a time: a second commit of
  the same device while one is in flight is rejected ("commit already in
  progress"), and `destroy` of the same device is rejected with the same
  busy reason while commit admission is held. The stop-and-seal critical
  section is serialized against a crash-recovery respawn (ADR-0010) by a
  per-entry mutex: either the respawn completes first and commit stops the
  recovery child too, or the respawn aborts on the `destroying` flag — a
  seal never runs while a device child is booting or alive.
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
  upper"), unavailable or changed Sparse parent identity, a concurrent commit ("commit already in progress"),
  a destroy already in progress ("device is being destroyed"), a config
  unreadable at create time, a missing upper file, an already-sealed
  upper, a missing or invalid shutdown checkpoint (device crashed), a
  rejected `virtual_size` re-baseline (grow-only or misaligned), and
  stop-timeout.

The integration case `supervisor: sparse export preserves the opened parent and reusable source`
uses the real image assembly and a simulated device child, a nonzero parent,
DATA/ZERO/INHERIT readback, changed config, and output-only capacity expansion.
It exercises the control socket and stop/reap path without a kernel ublk device.

Sparse uppers use the separate export path described by proposed ADR-0027.
After stopping and reaping the child, commit recovers exact ownership and writes
an ordinary sealed LSMT delta to `<upper.dir>/overlaybd.sparse.lsmt`, or to the
explicit `output` path (`obdctl commit ID --output PATH`). DATA is copied, ZERO
is encoded, and INHERIT is omitted. The source remains reusable. Identical
retries succeed; conflicting existing output fails without replacement.
`virtual_size` grows only the exported geometry, leaving source capacity intact.
Sparse parent identity comes from the child's actual opened top data layer,
not a later reread of the config. Missing or inconsistent identity refuses
commit before stopping the device. Protocol version 6 advertises `sparse_export`.
LSMT in-place commit does not accept `output`.

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
  dev-size-only (reads past the image's end are zero-filled by the queue
  handler).
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
wedged in uninterruptible IO. If the helper is still unreaped by then, its
pid is handed to the daemon's SIGCHLD reaper
(`MkfsRunner::take_orphan_pids()`), which collects abandoned helpers with
`WNOHANG` on every wake — so a wedged helper cannot stay a zombie, and
nothing detached (no coroutine, no thread) is left for shutdown to join.
The reaper still never uses the wildcard `waitpid(-1, …)`: it waits on a
helper pid only once its owner has given up on it.

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
  immediately. A matching reply releases admission, but its handler retains
  that operation's response and event until consumption. A later command or
  channel EOF cannot replace an already accepted response; timeout and
  write-failure cleanup can retire only the operation they belong to. If the
  bounded wait reports timeout, the caller receives the timeout error even
  when a reply races the later result-collection step.
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
  Busy versus unavailable is captured under the registry mutex when
  admission is rejected; completion or EOF of the earlier command cannot
  change the reason returned to that rejected client.
- **Recording captures guest OnDemand REMOTE reads only.** Records fire
  per fully-satisfied OnDemand pread on a lower's remote source —
  LayerStore local hits produce no record, and structural warm-up, trace
  replay, and background-fill traffic are filtered out even when they
  fall inside the window.

### Channel 2: obd-device → supervisor (status channel, fd 3)

A `socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC)` created before fork.
The child end is dup2'd to **fd 3**, cleared of CLOEXEC, and survives
`execve`; the device binary receives `--control-fd 3` on its argv. Same
JSON-lines framing, same 64 KiB cap. Device → supervisor traffic is
lifecycle **status lines** plus, since protocol 3, `reply`-discriminated
command replies/events (see "Trace recording" above; the supervisor
routes on the `reply` field and treats everything else as a status
line). Supervisor → device traffic includes the version-1 startup grant,
status/release commands and ordinary trace/resize commands. TERM/KILL policy
applies only to eligible unprotected children; protected startup requires
explicit release. Supported old/new supervisor/device pairs reject the missing
or unknown `--startup-generation` argument before publishing device work.
Device shutdown completes its private reader before destroying borrowed hooks;
the same reader spans startup and ready operation.
All device-side WRITERS (status reports, trace replies, the
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

Lifecycle vocabulary (`DeviceStatus` / `make_device_status`; supervised
reports additionally carry `startup_owner` below):

```json
{"state":"starting"}
{"state":"ready","device":"/dev/ublkb<N>"}
{"state":"failed","error":"<reason>"}
{"state":"stopped"}
```

`state` is required (string); `device` and `error` are optional and
omitted when empty. The supervisor additionally maintains one internal
state, `exited`, which the child never sends: it is assigned when the
status channel hits EOF for an unprotected generation, or the reaper collects
the process (`Child::try_reap`). Protected channel loss remains a failed owner.
EOF only reports loss of the control channel: the
process can still be checkpointing or destroying its ublk control owner.
The separate `reaped()` / `reaped_event()` barrier records actual process
exit and must precede entry removal or offline sealing. The externally
visible lifecycle is therefore:

```
starting → ready → stopped → exited
starting → failed ─────────→ exited   (exec failure: exit code 127)
starting ──────(crash)─────→ exited   (reaper fills exit_code)
```

The opt-in startup kernel fixture also requires the raw collected wait status:
reap proves collection, not successful process shutdown. In test-enabled builds,
`OBD_TEST_STARTUP_FIXTURE_DIR` plus a `partial-recovery-v1`,
`adopted-source-v1` or `report-refusal-v1` marker enables a Child-owned, exclusive
`child-<generation>.reap` mapping prepared before fork.
The matching `Child::try_reap` publishes PID, generation, reaper PID and raw wait
status before notifying waiters, using fixed stores without allocation or file
I/O at collection. Missing evidence makes the fixture fail but never changes
process ownership or teardown. The controller requires the intentionally killed
original to show SIGKILL and the released replacement to show normal exit zero,
even after a maintenance shutdown erases their registry entry. This internal
test record is separate from the public supervisor protocol.

### Protected startup ownership (ADR-0022)

Every supervised generation offers `startup_owner` version 1 before publishing
ublk work. The parent registers its Child, monitor and channel, installs
protection from automatic termination, then sends:

```json
{"cmd":"startup_begin","version":1,"generation":7}
{"cmd":"startup_status","generation":7}
{"cmd":"startup_release","generation":7,"seq":1}
```

Lifecycle replies use the command name in `reply`, echo the generation and any
sequence, and include the observed `startup_owner`. The single monitor routes
both status and replies; all parent writes share a serialized channel writer.
Grant delivery failure does not revoke protection. Missing capability/status
never selects a legacy cleanup mode.

The additive snapshot has these fields:

| Field | Meaning |
|---|---|
| `version`, `generation`, `revision` | Version 1; immutable nonzero child identity; monotonic observation revision. |
| `phase` | `preparing`, `control_pending`, `retained`, `releasing`, `ready`, or `released`. |
| `device_id` | Kernel device ID, or -1 before known. |
| `reason_errno` | Nonnegative startup failure reason; 0 when none. |
| `control_pending`, `kernel_debt_known` | Whether control work remains and whether raw ownership accounting is known. |
| `raw_published`, `raw_retired` | Original queue-command accounting; a cancellation CQE is not original retirement. |
| `release_sequence`, `release`, `release_errno` | Operator attempt identity, `none`/`pending`/`refused`/`released`, and nonnegative release errno. |

Only coherent current-generation observations may change protection; stale or
contradictory records cannot restore weaker state. A ready handoff needs both
`state=ready` and matching `phase=ready`. Failure stays `state=failed` with
retained/releasing ownership; a lost channel or actual external reap cannot
manufacture successful retirement or permit another replacement.

`destroy` admits an increasing sequence in `[1, 2^63)` on the current owner.
Acceptance reports pending. The private release ledger survives the client's
wait, failed delivery and timeout. Retrying observes the pending attempt;
a terminal refusal permits a newer attempt. Ready versus release admission is
serialized in the device, so release acceptance cannot be lost at handoff.
The child settles original control/raw commands and queue-owner frames, closes
queue/cdev resources, and performs one authorized DEL before reporting release.
The supervisor additionally requires actual reap before removing that entry.
Unknown outcomes remain retained; timeout never escalates to TERM/KILL.
Safe automatic cleanup without an operator sequence is a separate terminal
case, not acknowledgement of an accepted explicit request.

Create timeout/failure and daemon shutdown do not authorize destructive
cleanup of an unadopted recovery. During shutdown maintenance, status/list
include `shutdown_pending=true`; hello/list/status/destroy remain available,
while other new work is refused. See "Daemon shutdown" for execution ownership.

### Spawning contract

`Child::spawn` (`src/supervisor/child.cpp`) builds **argv and the Child owner
entirely before fork**, then: socketpair → fork → in the child, only async-signal-safe
calls (`close`, `dup2`, `fcntl`, `execve`, `_exit`) because the parent
hosts a live Elio runtime and anything else is UB. The parent immediately
attaches the PID and channel to the prepared owner through nonthrowing
assignments. An allocation failure during this preparation creates no process
or channel; a later exception inside this pre-grant spawn operation closes the
channel and collects the owned child before escaping. After a grant attempt,
protected ownership instead follows the retained-maintenance rules. The child
argv is:

```
<device_bin> --config <config.json> [--global <overlaybd.json>] \
             --control-fd 3 --startup-generation <G> [--dev-id <N>]
<device_bin> --blank-size <bytes> --blank-dir <workspace> \
             [--global <overlaybd.json>] --control-fd 3 --startup-generation <G> [--dev-id <N>]
```

The blank form (ADR-0014) replaces `--config`; `--global` is omitted when
empty; `--dev-id` is omitted when negative. The supervisor allocates a fresh
nonzero generation before each initial or recovery spawn; IDs reused after
destroy do not reuse generations within the daemon lifetime.
There is no PATH lookup — the supervisor resolves the binary itself. On
`execve` failure the child `_exit(127)`s; the parent observes EOF on the
channel plus exit code 127 and reports "exec failed (obd-device not found
or not executable)" (`Child::try_reap`).

### Daemon model

`run_daemon` runs three concurrent activities on the Elio scheduler:
the **accept loop** (one coroutine per one-shot client), the **reaper** (a
`signalfd` on SIGCHLD that reaps its registered device children one pid at
a time with `waitpid(pid, …, WNOHANG)` — never `waitpid(-1)`, so a helper
child owned by another component, e.g. the mode-3 mkfs runner, keeps its
exit status; the same sweep collects the helper pids that runner
ABANDONED via `take_orphan_pids()`, see the mkfs bound above), and one
**monitor coroutine per child** (a `LineReader` over
the child's status fd feeding `Child::update_status`). The children
registry of shared `DeviceEntry` owners is guarded by an
`elio::sync::mutex` — coroutine-aware, never held across heavy work.
Signals must be blocked process-wide by the caller (signalfd model).

Bounded waits: `create` waits up to `ready_timeout_sec` (default 60 s) for
ready/failed/exited and returns an error on failure or timeout. A protected
startup owner remains registered. For an eligible unprotected child,
`destroy` uses TERM, `stop_timeout_sec` (default 10 s), then KILL and a 2 s
reap grace; failure to confirm reap retains the entry. For protected startup,
destroy requests explicit release and retains the entry on refusal/timeout.
Commit refuses protected ownership and otherwise requires actual reap before
sealing. Reaping proves process exit, not retirement of kernel loans.

Shutdown keeps maintenance service operational while protected owners or
unpublished create admissions remain. Its eventual task drainage and
exception behavior are specified under "Daemon shutdown" below.

## Public API

### `src/supervisor/protocol.hpp`

- `inline constexpr size_t kMaxMessageBytes = 64 * 1024` — maximum JSON-line
  length on both channels.
- `inline constexpr int kProtocolVersion = 5` — control-protocol revision;
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
  currently `["commit","trace","resize","blank","startup_owner","sparse_export"]`).
- `inline constexpr int kStartupOwnerVersion = 1` — private startup capability.
- `struct StartupOwnerStatus` — the generation, revision, phase, device ID,
  reason, control/raw-debt and release fields specified above.
- `nlohmann::json startup_owner_json(const StartupOwnerStatus&)` and
  `std::optional<StartupOwnerStatus> parse_startup_owner(const nlohmann::json&)`
  — versioned ownership encoding and validation.
- `bool valid_startup_owner(const StartupOwnerStatus&) noexcept` — coherent
  facts; Child additionally validates request admission and monotonicity.
- `struct DeviceStatus` — `state` (`starting` | `ready` | `failed` |
  `stopped`), `device` (`/dev/ublkb<N>` when ready), `error` (when failed),
  optional `startup_owner`.
- `std::optional<DeviceStatus> parse_device_status(std::string_view)` —
  `nullopt` unless the line is a JSON object with a string `state`;
  `device`/`error` default to `""`. A present malformed ownership record is
  rejected. Codec acceptance of absent ownership does not enable a legacy
  bypass in the daemon's mandatory startup handshake.
- `std::string make_device_status(const DeviceStatus&)` — the obd-device
  side encoder; omits empty `device`/`error`. Round-trips with
  `parse_device_status`.

### `src/supervisor/child.hpp`

- `struct ChildSpec` — `id`, `device_bin` (absolute path), `config_path`,
  `global_path` (may be empty), `dev_id_request` (`-1` = auto), `recover`,
  `startup_generation` (nonzero for supervised startup),
  and — for blank devices — `blank` + `blank_size` + `blank_dir`
  (ADR-0014; a blank spec carries no `config_path`).
- `static std::unique_ptr<Child> Child::spawn(const ChildSpec&)` — forks
  and execs per the spawning contract above; the parent end of the channel
  is set `O_NONBLOCK`. Throws `obd::error` on socketpair/fork failure.
  Argument preparation and owner allocation may also throw. Returning the
  owner does not confirm exec or readiness: an exec failure is observed
  after collection through the child's status (`exit_code == 127`), rather
  than thrown synchronously by `spawn`.
- `~Child()` — cannot force-kill protected startup ownership. Normal daemon
  ownership retains the complete operational graph until release; the
  destructor's blocking fallback is not a replacement for that ownership.
  For an eligible unprotected child it closes the held control fd and uses
  best-effort KILL/reap. Never signals an already reaped PID.
- `const std::string& id() const`, `pid_t pid() const`.
- `int control_fd() const` — parent end of the status channel.
- `int release_control_fd() noexcept` — detaches the fd (the monitor
  coroutine takes ownership; `~Child` then won't close it).
- `struct Child::Status` — `state` (starts as `"starting"`; supervisor-side
  vocabulary adds `"exited"`), `device`, `error`, `exit_code` (`-1` until
  reaped).
- `Status status() const` — mutex-guarded snapshot.
- `Child::Snapshot snapshot() const` — one coherent observation containing
  `status`, optional `startup_owner`, and `startup_protected`, used by public
  list/status so lifecycle and protection facts cannot come from different
  revisions.
- `void update_status(const Status&)` — publishes a status; fires
  `ready_event` the first time the state becomes `ready` / `failed` /
  `exited`, and `exit_event` on `exited`. Each event fires at most once.
  Late monitor updates cannot overwrite a confirmed reap's terminal state
  or exit code.
- `elio::sync::event& ready_event()`, `elio::sync::event& exit_event()`.
- `bool reaped() const`, `elio::sync::event& reaped_event()` — actual
  process-reap state and its one-shot event. EOF never fires this event.
- `void terminate() noexcept` — SIGTERM. `void kill() noexcept` — SIGKILL.
  Both are no-ops for protected startup, an untracked PID or an already
  reaped PID. Protection and signal admission share the Child mutex.
- `bool try_reap()` — performs owned-PID `waitpid(..., WNOHANG)` and
  records a newly confirmed reap, returning true only for that transition:
  `exit_code` = `WEXITSTATUS`, or `128 + WTERMSIG` when signaled; state
  becomes `"exited"` (a clean `stopped`/`ready` history is deliberately not
  preserved — the state is bookkeeping, the detail lives in `exit_code`);
  exit code 127 with no prior error gets the "exec failed" message. It
  fires `reaped_event` after publishing the result. Callers must never
  independently `waitpid` a live `Child`'s PID; this method owns collection
  and prevents a reap-to-publication interval in which another thread
  could signal a recycled PID.
- `bool try_notify_waiters()` — retries sticky ready/exit/reap notifications.
  Returns false if native event dispatch storage allocation fails, leaving
  that event's waiters registered. Attempts all events independently.
  Coroutine owners retain the child, yield and retry before abandoning
  their observation; the monitor, reaper and stop helper apply this rule.
  Reap facts and exit codes remain committed if notification needs retry.

- `uint64_t startup_generation() const noexcept` — immutable process identity.
- `bool protect_startup(uint64_t generation)` — install protection before grant
  delivery; reject a foreign or completed owner.
- `bool startup_protected() const`, `bool startup_granted() const` — synchronized
  facts; granted includes a conservatively protected delivery attempt.
- `std::optional<uint64_t> admit_startup_release()` — persist operator authority;
  return the current pending sequence or a fresh sequence after refusal.
- `bool observe_startup(const StartupOwnerStatus&, bool ready_report)` — validate
  identity, monotonic facts and release correlation before changing protection.
- `std::optional<StartupOwnerStatus> startup_status() const` — diagnostic snapshot;
  copying diagnostic strings can allocate and is not an ownership transfer.

### `src/supervisor/device_control.hpp`

`DeviceControlSession` keeps one private-channel reader across startup and
ready operation. Its `Lifecycle` callback has signature
`std::function<std::optional<nlohmann::json>(const nlohmann::json&)>`; a present
reply handles a lifecycle command, while absence permits ordinary dispatch.
`publish_ready(std::shared_ptr<image::TraceRecorder>, DeviceControlHooks)`
publishes serving hooks under a mutex. `load` copies that pair only after
publication; `lifecycle` invokes the immutable callback. Borrowed captures must
outlive the reader and its final frame destruction.

`run_device_control_with_session(ControlChannelWriterPtr,
std::shared_ptr<image::TraceRecorder>, DeviceControlHooks,
std::shared_ptr<DeviceControlSession>)` returns `elio::coro::task<void>` and
serves both phases without competing readers. Command/reporting exceptions do
not transfer startup ownership or authorize teardown. The original
`run_device_control` entry point remains available for ready-only callers.

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
  enters shutdown maintenance if protected owners remain, then drains all
  owned tasks and eligible children. Returns the process
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
device: <id>"}`; `commit` additionally rejects upper-less devices and stops a live device
before sealing or exporting (see "Offline commit").
Blank-born devices commit exactly like image-born ones: their upper path
and kind are recorded at create time (from the workspace layout, no config
pre-parse needed).

Create admission reserves an absent ID before asynchronous configuration or
child setup. Another create for that ID receives the existing duplicate-ID
error before spawning a child or initializing a workspace. Unrelated IDs
continue independently; the registry mutex covers only admission and
publication. Private reservations never appear in list/status as childless
entries. Publication checks the reservation owner and inserts a complete
entry atomically, then releases the reservation. Failed setup invalidates
only its own token; an unpublished child's final owner kills/reaps it before
that token expires. Inactive weak reservations are pruned by the next create.
Once published, ordinary registry ownership and existing failure/teardown
rules apply, including when the originating create remains inside mkfs.

## Invariants & Guarantees

- **Isolation**: every device is a separate process; a child crash, hang,
  or exec failure is detected (EOF on fd 3 and/or SIGCHLD), recorded, and
  reported — siblings and the daemon are unaffected (ADR-0004).
- **Reaping completeness**: every spawned child is reaped exactly once —
  by the SIGCHLD reaper or a stop path through `Child::try_reap`, with
  the ordinary Child fallback is unavailable to protected startup owners.
  Removal requires eligible retirement and actual reap; sealing refuses
  unresolved startup ownership. Protected generations retain the daemon.
- **One-shot control channel**: an admitted command produces at most one
  reply before the connection closes. Malformed commands map to a
  `{"ok":false,...}` reply; shutdown or I/O failure can prevent delivery
  (see "Daemon shutdown").
- **Bounded process waits**: readiness waits use `ready_timeout_sec`;
  eligible graceful and post-KILL reap waits use `stop_timeout_sec` + 2 s.
  Protected release waits may return unresolved; resource retention itself
  is not bounded by a client deadline.
  Required notification retries retain their owners through allocation
  failure, as do task drains; these are process-wait bounds, not a global
  wall-clock deadline under sustained memory exhaustion.
- **Line discipline**: both channels cap a line at 64 KiB; a peer that
  exceeds the cap is treated as disconnected (`LineReader` returns EOF).
  Anchor: `src/supervisor/protocol.hpp::kMaxMessageBytes`.
- **Fork safety**: between fork and execve the child executes only
  async-signal-safe calls. Anchor: `src/supervisor/child.cpp::Child::spawn`.
- **Status monotonicity**: `ready_event` fires at most once, on the first
  creation-terminal state; `exit_event` fires at most once, on `exited`;
  `reaped_event` fires at most once, only after actual `waitpid` collection.
  Monitor lines cannot replace a confirmed reap's terminal status.
  Anchor: `src/supervisor/child.cpp::Child::update_status`.
- **Exit-code convention**: 127 means exec failure and is translated into a
  human-readable error; signaled exits are reported as `128 + signo`.
  Anchor: `src/supervisor/child.cpp::Child::try_reap`.

## Concurrency & Call Permissions

- `run_daemon` and all command handlers are **Elio coroutines** — they must
  run on the Elio scheduler. The registry mutex is `elio::sync::mutex`
  (coroutine-aware); it is never held across child IO. Recovery publishes
  the replacement child and recovery count together under this mutex,
  inside the per-device operation lock. Status/list retain the child and
  copy the count and trace metadata under the registry mutex, then build
  replies outside it. PID and status come from that retained generation;
  a reply already in progress may describe the previous generation.
  The previous child owner is released outside the registry mutex because
  child destruction can kill and reap the process.
- `Child::spawn` is a **plain blocking function** containing `fork`; call
  it from the daemon coroutine (as `cmd_create` does) but treat it as a
  cold path. After spawn, all interaction with the child is coroutine-based
  (monitor reads, event waits) except `terminate`/`kill`, which are
  signal-only and safe from any thread.
- `Child`'s mutable state (`st_`, event-fired flags, reap state) is protected
  by a plain `std::mutex`; `status()` / `update_status()` / `try_reap()` are
  safe to call from the monitor coroutine, the reaper coroutine, and
  command handlers concurrently. Signals and nonblocking collection plus
  terminal-state publication take the same mutex, so a numeric PID cannot
  be signalled after another thread has reaped it. Event publication
  happens outside the mutex.
- The control fd passes ownership exactly once via `release_control_fd()`
  into a shared stream retained by the monitor and in-flight writers.
  Monitor EOF removes channel availability from the registry; the fd closes
  when the last stream owner departs, after its pending I/O completes.
- The reaper never calls the wildcard `waitpid(-1, …)`: it sweeps the
  registry's device pids plus the pids the mode-3 mkfs runner explicitly
  handed over after abandoning them (`MkfsRunner::take_orphan_pids()`), so
  it can neither steal a LIVE helper child's exit status (the runner owns
  its child until it gives up on it) nor wait on a pid it does not
  manage. A handled pid is dropped from that list as soon as the wait
  answers with the pid or a terminal error (`ECHILD`). Its registry snapshot is taken under
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
  above; ADR-0014). ADR-0022 additionally requires a compatible
  supervisor/device startup-owner handshake; client upgrades remain additive.
- **The fd-3 + argv contract** between supervisor and obd-device
  (`--control-fd 3`, mandatory `--startup-generation`, `--config` for image mode, `--blank-size
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

Retained startup coverage is separate from kernel retirement evidence:

- `supervisor: retained startup survives release refusal then releases on retry`
  exercises the real daemon with a protocol peer and a correlated retry.
- `supervisor: SIGTERM preserves retained owners and serves maintenance commands`
  exercises maintenance observation, admission refusal and explicit destroy.
- `supervisor: malformed startup ownership never becomes legacy status`
  checks that invalid ownership input cannot select unprotected fallback.

These ordinary process/codec tests do not prove raw kernel-command retirement.
Acceptance additionally requires owned Linux 6.6 and current-kernel fixtures
covering original CQEs, native-frame retirement, close-before-DEL and reap.
The proposed ADR-0022 does not assert that the complete kernel matrix has run.


`supervisor: create validates device ids before integer narrowing` exercises
image and blank create requests at both valid endpoints and signed/unsigned
overflow boundaries, including `UINT64_MAX`. It checks the parser directly
and does not create a device.

The daemon-lifetime fixture records each exact weak `Child` owner before
registry publication can expose it to the reaper. Cleanup locks that
generation and uses its synchronized signal/reap methods, never a numeric
PID lookup or an independent `waitpid`. An already-reaped or expired
generation cannot authorize a signal to a replacement process. Final
reaping has a five-second fixture deadline after the runtime and client
thread finish; cleanup errors fail the test. The early signal also
releases monitors left behind by a broken daemon. These waits run on
fixture threads and do not define a product shutdown deadline.

The real-device teardown fixture pins its ready generation's sysfs,
character and block node identities with `O_PATH` before teardown. These
handles do not invoke device open or I/O. Post-teardown checks observe
only that generation; another device appearing at the released numeric ID
is not an original-node leak. Failed checks remain visible and never
trigger a numeric fallback `STOP_DEV`/`DEL_DEV` against a possible
replacement device. A nonprivileged pathname-replacement test covers
that observation rule without forcing global device-ID reuse.

The controlled recovery tests pause real command/monitor paths on a
four-worker runtime, while a synchronous client kills the fake child and
queries its replacement. The internal bounded coroutine gate adds no child
ownership and invokes no caller callbacks; only this test entry point pins
commands and monitors to distinct workers. No kernel ublk is required.

| Regression | Evidence |
| --- | --- |
| `supervisor: old command cleanup preserves the next waiter` | A consumes its response while B awaits its own device reply; B and C complete. |
| `supervisor: completed commands retain their distinct payloads` | B completes while A is parked; both retain their own sizes. |
| `supervisor: earlier device error survives later success` | A retains its device error after B succeeds. |
| `supervisor: earlier success survives later device error` | A retains its success after B receives a device error. |
| `supervisor: terminal command reply survives later channel EOF` | EOF fails only active B while accepted A keeps its response. |
| `supervisor: stale and malformed replies cannot complete a newer command` | Real routing processes old, mismatched, malformed and missing sequence replies before the valid B reply. |
| `supervisor: late reply after timeout cannot complete the next command` | A really times out; its later reply cannot complete B. |
| `supervisor: timed-out handler cleanup preserves a newer command` | A times out and parks; a late reply releases admission, and A cleanup cannot retire B. |
| `supervisor: closed peer fails an admitted command write` | The fake exits after admission and before the actual write; the handler reports its write error. |
| `supervisor: shutdown drains an active device command reply` | The real fake reply completes an active handler after shutdown begins, before daemon task drain returns. |
| `supervisor: rejected command keeps busy reason after pending reply` | Hold a rejected resize after admission unlock; the real monitor accepts the earlier command's matching reply on another worker before the rejected client receives its original busy reason. |
| `supervisor: rejected command keeps busy reason after channel EOF` | Hold the same rejection while device EOF fails the earlier request; the admission-time busy reason survives channel closure. |
| `supervisor: recovery publishes child and count together` | Status/list observed at the publication boundary agree on the replacement PID and recovery count. |
| `supervisor: status owns its child generation and trace snapshot` | A paused status retains the old child and reports its PID/count/recording trace after concurrent recovery marks the live trace lost. |
| `supervisor: list owns its child generation and trace snapshot` | A paused list preserves the same owned generation and trace while recovery and other commands proceed on another worker. |
| `supervisor: trace start metadata is owned by crashed generation` | A trace_start reply publishes on its retained entry before client completion; recovery marks that generation's live recording lost, and the parked handler cannot restore it. |
| `supervisor: trace start metadata cannot rewrite a reused id` | A parked trace_start completion from a destroyed entry cannot publish metadata into a newer device that reused the same id. |
| `supervisor: timed-out trace start reply cannot publish metadata` | A trace_start handler is parked after timeout wins; a later matching success reply is routed but cannot publish recording metadata. |
| `supervisor: trace start rejects wrong success discriminator` | A matching-sequence success reply with `reply:"resize"` cannot complete trace_start or publish trace metadata. |
| `supervisor: trace start rejects success without metadata` | A matching-sequence trace_start success without required metadata fields is returned as a clean error and leaves status trace-free. |
| `supervisor: trace stop metadata cannot rewrite a reused id` | A parked trace_stop completion from a destroyed entry cannot publish stopped metadata into a newer device that reused the same id. |
| `supervisor: trace stop rejects wrong success discriminator` | A matching-sequence success reply with `reply:"resize"` cannot complete trace_stop or publish stopped trace metadata. |
| `supervisor: trace stop rejects malformed success metadata` | A matching-sequence trace_stop success with wrong-typed metadata fields is returned as a clean error and keeps the live trace recording. |
| `supervisor: trace stop history survives child EOF` | A completed trace_stop publishes stopped history that child EOF must preserve instead of overwriting as lost. |
| `supervisor: shutdown drains a monitor after its entry was erased` | Hold EOF cleanup after a successful destroy; shutdown must retain the daemon until the erased entry's monitor departs. |
| `supervisor: fixture cleanup keeps the original child generation` | Cleanup of a reaped and then expired weak owner leaves a separately owned live replacement untouched; all owned processes drain before assertions. |
| `supervisor: duplicate create admission cannot spawn or replace an active id` | Hold admitted A before setup; B for the same ID is rejected without a child/workspace, while another ID reaches ready and serves commands. |
| `supervisor: failed create admission permits retry without publishing a placeholder` | Inject a preparation allocation failure after admission; public lookup stays empty and a later same-ID create succeeds. |
| `supervisor: unpublished create failure reaps its child before admission reuse` | An early pinned pidfd proves an unpublished child exits before failed-create reply and the next same-ID create. |
| `supervisor: released device node checks preserve a replacement generation` | A pinned original inode disappears while a new file occupies its old pathname; read-only teardown checks preserve the replacement. |
| `supervisor: destroy waits for reap after control EOF` | A live fake closes its channel while final cleanup is held; destroy cannot finish or kill it before release. |
| `supervisor: control EOF stays distinct from a confirmed reap` | EOF fires only the liveness event while the process is alive; actual collection publishes a durable exit code that late monitor lines cannot overwrite. |
| `supervisor: confirmed reap retries failed lifecycle notifications` | A native dispatch-allocation failure retains the actual reap and pending waiter; retry releases all lifecycle observers. |
| `supervisor: commit waits for reap before sealing` | A checkpoint exists before EOF, but commit waits for held final teardown and actual process exit before sealing. |
| `supervisor: failed blank create waits for reap before removal` | Failed mode-3 mkfs cleanup keeps its child alive through held cleanup and reaps it before removal. |
| `supervisor: shutdown waits for reap after control EOF` | Daemon shutdown retains its reaper until an EOF-reporting child completes held cleanup and actually exits. |
| `integration: supervisor reaps children before releasing ublk devices` | Privileged real-device create/destroy cycles, commit, failed mkfs cleanup and three-child shutdown leave no owned sysfs, block or character nodes. |
| `supervisor: shutdown drains an admitted create and its late monitor` | Hold an admitted create across shutdown; drain its handler and the monitor it spawns before returning. |
| `supervisor: shutdown drains an accept racing handler registration` | Hold an accepted socket before handler registration; cancellation and draining include the late handler. |
| `supervisor: shutdown wakes an idle accepted client` | An accepted peer sends no bytes and remains open; shutdown cancels the read and closes its stream after completion. |
| `supervisor: shutdown wakes a partial accepted command` | A peer sends an incomplete JSON line and remains open; shutdown completes without more peer traffic. |
| `supervisor: startup failure drains already admitted client work` | An injected failure after accept-loop startup cancels and drains the existing idle client before propagating. |
| `supervisor: client task failure preserves daemon shutdown drain` | A client body failure is logged without cancelling the daemon; its independent activity groups still drain on shutdown. |

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
- `supervisor: stale destroy leaves a recreated device generation registered`
  (integration, `tests/integration/test_commit.cpp`) — hold an admitted destroy
  on A while failed-mkfs cleanup removes A, then publish B under the same ID.
  Releasing the stale destroy must leave B registered with the same ready PID,
  able to serve a resize command and be destroyed normally. Both lifecycle
  removal paths compare the retained entry identity under the registry mutex;
  a string ID alone cannot authorize removing a newer generation.
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
  and `try_reap` transitions the child to `exited` with `exit_code` 0.
  This pins the fd-3 spawning contract end to end.
- `supervisor: exec failure surfaces as exit 127` — spawns a child whose
  binary does not exist and guards that the reaped `exit_code` is exactly
  127 and the status error contains "exec failed". This pins the exec-fail
  convention that operators and `cmd_create` rely on.
- `supervisor: reap evidence preserves actual child wait status` — real
  fork/exec exits distinguish exit zero, exit 134, SIGABRT and SIGKILL under
  partial-recovery, adopted-source and report-refusal fixture markers.
  A reported stopped state cannot commit the observation; actual collection
  does, and the durable record remains after Child destruction.
- `supervisor: unavailable reap evidence does not change child collection` —
  missing marker and exclusive-file collision for each fixture mode leave collection unchanged,
  never overwrite earlier evidence, and cannot produce a passing witness.
- `supervisor: failed child ownership setup leaves no process or channel` —
  injects an owner-allocation failure and a parent-side exception after fork;
  no returned `Child` is needed to ensure collection and channel closure.
- `supervisor: child ownership handoff retains the process and channel` —
  verifies the successful factory result owns its original PID, nonblocking
  status channel and copied ID, reports normal exit and closes the channel.
- `supervisor: spawn failure fixture skips signaling a collected process` —
  covers parent unwind without a retained process pin; fixture cleanup
  first checks collection and sends no signal to the retired PID.
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
- `supervisor: successful commit disables recovery respawn` (integration,
  `tests/integration/test_commit.cpp`) — with recovery attempts enabled, a
  successful commit stops and seals the child without publishing a recovery
  replacement; this pins the no-respawn `destroying` transition after
  pre-stop commit checks pass.
- `supervisor: rejected commits preserve crash recovery` (integration,
  `tests/integration/test_commit.cpp`) — commit refusals that happen before
  any intentional stop (sparse upper, upper-less/read-only config, and
  mode-3 host-mkfs upper) leave the live child running and do not set the
  no-respawn teardown flag; a later controlled child crash still consumes
  the remaining ADR-0010 recovery attempt.
- `supervisor: destroy is rejected while commit is admitted` (integration,
  `tests/integration/test_commit.cpp`) — a paused admitted commit rejects a
  concurrent `destroy` with the same busy reason used for concurrent commit,
  then the commit is allowed to finish and the device can be destroyed.
- `supervisor: commit is rejected while destroy is admitted` (integration,
  `tests/integration/test_commit.cpp`) — a paused admitted destroy rejects a
  concurrent `commit` with the destroy-in-progress reason, then the destroy
  is allowed to finish and removes the entry.
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
- `supervisor: trace start is rejected after shutdown admission closes` —
  the device-side command loop shares a shutdown admission gate with the
  device shutdown path, so a queued `trace_start` is rejected cleanly
  instead of arming a recorder after the shutdown stop barrier.
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
**unexpected ready-device** exit after actual reap (not a requested destroy,
shutdown, or an unresolved startup owner), respawns the child with `--recover --dev-id N`: the ublk device was
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

Shutdown first closes new-work and startup-grant admission. While a protected
startup owner remains, it retains the listener, clients, registry, monitors,
reaper and runtime for hello/list/status/explicit destroy. It reports
`shutdown_pending`, refuses other new work, and does not signal protected
children. Create reservations and recovery publication are accounted for
before final drainage. External reap without retirement evidence does not
clear this condition (ADR-0022).

After protected owners release, final shutdown cancels the listener's pending accept,
accepted clients' socket reads/writes, and forwarded device-channel writes.
An idle peer, a partial JSON line, or a device that stops reading therefore
needs no additional peer traffic to let those I/O operations finish. The
listener and each stream retain their descriptors through I/O completion.

Command admission occurs at the shutdown check after reading a complete
line. A connection accepted earlier, or even a line fully read before that
check, can receive EOF/reset without dispatch or a JSON reply when final
drainage has begun. Maintenance permits the observation/release commands above. Already admitted handlers remain owned until completion, which
may include cancellation or failure; draining them does not guarantee
successful command execution or response delivery.

The daemon drains accepted handlers first, with device monitors and the
SIGCHLD reaper still running: an already admitted command may finish a
create, spawn its monitor, or wait for a device reply or exit. It then stops
the stable set of registered children, waits for actual reaps (including
the ordinary 2 s post-KILL grace for eligible children), cancels remaining monitor reads, and
drains **all** admitted monitors, including those whose entry was removed
by `destroy`. Only after this work departs does it wake and drain the reaper.
Recovery publication still takes `op_mu` followed by the registry mutex;
shutdown takes `op_mu` before choosing the generation to terminate.

Accepts, clients, monitors and the reaper have separate Elio TaskGroups
under the ADR-0002 runtime rule. Each stage's join closes its admission and
waits for frame and callable destruction, not merely a task result or a
child's exit event. Startup/signal-wait exceptions follow the same ownership/maintenance path
before propagating; diagnostics and transient observation failures cannot
unwind the retained graph. Task body failures are logged promptly and do not
cancel siblings; admission failures are reported after the affected group
drains. The reaper remains placed on worker 0 using Elio affinity. Test
client/monitor bodies use workers 2/1 to exercise cross-worker ownership.
Elio resolves the current worker's I/O context for each `signal_fd` wait.

This is a completion guarantee, not a single global shutdown deadline.
Readiness, device-reply, stop, and host-mkfs waits retain their existing
bounds when reached; local file/seal operations must complete as usual.
Forwarded writes can finish with cancellation before the whole request has
been sent. The existing 30-second device-reply timeout begins only after a
complete send, so write cancellation must be available while handlers drain.
The daemon does not free shared state when a timeout expires while an owned
task can still access it.

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
