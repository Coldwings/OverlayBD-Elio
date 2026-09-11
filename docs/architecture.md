# Architecture

System-wide view of overlaybd-elio: what it is, how data moves through it,
how processes and threads are arranged, and which cross-module contracts are
load-bearing. Each module has its own document (linked below); this file is
the map.

## Overview

**overlaybd-elio** is an OverlayBD-compatible lazy-loading block-device
stack: it serves container images in the upstream OverlayBD on-disk formats
(ZFile-compressed, LSMT-indexed layers) as Linux block devices, fetching
blob data on demand from an OCI registry or local files instead of
downloading whole images up front. It is implemented in C++20 on the
[Elio](https://github.com/Coldwings/Elio) coroutine runtime, which is the
project's runtime foundation (ADR-0002): all IO on the image path is
async coroutine IO.

Design goals, and the decisions that fix them:

- **OverlayBD format compatibility.** Images built by upstream
  `overlaybd-*` tooling must load byte-identically. The ZFile and LSMT
  layouts are treated as a wire format, not an implementation detail.
- **ublk, not tcmu.** The block-device backend is the kernel ublk
  interface with per-queue io_uring uring-cmds (ADR-0003).
- **Per-device process isolation.** One `obd-device` child process serves
  exactly one block device, supervised by one `obd-supervisor` daemon; a
  device crash never takes down its siblings (ADR-0004).
- **DART as an optional external prefix proxy.** P2P acceleration is not
  an in-process engine; when enabled, blob requests are rewritten as
  `GET http://<dart>/<prefix>/<full upstream URL>`, with automatic
  fallback to direct registry reads (ADR-0005).
- **Read-first, with writable uppers.** The base stack is read-only; an
  optional writable upper layer (ADR-0008, which supersedes the
  read-only-only scoping of ADR-0007) turns a device into a
  copy-on-write writable disk without mutating the sealed lowers.

The shipped binaries are `obd-supervisor` (the daemon), `obd-device` (the
per-device server, spawned by the supervisor), `obdctl` (control CLI), and
`obd-mkimage` (test-image generator).

## Data Flow

### Read path

A read travels through four ownership boundaries:

1. **Kernel → queue thread.** The kernel places an IO descriptor in the
   queue's shared command buffer; the queue thread (one per ublk queue)
   picks it up via a `UBLK_IO_FETCH_REQ` uring-cmd on its private
   io_uring ring and decodes it into an `IoRequest`
   (`src/ublk/queue.hpp::IoRequest` — tag, op, start sector, sector
   count).
2. **Queue thread → Elio scheduler.** The request is pushed onto the
   queue's pending deque and the bridge is woken through the `elio_efd`
   eventfd. The bridge coroutine (`src/ublk/elio_bridge.cpp`, see
   `run_bridge`) pops pending requests and spawns one coroutine per tag,
   so per-tag IO overlaps; completion ordering across tags is irrelevant
   to ublk.
3. **Elio → image stack.** Each IO coroutine calls
   `BlobSource::pread` on the device root (`src/source/blob_source.hpp::BlobSource`).
   The root is either a `MergedLsmt` (read-only image) or a
   `MergedWritable` (image with a writable upper). Reads walk the layer
   chain top-down: each `LsmtLayer` resolves its segment index and falls
   through holes to the layer below; a layer's backing view is a
   `ZFileSource` when the blob is ZFile-compressed (transparent
   decompression), wrapped over a `TarOffsetSource` (ustar wrapper
   stripping), over the raw blob source — a `LocalFileSource` for
   already-local blobs, or a `RegistrySource` (HTTP range reads with
   bearer auth) behind a `LayerStore` (ADR-0011: sparse-file read-through
   persistence per layer; extents already persisted are served locally,
   and a completed layer is renamed to `overlaybd.commit` for the local
   probe to bind on the next open) — or, for a layer without a configured
   `dir`, served remote-only (ADR-0016). Every remote fetch of every
   lower passes one per-device read admission funnel (ADR-0012):
   guest-blocking misses are admitted unconditionally, while prefetch and
   background fill are scavenger classes gated by an AIMD concurrency
   window (see `docs/source.md`). When
   DART is enabled and reachable, the registry client's requests go
   through the DART prefix proxy instead (ADR-0005). This assembly is
   built once at open time in `open_image` (see `src/image/image_file.cpp`).
4. **Elio → queue thread → kernel.** The IO coroutine pushes the result
   (bytes transferred, or a negative errno) onto the done deque and wakes
   the queue thread via `done_efd`; the queue thread issues
   `UBLK_IO_COMMIT_AND_FETCH_REQ` to complete the request and fetch the
   next one. Short reads at end-of-blob are zero-padded so the device
   always answers the full request length.

### Write path (ADR-0008)

Writes exist only when the image config carries a writable `upper`. The
device root then implements `WritableBlobSource`
(`src/source/blob_source.hpp::WritableBlobSource`), and the bridge
dispatches `UBLK_IO_OP_WRITE` to `pwrite` and `UBLK_IO_OP_FLUSH` to
`flush`. `MergedWritable` resolves each write against the upper layer
first: data already covered by the upper is overwritten in place;
previously-uncovered ranges are copy-on-write — new data lands in the
upper and shadows the sealed lowers, which are never mutated. The upper
is either an in-place-edit unsealed LSMT file (`<dir>/overlaybd.rw`) or
a fiemap sparse file (`<dir>/overlaybd.sparse`); see `docs/config.md`.
On a read-only device the root does not implement `WritableBlobSource`:
`WRITE`, direct `DISCARD`, and direct `WRITE_ZEROES` return `-EROFS`, while
read-only ublk params do not advertise discard/write-zeroes limits so the
kernel normally never issues them. On a writable device `DISCARD` and
`WRITE_ZEROES` are advertised and dispatched to the writable root (ADR-0009).
`WRITE_SAME` and unknown operations are never advertised and are rejected
defensively with `-EOPNOTSUPP`.

## Process Model

One `obd-supervisor` daemon owns the machine-facing control plane; one
`obd-device` child process per block device owns the data plane
(ADR-0004). The two communicate over two wire protocols (both JSON-lines,
both stability-contracted):

- **Control channel (UDS).** The supervisor listens on a unix domain
  socket (default `/run/overlaybd-elio/supervisor.sock`). `obdctl` (or
  any client) sends one JSON command per connection — `create`,
  `destroy`, `list`, `status` — and receives one JSON reply. `create`
  takes a device id and a per-image `config.json` path; the supervisor
  validates the id, spawns the child, and waits (bounded by
  `ready_timeout_sec`) for the child to report `ready` or `failed`
  before replying.
- **Status channel (fd 3).** `Child::spawn` (`src/supervisor/child.cpp::Child`)
  creates a `socketpair`, forks, `dup2`s the child end onto fd 3, clears
  `FD_CLOEXEC`, and `execve`s `obd-device`. The child reports lifecycle
  states (`ready`, `failed`, …) as JSON-lines on that inherited fd; the
  supervisor's monitor coroutine parses them and EOF marks the process
  gone. A `SIGCHLD`-driven reaper fills in exit codes. A failed `execve`
  surfaces as exit code 127.

Shutdown is ordered: on `SIGTERM`/`SIGINT` the supervisor stops
accepting, terminates every child, waits up to `stop_timeout_sec` per
child, and SIGKILLs stragglers. There is no shared memory and no fd
passing between devices; one device's crash, hang, or corrupt image
cannot affect its siblings.

## Thread Model

Two thread families meet only at the documented bridge (ADR-0006):

- **ublk queue threads** (one per queue per device) each own one
  io_uring ring *exclusively*: only the queue thread submits
  FETCH/COMMIT uring-cmds, because the kernel pins `io->task` at FETCH
  time and requires the COMMIT from the same task. The queue thread also
  parks a `POLL_ADD` on `done_efd` in its ring, so Elio-side completions
  wake its io_uring wait without a separate poll thread.
- **Elio scheduler threads** run everything else: the bridge coroutine,
  one IO coroutine per in-flight tag, the whole image/source stack, HTTP,
  downloads, and (in the supervisor) the accept loop, monitors, and
  reaper. The Elio scheduler never submits to a queue ring.

Crossing the boundary is message-passing only: requests travel
queue-thread → Elio via the pending deque plus `elio_efd`; completions
travel Elio → queue-thread via the done deque plus `done_efd`. Both
deques are guarded by a short mutex section (`src/ublk/queue.hpp::Queue`).
The per-tag IO buffers live in the device process's vm space; the kernel
copies request data in and out of them, and Elio workers write read
payloads into them before completing the tag.

## Module Map

### common — `docs/common.md`

Shared utilities with no dependencies on the rest of the tree: error
types (`obd::error`, `obd::format_error`), logging glue, SHA-256 and
CRC32C digests, and little-endian byte-range helpers including the
OverlayBD `segment_mapping` bit layout. Everything here is pure and
reentrant; these helpers pin the exact encodings the format module
relies on (e.g. the OverlayBD raw running CRC). Complexity notes and
boundary behavior are documented per function in the module document.

### format — `docs/format.md`

Readers (and test-fixture writers) for the OverlayBD on-disk formats:
ZFile (compressed, jump-table indexed) and LSMT (log-structured merge
tree layers), plus `MergedLsmt`, which merges N sealed layers into one
read-only block view with hole fall-through. Since ADR-0008 the module
also hosts the writable layers: `LsmtRwLayer` (unsealed, in-place-edit
LSMT with `seal()` compaction), `SparseRwLayer` (fiemap sparse file),
and `MergedWritable` (copy-on-write merge of a writable upper over
sealed lowers). Header parsing is byte-exact against the upstream wire
format; fixture writers exist only to produce test images. Any change
to parsing semantics is breaking for image compatibility.

### source — `docs/source.md`

Pluggable blob sources behind the single async `BlobSource` interface:
`LocalFileSource`, `RegistrySource` (OCI registry HTTP range reads,
bearer-token auth, redirect handling), `LayerStore` (sparse-file
read-through layer persistence with sidecar bitmap, per-extent CRC, and
background fill — the remote-layer backing in image assembly, ADR-0011),
`TarOffsetSource`
(ustar wrapper detection), the DART prefix proxy helpers (ADR-0005), and
the `CredentialStore` (longest-prefix registry credential matching).
`WritableBlobSource` extends the interface for writable device roots
(ADR-0008). The module's pread contract — positional, no mid-blob short
reads, negative-errno errors, fixed size — is what every layer above
relies on.

### image — `docs/image.md`

Assembly: parses the global `overlaybd.json` and the per-image
`config.json` (overlaybd-snapshotter compatible) and turns them into an
opened device root. `open_image` resolves credentials, probes DART
reachability, picks local-or-remote per lower, stacks tar/ZFile/LSMT
views, and attaches either a `MergedLsmt` or, when `upper.dir` is set, a
`MergedWritable` with the configured upper type. Configuration semantics
(field names, defaults, compatibility rules) are contracted in
`docs/config.md`.

### ublk — `docs/ublk.md`

The data plane: the control channel (`Ctrl`, device lifecycle over
`/dev/ublk-control`, parameter setup), the per-queue data plane
(`Queue`, the ADR-0006 ring-ownership boundary), the Elio bridge, and
`uapi_compat.hpp` — the only place where kernel-header gaps are filled,
via `#ifndef`-guarded additions over `<linux/ublk_cmd.h>`. Device
parameters advertise exactly the operations the image stack implements;
everything else is rejected defensively. The module owns the uapi
compatibility policy: never redefine what the kernel header provides.

### supervisor — `docs/supervisor.md`

The process model: the daemon (`src/supervisor/daemon.cpp::Daemon`),
the child lifecycle (`Child::spawn` — pre-fork argv construction,
async-signal-safe fork section, fd-3 status channel, exec-failure
convention), and the two JSON-lines wire protocols (control commands on
the UDS, device status on fd 3). Both protocols are wire contracts
between binaries that may be upgraded independently. The supervisor
keeps no per-device IO state; it supervises processes, not data.

### binaries — `docs/binaries.md`

`obd-supervisor` runs the daemon loop; `obd-device` is spawned per
device with `--config`, optional `--global`, `--control-fd 3`, and
optional `--dev-id`, and serves one image on one ublk device
(`/dev/ublkb<N>`); `obdctl` speaks the control protocol with plain
blocking IO (no Elio runtime); `obd-mkimage` generates OverlayBD-format
test images for fixtures and golden tests. Usage, flags, and behavior
guarantees live in the binaries document.

## Concurrency & Call Permissions

Cross-module rules every contributor must hold (module documents add
their own local rules):

- **Coroutine context.** Everything reachable from the read/write path —
  the whole `source`, `format`, and `image` stack — runs as Elio
  coroutines and must only be called on an Elio scheduler thread.
  `BlobSource::pread`/`pwrite`/`flush` are coroutines; blocking a
  coroutine on a synchronous syscall is forbidden — use the Elio IO
  backend. Setup/cold paths may throw exceptions; hot IO paths return
  negative errnos and never throw.
- **Ring ownership (ADR-0006).** Only the queue thread submits to its
  queue's io_uring ring. Elio threads touch a `Queue` exclusively
  through `try_pop_request`, `push_completion`, `io_buf`, and
  `elio_efd`. No other cross-thread use of a `Queue` is permitted.
- **Positional, stateless reads.** `pread` never mutates shared read
  state, so concurrent IO coroutines against one source are safe unless
  a concrete type documents otherwise. Writable layers serialize their
  own internal mutation; callers still issue positional, sector-aligned
  IO.
- **fd ownership across fork (ADR-0004).** Between `fork` and `execve`
  the supervisor child runs only async-signal-safe calls; all argv
  memory is allocated pre-fork. The status socketpair is `SOCK_CLOEXEC`
  on the parent side and explicitly cleared on the child's fd 3 so it
  survives `execve`. No other fds leak into device processes.
- **Buffer ownership.** ublk per-tag IO buffers are owned by the
  `Queue`; an IO coroutine may write a read payload into its tag's
  buffer between popping the request and pushing the completion, and
  must not touch it afterwards.

## Stability Contract

These cross-module contracts are load-bearing; changing any of them is
breaking and requires an ADR (trigger T1, see `docs/adr/README.md`):

- **ZFile and LSMT on-disk layouts** — magic numbers, header fields,
  index encodings, compression framing. Images built by upstream
  `overlaybd-*` tools must keep loading byte-identically. Pinned by
  golden-header tests such as `format: zfile header bytes match the OverlayBD wire format`.
- **Supervisor wire protocols** — the obdctl↔supervisor control protocol
  (JSON-lines commands `create`/`destroy`/`list`/`status`, one command
  per UDS connection, `ok`/`error` replies) and the supervisor↔obd-device
  lifecycle protocol (JSON-lines status on fd 3; exit 127 on exec
  failure). Both sides may be upgraded independently.
- **`config.json` semantics** — compatibility with overlaybd-snapshotter
  output is an operator contract: unknown fields are ignored, known
  fields keep their overlaybd meaning. The full field reference and
  compatibility rules are `docs/config.md`.
- **ublk uapi compatibility** — structures come from
  `<linux/ublk_cmd.h>` (kernel ABI); `src/ublk/uapi_compat.hpp` only
  adds what a given header version lacks, guarded by `#ifndef`.

## Testing

Strategy, naming convention, golden-value policy, and the full test
inventory live in **`docs/testing.md`**. Representative tests for the
contracts above:

- `format: zfile header bytes match the OverlayBD wire format` and
  `format: lsmt header bytes match the OverlayBD wire format` — golden
  wire-format bytes.
- `format: merge falls through holes to lower layers` and
  `format: merged writable falls through and copy-on-writes` — merge and
  ADR-0008 write semantics.
- `image: assembly from local layer files reads merged content` — the
  `open_image` assembly chain end to end.
- `ublk: command buffer geometry matches the driver layout` — uapi
  geometry against the kernel header.
- `supervisor: child spawn execs and reports through the channel` — the
  fd-3 lifecycle protocol.
- `integration: ublk device serves sector reads from a blob` — the
  privileged end-to-end path (self-skips without `/dev/ublk-control`).

## Limitations & TODO

- **TurboOCI** (the third upstream on-disk format) is not supported;
  deferred.
- **Crash recovery is bounded, not transparent persistence.** Devices are
  created with ublk `USER_RECOVERY` when `ublkConfig.enableRecovery` is
  true (the default) and the kernel supports it; the supervisor respawns
  a crashed child in recovery mode up to `max_recovery_attempts`
  (ADR-0010). Older kernels whose `ADD_DEV` call rejects the
  recovery flags with `EINVAL` degrade to a non-recoverable device at
  create time. Recovery reassembles the image
  from restart-recoverable inputs: sealed lowers and sparse writable
  extents can be reopened, but an unsealed LSMT-RW upper is not reopened
  by recovery even if graceful shutdown wrote a checkpoint.
- **Prefetch** covers only the active populate paths: structural
  head/tail warm-up (ADR-0012) and trace replay (the upstream trace blob
  IS replayed through `populate` when the image config marks an
  `accelerationLayer`, ADR-0013 — see `docs/image.md`). Those two paths
  are admitted at the device's ADR-0012 funnel as the Prefetch scavenger
  class, and the `prefetch` config section's `enable` switch is honored
  for both. LayerStore background fill is a separate ADR-0012 Fill
  scavenger class behind Prefetch and is governed by the `download`
  config. Trace recording is a supervisor command path (ADR-0013; see
  `docs/supervisor.md`) that passively records fully satisfied remote
  reads at the trace tap; it does not call `populate` or use the Prefetch
  class, and issue #33 tracks filtering recordings down to OnDemand reads
  only.
- **Discard / punch-hole** reaches only writable devices (ADR-0009).
  Read-only images do not advertise discard limits, so the kernel never
  issues discard/write-zeroes to them. Writable LSMT-RW uppers satisfy the
  ADR-0009 mask contract with zeroed segments. Sparse uppers satisfy the
  same merged-view mask by punching top-file holes and recording sidecar
  zero-mask metadata that `flush()`/`checkpoint()` persists.
- **LSMT-RW durability:** an unsealed `overlaybd.rw` upper keeps its
  segment index in memory only; unsealed data is **not restart-recoverable**
  as a writable upper. A graceful obd-device shutdown checkpoint is the
  input to the supervisor's offline `commit` (ADR-0014), not a reopen path;
  a later image open creates/truncates a fresh unsealed LSMT-RW file. Only
  `seal()` / offline commit compacts the data into a standard sealed LSMT
  layer.
