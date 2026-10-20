# ublk — kernel control plane, per-queue data plane, Elio bridge

## Overview

The `ublk` module is the data plane of overlaybd-elio: it exposes one merged
OverlayBD block view as a real Linux block device (`/dev/ublkb<N>`) through
the kernel **ublk** driver (the ADR-0003 decision: ublk, not tcmu). It has
three layers:

- **Control plane** (`src/ublk/ctrl.hpp`) — synchronous `/dev/ublk-control`
  commands driving the device lifecycle: `ADD_DEV` → `SET_PARAMS` → (queues
  park their FETCH commands) → `START_DEV` → … → `STOP_DEV` → `DEL_DEV`.
  Control commands are `IORING_OP_URING_CMD` on the control fd (with
  `IORING_SETUP_SQE128`, which the driver requires) — ublk-control has
  never had an `unlocked_ioctl` handler; libublksrv works the same way.
- **Per-queue data plane** (`src/ublk/queue.hpp`) — one `Queue` per hardware
  queue, each owning a raw liburing ring that speaks
  `UBLK_IO_FETCH_REQ` / `UBLK_IO_COMMIT_AND_FETCH_REQ` uring-cmds to
  `/dev/ublkc<N>`, plus the kernel-shared command buffer (mmap) and per-tag
  IO buffers.
- **Elio bridge** (`src/ublk/elio_bridge.hpp`) — coroutines that take
  requests off a queue, serve them from the merged `BlobSource` stack, and
  hand completions back to the queue thread (ADR-0006: queue rings belong to
  queue threads; Elio bridges by completion).

`src/ublk/uapi_compat.hpp` is the module's single view of the kernel uapi:
it includes `<linux/ublk_cmd.h>` and adds — `#ifndef`-guarded — only what a
given header version lacks, per the AGENTS.md rule that we never redefine
what the kernel header provides. Baseline: kernel headers >= 6.0.

One `Device` (`src/ublk/device.hpp`) ties the three layers together for one
image inside one `obd-device` process (ADR-0004 isolation).

## Concepts

### Queue rings and the ADR-0006 ownership rule

Every hardware queue has exactly one **queue thread**, and that thread owns
its io_uring ring *exclusively*: only it submits `FETCH` / `COMMIT_AND_FETCH`
uring-cmds. This is not a style choice — the kernel pins `io->task` when a
`FETCH_REQ` is issued and requires the matching `COMMIT_AND_FETCH_REQ` to
come from the same task. The Elio scheduler therefore **never submits** to a
queue ring; the two worlds exchange work through two mutex-guarded deques
plus eventfds (see `src/ublk/queue.hpp`):

```
queue thread --(pending_ + elio_efd)--> Elio bridge coroutines
Elio workers --(done_ + done_efd)-----> queue thread
```

The queue thread parks a `POLL_ADD` on `done_efd` inside its own ring, so
Elio-side completions wake its `io_uring_submit_and_wait` without a separate
poll thread.

`Device::create` and `Device::attach` open `/dev/ublkc<N>` once for the
device: the driver's open exclusion applies to the whole device, including
all hardware queues. The first `Queue` owns that descriptor; each additional
queue owns a close-on-exec duplicate sharing its open file description.
Every queue still has its own command-buffer mapping, tag buffers, eventfds,
ring and issuer thread. Queue destruction closes only its owned descriptor;
the existing mapping and command lifetime barriers still apply. Final kernel
file release waits for all references, including mappings and accepted
io_uring commands. Recovery opens the first descriptor only after
`START_USER_RECOVERY` succeeds.

### Command buffer mmap geometry

The kernel shares per-tag `ublksrv_io_desc` records with the daemon through
an mmap of the queue's char device. The geometry is pinned by compile-time
asserts and helpers in `src/ublk/uapi_compat.hpp`:

- `sizeof(ublksrv_io_desc) == 24`, with `op_flags` at offset 0,
  `nr_sectors` at 4, `start_sector` at 8, `addr` at 16 (static_asserts — a
  future header that changes these breaks the build loudly instead of
  corrupting the ring protocol);
- `sizeof(ublksrv_io_cmd) == 16` (fits the 16-byte uring-cmd area);
- `UBLK_MAX_QUEUE_DEPTH == 4096`;
- per-queue mapping **stride**: `q_id * round_up(4096 * 24, 4096)` =
  `q_id * 98304` (matches `ublk_ch_mmap` / `ublk_max_cmd_buf_size` in the
  driver);
- per-queue mapping **length**: `round_up(depth * 24, 4096)` — it must match
  exactly or the driver rejects the mapping.

The mapping is `PROT_READ | MAP_SHARED | MAP_POPULATE`: the descriptor page
is written by the kernel, read by us. Request payloads live in daemon-owned
per-tag IO buffers (`aligned_alloc(4096, depth * max_io_buf_bytes)`), whose
addresses are passed to the kernel in each uring-cmd; the kernel copies data
in (writes) and out (reads).

### Tag lifecycle

1. The queue thread parks one `UBLK_IO_FETCH_REQ` per tag. The driver
   refuses `START_DEV` with `EBUSY` until every tag of every queue is
   parked — `Device::create` therefore starts queue threads first and
   retries `START_DEV`.
2. A completed FETCH cqe means tag `t`'s `ublksrv_io_desc` in the shared
   window is now valid; the queue thread extracts an `IoRequest`
   (op, start_sector, nr_sectors) and pushes it to `pending_`, then writes
   `elio_efd`.
3. A bridge coroutine pops the request, serves it from the block source
   (each tag in its own coroutine, so per-tag IO overlaps; ublk does not
   care about cross-tag completion ordering), and calls
   `Queue::push_completion(tag, result)`, which appends to `done_` and
   writes `done_efd`.
4. The queue thread's parked poll wakes; it drains `done_` and issues
   `UBLK_U_IO_COMMIT_AND_FETCH_REQ` per finished tag — committing the result
   *and* re-parking the fetch for the next IO in one command.

`user_data` encoding on the ring (`src/ublk/queue.cpp`): bit 63 = done_efd
poll marker, bit 62 = commit cqe, low 16 bits = tag.

### Op dispatch table

The bridge (`src/ublk/elio_bridge.cpp`, function `handle_io`) implements the
device's op policy:

| ublk op | Behavior |
|---|---|
| `READ` | `BlobSource::pread` into the tag's IO buffer; short reads at EOF are **zero-filled** so the device always answers the full request; success result = requested length. A miss that reaches a remote source is admitted at the device's read admission funnel as the unconditional OnDemand class (ADR-0012 — the funnel lives in the source chain below, see `docs/source.md`). |
| `WRITE` | If the root source is a `WritableBlobSource` (writable image, ADR-0008): `pwrite` from the tag's IO buffer; success result = requested length. Otherwise **-EROFS**. |
| `FLUSH` | If writable root: `WritableBlobSource::flush()` (0 or -errno). Otherwise immediate **0** (read-only device, nothing to persist). |
| `DISCARD` | If writable root: `WritableBlobSource::discard()`; writable uppers provide ADR-0009 mask-with-zeroes semantics. LSMT-RW records zeroed segments, while sparse uppers publish a protective sidecar zero map before best-effort hole punching. Success result = 0. Otherwise **-EROFS**. |
| `WRITE_ZEROES` | If writable root: `discard()` — or, with `UBLK_IO_F_NOUNMAP`, a real write of zeroes (no deallocation). Otherwise **-EROFS**. |
| `WRITE_SAME` | **-EOPNOTSUPP** (never advertised via params; rejected defensively). |
| anything else | **-EOPNOTSUPP**. |

`SET_PARAMS` advertises BASIC with `UBLK_ATTR_VOLATILE_CACHE` (plus
`UBLK_ATTR_READ_ONLY` when `DeviceParams::read_only`), `io_min` = logical
block shift, `io_opt` = physical block shift. For writable devices it adds
the DISCARD parameter type with sector granularity (matching the layer
contract) and `max_discard_sectors`/`max_write_zeroes_sectors` =
`max_sectors`, one segment; for read-only devices all discard limits stay
0 so the kernel never issues the commands (ADR-0009).

### Crash recovery (ADR-0010)

Devices are created with `UBLK_F_USER_RECOVERY |
UBLK_F_USER_RECOVERY_REISSUE` when `DeviceParams::enable_recovery` (the
default, driven by `ublkConfig.enableRecovery`): the kernel then keeps the
device QUIESCED when its server process dies and reissues outstanding I/O
to a replacement. `Ctrl::start_user_recovery` / `end_user_recovery` drive
the handshake and `Device::attach` is the replacement-side path:
`START_USER_RECOVERY`, open `/dev/ublkcN`, re-park FETCH for every tag on
every queue, then `END_USER_RECOVERY`. Both handshake steps retry `EBUSY`,
but wait for different conditions:

- `START_USER_RECOVERY` waits for the old server's cdev to close and for
  the driver to enter **QUIESCED**, using one 30-second monotonic retry
  budget and waits of at most 50 ms. Process death/reaping does not imply
  that the driver's asynchronous quiesce has finished: on Linux 6.6 its
  periodic death detection can delay readiness by about five seconds.
  The 30-second budget is an application policy allowing margin for that
  work and scheduler load, not a kernel-mandated timeout. Command time
  and timer delays consume the same budget; an already-running control
  command is not preempted. The replacement must not open any queue cdev
  before START succeeds, as an open cdev itself makes START return `EBUSY`.
- `END_USER_RECOVERY` waits until every replacement queue tag has a parked
  FETCH, retrying up to 100 times with 50 ms waits, like `START_DEV`.

Other errors fail immediately; exhausting START's budget reports
`ublk START_USER_RECOVERY not ready after 30s`. The replacement adopts
cleanup ownership only after END succeeds, so a failed START does not
delete the existing kernel device. It completes all fallible metadata
preparation (GET_PARAMS, path formatting and the preparation log) before
END. Successful END is followed immediately by nonthrowing adoption and
return, so a caller's fallback cleanup guard can hand off ownership when
attach returns without retaining an id already deleted by failed adopted
setup. The preparation log does not announce recovery success; readiness
is published after attach returns. `ADD_DEV`
falls back to a non-recoverable device (with a warning) when the kernel
rejects the flags with `EINVAL`.

### Online resize (D3)

A live device can grow through `Ctrl::update_size` (D3):
`UBLK_U_CMD_UPDATE_SIZE` tells the driver the new capacity — the size
rides `ublksrv_ctrl_cmd.data[0]`, in 512-byte sectors (kernel ABI,
ublk_cmd.h) — and the driver updates the gendisk capacity. The command
landed in the 6.16 development cycle, so a driver without it rejects
the command: pre-6.15 kernels answer `ENOTSUPP` (524, the control
dispatch default), 6.15+ kernels answer `EOPNOTSUPP` (95) — either is
surfaced as a clean error. The command is
**grow-only by contract**: `Device::resize_blocking` rejects a request
at or below the current size before any kernel IO, and the supervisor's
`resize` executor enforces the same rule against the device's tracked
size (docs/supervisor.md). For a WRITABLE image the executor grows the
data plane (merged view + writable top; docs/format.md) BEFORE the
kernel call, so headroom writes land in the upper; this layer module
itself only changes the kernel gendisk. Like every control call it is a
blocking cold-path call: `Device::resize_blocking` must be run through
`elio::spawn_blocking` (the device command loop does so), never on an
Elio worker.

**Recovery baseline (D3).** `Device::attach` seeds the grow-only
baseline (`size_bytes()`) from the kernel's REAL current capacity via
`Ctrl::get_params`, not from the create-time `DeviceParams`: a grown
device keeps its capacity across USER_RECOVERY (the driver never resets
it, and attach issues no SET_PARAMS/UPDATE_SIZE), so a params-derived
baseline would let a post-recovery resize pass the grow-only check and
actually shrink the gendisk. GET_PARAMS returns the same
`basic.dev_sectors` the driver updated at UPDATE_SIZE time. The read takes
place before END while the device is still quiesced; END changes neither
that capacity nor the parameters.

## Public API

### `src/ublk/uapi_compat.hpp`

- `#ifndef UBLK_F_URING_CMD_COMP_IN_TASK` → defined as `(1ULL << 1)` for
  pre-6.0 headers.
- `#ifndef UBLK_U_CMD_UPDATE_SIZE` → defined as the ioctl-encoded
  command (D3 online resize; added to the uapi in the 6.16 cycle) for
  older headers — same shape as the in-header definition.
- `constexpr uint64_t obd::ublk::cmd_buf_stride(uint32_t io_desc_size)` —
  per-queue mmap window stride: `round_up(UBLK_MAX_QUEUE_DEPTH *
  io_desc_size, 4096)`.
- `constexpr uint64_t obd::ublk::cmd_buf_size(uint16_t depth, uint32_t
  io_desc_size)` — mmap length for one queue: `round_up(depth *
  io_desc_size, 4096)`. Must match the driver's expectation exactly.
- Compile-time ABI pinning: `static_assert`s on `ublksrv_io_desc` size (24)
  and field offsets, `ublksrv_io_cmd` size (16), and
  `UBLK_MAX_QUEUE_DEPTH` (4096).

### `src/ublk/ctrl.hpp` — `obd::ublk::DeviceParams`, `obd::ublk::Ctrl`

`DeviceParams` (plain struct; defaults in parentheses): `dev_id`
(`UINT32_MAX` = driver picks), `nr_queues` (1), `queue_depth` (128),
`max_io_buf_bytes` (128 KiB), `dev_sectors` (0 — must be set: image virtual
size / 512), `logical_bs_shift` (9 = 512 B), `physical_bs_shift` (12 = 4 K),
`max_sectors` (256 = 128 KiB per request), `read_only` (true →
`UBLK_ATTR_READ_ONLY`).

`Ctrl` — one instance per device process; owns the `/dev/ublk-control` fd
  and the SQE128 control ring
and remembers the added device for best-effort cleanup. Non-copyable.

- `Ctrl()` — opens `/dev/ublk-control` (`O_RDWR | O_CLOEXEC`); throws
  `obd::error` when unavailable (driver not loaded / no permission).
- `~Ctrl()` — if a device was added: best-effort `STOP_DEV` + `DEL_DEV`,
  then closes the fd. Never throws.
- `uint32_t add_dev(const DeviceParams&)` — `UBLK_U_CMD_ADD_DEV` with
  `UBLK_F_URING_CMD_COMP_IN_TASK` and `ublksrv_pid = getpid()`; returns the
  driver-assigned device id. Throws `obd::error`.
- `ublksrv_ctrl_dev_info get_dev_info(uint32_t dev_id)` —
  `UBLK_U_CMD_GET_DEV_INFO`, returning the kernel's actual device geometry,
  state and feature flags. Query `flags` to determine negotiated recovery
  capability; requested params or block-node existence cannot establish it.
  Does not adopt cleanup ownership. Throws `obd::error`; run off-worker
  from coroutine code, like the other blocking control calls.
- `void set_params(uint32_t dev_id, const DeviceParams&)` —
  `UBLK_U_CMD_SET_PARAMS` with BASIC | DISCARD types (see the params table
  above). Throws `obd::error`.
- `void start_dev(uint32_t dev_id)` — `UBLK_U_CMD_START_DEV` with this
  process as the ublk server (pid passed in `data[0]`). Throws `obd::error`;
  the kernel answers `EBUSY` until every queue tag has a parked FETCH —
  callers poll (see `Device::create`).
- `void update_size(uint32_t dev_id, uint64_t sectors)` — D3 online
  resize: `UBLK_U_CMD_UPDATE_SIZE` with the new capacity (sectors) in
  `data[0]`. Throws `obd::error` (a kernel without the command returns
  `ENOTSUPP`/524 on pre-6.15, `EOPNOTSUPP`/95 on 6.15+). Blocking; see
  the spawn_blocking rule below.
- `ublk_params get_params(uint32_t dev_id)` — `UBLK_U_CMD_GET_PARAMS`;
  `basic.dev_sectors` is the kernel's current capacity in sectors
  (which, after an online grow, may exceed the create-time params).
  Throws `obd::error`. Blocking; see the spawn_blocking rule below.
- `void stop_dev(uint32_t) noexcept`, `void del_dev(uint32_t) noexcept` —
  best-effort teardown commands; errors are deliberately ignored.
- `static std::string cdev_path(uint32_t)` → `/dev/ublkc<N>`;
  `static std::string bdev_path(uint32_t)` → `/dev/ublkb<N>`.

All `Ctrl` commands are blocking cold-path calls. Coroutine callers
(`Device::create` / `Device::attach`) run them through
`elio::spawn_blocking`: kernel control commands may sleep on our own
data plane — START_DEV's `add_disk` partition scan issues device reads
serviced by the bridges, and END_USER_RECOVERY waits for reissued IO.
A synchronous control call on a scheduler worker deadlocks it (the
worker stalls, the bridge coroutine never runs). Never invoke them on
a coroutine hot path.

### `src/ublk/queue.hpp` — `obd::ublk::IoRequest`, `obd::ublk::Queue`

`IoRequest` — one command extracted from a shared `ublksrv_io_desc`:
`tag`, `op` (`UBLK_IO_OP_*`), `start_sector`, `nr_sectors`;
`byte_offset() = start_sector * 512`, `byte_len() = nr_sectors * 512`.

`Queue(uint32_t dev_id, uint16_t q_id, uint16_t depth, uint32_t
max_io_buf_bytes)` — throws `obd::error(EINVAL)` for `depth == 0` or
`depth > UBLK_MAX_QUEUE_DEPTH`. Non-copyable; destructor tears down ring,
mapping, buffers, fds.

- `void open(int shared_cdev_fd = -1)` — opens `/dev/ublkc<N>` when the
  argument is negative; otherwise owns a `F_DUPFD_CLOEXEC` duplicate of the
  borrowed descriptor without closing or adopting that argument. The caller
  keeps the borrowed descriptor valid through this call. Mmaps the command buffer at
  `q_id * cmd_buf_stride(...)` with length `cmd_buf_size(depth, 24)`
  (PROT_READ, MAP_SHARED | MAP_POPULATE), allocates and zeroes the per-tag
  IO buffers, creates the two eventfds (`EFD_CLOEXEC | EFD_NONBLOCK`), and
  leaves ring creation to `run()` on the issuing queue thread. Throws
  `obd::error`.
- `uint16_t q_id() const`, `uint16_t depth() const`.
- `int cdev_fd() const noexcept` — borrows the queue's owned descriptor for
  subsequent queue setup; returns `-1` before descriptor acquisition. Do not
  close it or retain it past Queue destruction;
  duplicate it when independent ownership is needed.
- `void* io_buf(uint16_t tag) noexcept` — the tag's kernel-shared payload
  buffer (`tag * max_io_buf_bytes` into the allocation). Read handlers write
  into it; write handlers read from it. No bounds check — `tag < depth` is a
  kernel-guaranteed precondition.

Elio side (any Elio worker thread):

- `int elio_efd() const` — eventfd signaled when `pending_` is non-empty;
  register it with the Elio IO backend.
- `bool try_pop_request(IoRequest& out)` — pops one pending request; false
  when empty. Short mutex section; safe against the queue thread.
- `void push_completion(uint16_t tag, int32_t result)` — completes a
  request: `result >= 0` = bytes transferred, `< 0` = `-errno`. Wakes the
  queue thread via `done_efd`.
- `void fail(int err) noexcept` — publishes the failure errno and sticky
  flag, then wakes the queue thread through the same `done_efd`. A completion
  that cannot be enqueued can therefore end the ring wait without another
  request or a device stop.

Queue thread side:

- `void run(std::atomic<bool>& stop)` — the blocking queue loop: parks the
  per-tag FETCH commands, then waits on the ring, dispatching new requests
  to `pending_` and committing finished ones. Returns when `stop` becomes
  true (call `wakeup()` to interrupt the wait) or when the ring fails
  (check `failed()`).
- `void wakeup() noexcept` — interrupts the `run()` wait.
- `bool failed() const`, `int failure() const` — sticky ring-failure flag
  and errno. `-ENODEV` / `-EINTR` / `-ECANCELED` cqes are treated as normal
  teardown, not failures.

### `src/ublk/elio_bridge.hpp` — `obd::ublk::run_bridge`

```cpp
elio::coro::task<void> run_bridge(Queue* q, source::BlobSource* src,
                                  std::atomic<bool>* stop);
```

Runs the bridge loop for one queue against the block source until `stop`
becomes true (or the efd watch errors out on teardown). Launch an independent
joinable task from the device process; one bridge per queue. Waits on
`q->elio_efd()` and drains `try_pop_request`. On a tag's first request, it
admits one long-lived handler through native TaskGroup and binds that handler
to the bridge's worker. Later requests for that tag use a native event rather
than another scheduler admission. Tags remain independent: a source await on
one tag does not serialize the other tags (ADR-0021).
`-EINTR` / `-EAGAIN` on the eventfd read are retried.
Each bridge owns its tag handlers in an Elio collect-all TaskGroup and
joins them before returning. Ordinary stop wakes the bridge without
cancelling admitted source IO; parked handlers are signaled to leave. Each
handler checks the sticky close predicate before parking again, so a pending
request and close notification may coalesce without requiring a second wake.
Accepted source work still finishes before that handler leaves. Each request
checks the group's cooperative cancellation token before invoking
the source, including requests on a previously used tag. An admitted request's
completion token posts `-EIO` if its initial child is rejected or skipped;
a source exception also posts `-EIO`. A failed initial handler can be admitted
again for a later request. Lifetime ownership stays with Elio, while the token
records the kernel completion obligation (ADR-0006).
The bridge and handlers access tag state on the bridge worker. Elio user
affinity is best effort: a stolen continuation can execute on another worker
when owner re-enqueue fails. Every relevant suspension return therefore
restores affinity and verifies the actual worker before accessing tag state:
request-event wakes, source returns, bridge reads, notification retry yields,
cleanup entry, and the handler join. Rejected migration retains the same frame,
request and pool, yielding and retrying until the owner is reached. The native
already-local affinity awaitable allocates no helper frame or scheduler task.
A source may deliberately migrate while running; the same restoration applies
before clearing busy state or publishing completion. Failed/skipped child
destructors on foreign workers publish only atomic failure facts and notify
the bridge; they do not mutate owner-only tag state. Failure facts are consumed
before admitting a reused tag. Native event dispatch allocation failure retains
the request and retries notification from the same bridge frame, including
stop notifications. Sustained allocation failure can extend this drain.
If the fallback completion itself cannot be enqueued, the token marks the
queue failed and wakes its waiter. The same failure publication is used after
the bridge drains admitted handlers when a loop exception escapes, and on
cold handler-pool setup failure.

This placement policy retains the dedicated queue thread and both eventfd
handoffs in ADR-0006. It does not make the whole request path allocation-free:
source tasks and unset native event waits can still allocate. Performance issue
[#120](https://github.com/Coldwings/OverlayBD-Elio/issues/120) also requires
evaluation of those remaining handoffs and concurrent workload trade-offs.

The default prioritizes LSMT latency, throughput and CPU cost. Tags can overlap
asynchronous I/O, but CPU work for one queue runs on its bridge worker. A
CPU-intensive gzip source may therefore use fewer cores and lose high-depth
throughput. The runtime's worker count is unchanged; this policy does not
guarantee the same performance trade-off for every source or queue geometry
(ADR-0021).

### `src/ublk/device.hpp` — `obd::ublk::Device`

Full lifecycle of one image as one ublk device. Non-copyable.

- `static elio::coro::task<std::unique_ptr<Device>> create(const
  DeviceParams&, source::BlobSourcePtr src)` — **must be awaited on the
  Elio scheduler** (bridge coroutines are independently joinable). Order:
  open `Ctrl` → `ADD_DEV` → `SET_PARAMS` → open all queues → launch queue
  threads (they park the FETCH commands) → spawn one bridge coroutine per
  queue → `START_DEV`, retrying `EBUSY` up to 100 × 50 ms (5 s). Throws
  `obd::error(EINVAL)` for a null source or `dev_sectors == 0`, and
  `obd::error` on any setup failure (partial state is torn down via
  `stop_async()` and off-worker destruction before the original error is
  rethrown).
- `static elio::coro::task<std::unique_ptr<Device>> attach(uint32_t,
  const DeviceParams&, source::BlobSourcePtr)` — attach a replacement to
  an existing recoverable device. Retry START's transient `EBUSY` against
  a 30-second steady-clock deadline before opening queues; re-park all
  FETCH commands and retry END's `EBUSY` up to 100 times with 50 ms waits.
  Queue geometry must match the original device. Other control errors
  propagate immediately through the same partial-state cleanup as create.
- `uint64_t size_bytes() const` — the current device capacity in bytes
  (the D3 grow-only resize baseline: params-derived on create, kernel-
  derived via `Ctrl::get_params` on a recovery attach — which may exceed
  the create-time size after a grow — and updated by `resize_blocking`).
- `uint64_t resize_blocking(uint64_t bytes)` — D3 grow-only online
  resize: `Ctrl::update_size` after validating positivity, 512-byte
  alignment, and growth over `size_bytes()`. Returns the new capacity.
  BLOCKING — route through `elio::spawn_blocking` (the device command
  loop does; docs/supervisor.md).
- `uint32_t dev_id() const`, `std::string bdev_path() const` →
  `/dev/ublkb<N>`.
- `bool started() const` — true once the kernel gendisk is live
  (`START_DEV` succeeded).
- `void stop() noexcept` — blocking: signals queue threads and bridges,
  waits for every bridge frame and its grouped I/O handlers to release,
  then joins queue
  threads. Idempotent. Call off an Elio worker with source executors still
  able to progress; elapsed time never permits release of unfinished I/O.
- `elio::coro::task<void> stop_async()` — scheduler-side equivalent: signals
  stop and uses native async frame-destruction observation without
  occupying a worker or the blocking
  pool, then joins queue threads off-worker. Retain the Device until the
  await completes. Use this from coroutines, including setup-error cleanup.
- `~Device()` — `stop()`, then `STOP_DEV` / `DEL_DEV` via `~Ctrl`.

## Invariants & Guarantees

- **Ring ownership**: a queue ring is touched (submit, peek, advance) only
  by its queue thread. This is kernel-required (task-pinned IO), not just a
  convention (ADR-0006). Anchor: `src/ublk/queue.hpp::Queue`.
- **Full-request reads**: a READ completion always reports the requested
  length on success; EOF short reads from the source are zero-filled in the
  tag buffer. The kernel copies exactly `nr_sectors * 512` bytes out.
  (Implementation: `handle_io` in src/ublk/elio_bridge.cpp.)
- **Sector math**: all offsets/lengths crossing the bridge are 512-byte
  sector based (`IoRequest::byte_offset` / `byte_len`); sources see
  byte-granular positional IO. Anchor: `src/ublk/queue.hpp::IoRequest`.
- **START_DEV ordering**: `START_DEV` is issued only after every queue tag
  has a parked FETCH; `EBUSY` is retried, never propagated, within the 5 s
  bound. Anchor: `src/ublk/device.cpp::Device::create`.
- **Teardown safety**: `stop()` is idempotent and noexcept; `~Ctrl`
  best-effort unregisters a still-registered device. Destroying a `Device`
  never leaks a kernel device.
- **Error discipline**: setup/cold paths throw `obd::error`; the per-IO hot
  path never throws — failures are negative-errno completions.
- **Result contract**: bridge results follow the ublk convention —
  `>= 0` bytes transferred, `< 0` `-errno` — enforced at
  `Queue::push_completion` / `COMMIT_AND_FETCH_REQ` time.

## Concurrency & Call Permissions

Three distinct execution contexts, with strict permissions:

| Context | Threads | May touch |
|---|---|---|
| Control path | caller of `Device::create` (an Elio coroutine) | `Ctrl` control commands (blocking; cold path only) |
| ublk queue threads | one `std::thread` per `Queue` | **exclusively** their ring, the read-only mmap window, `pending_` push, `done_` drain |
| Elio scheduler threads | bridge + per-tag `handle_io` coroutines | `try_pop_request`, tag IO buffers, `BlobSource` stack, `push_completion` |

- `pending_` / `done_` are the only cross-thread shared state, guarded by
  `Queue::mu_`; wakeup is via `elio_efd_` / `done_efd_` (nonblocking,
  EAGAIN-tolerant writes). Anchor: `src/ublk/queue.hpp::Queue`.
- Tag IO buffers are effectively single-writer: the kernel owns a tag's
  buffer between FETCH and COMMIT for writes (data-in), the bridge owns it
  for reads (data-out). One tag is in flight on exactly one side at a time
  because only the queue thread re-parks it.
- `Device::create` must run on the Elio scheduler; `Ctrl` methods are plain
  blocking calls and must not be issued from a latency-sensitive coroutine
  path. `Device::stop` / `~Device` join threads — call them from a context
  where blocking is acceptable and source completion can still progress.
  Coroutine callers await `stop_async()` before offloading final destruction;
  they must do so on exceptions and early returns as well as success. Awaiting
  source I/O must not occupy the sole blocking thread: the source may need it
  to complete. Stop calls for a Device are sequential, not concurrent.
  Native destruction-observer allocation failure retries in the existing
  stop coroutine frame; owners remain retained until the bridge frame departs.
- `run_bridge` and `handle_io` are Elio coroutines; all source IO goes
  through the Elio IO backend (no blocking syscalls on the scheduler).
- Inputs are not mutated: `DeviceParams` is read-only after `create`;
  `src` ownership moves into `Device` and outlives all bridges (destruction
  order: stop drains bridges and handlers and joins queue threads before
  members die). The daemon retains its owner until the control task body and
  captures are destroyed, then releases the Device off-worker. Its successful
  shutdown drains resize, drains device I/O, finalizes recording, checkpoints,
  parks fills, reports stopped, and closes/joins the control loop in that order.

## Stability Contract

- **Kernel ABI view** (`src/ublk/uapi_compat.hpp`): the `#ifndef`-guarded
  shim and the asserted geometry (24-byte descriptor, field offsets, 16-byte
  `ublksrv_io_cmd`, depth 4096, mmap stride/size formulas) are the project's
  contract with the kernel. We add, never redefine; any change to this view
  is a wire-format change and requires an ADR (T1, per AGENTS.md and
  ADR-0006).
- **Op dispatch semantics** are user-visible contract: READ zero-fill at
  EOF; WRITE → writable root `pwrite` else `-EROFS`; FLUSH → writable root
  `flush` else `0`; DISCARD → writable root `discard` else `-EROFS`;
  WRITE_ZEROES → `discard`, or a real zero-write under `UBLK_IO_F_NOUNMAP`;
  WRITE_SAME / unknown → `-EOPNOTSUPP` (ADR-0009). Changing any of these
  is breaking for callers that depend on the errno surface.
- **Bridge contract**: `Queue::push_completion` result convention
  (`>= 0` bytes, `< 0` `-errno`) and the eventfd handoff are internal but
  cross-module (source layer depends on the read/write semantics through
  `BlobSource` / `WritableBlobSource`, ADR-0008).
- **DeviceParams defaults** (queue depth 128, 128 KiB max IO buf, 512 B
  logical / 4 K physical block, read-only by default) are behavioral
  contract; changing defaults is breaking.
- Not contractual: `user_data` bit encoding, retry counts, internal deque
  types — free to change without notice.

## Testing

Unit tests live in `tests/unit/test_ublk.cpp` (built only when
`OBD_ENABLE_UBLK` is on); the kernel-dependent end-to-end test lives in
`tests/integration/test_ublk_e2e.cpp`.

- `ublk: repeated tag reads stay on the bridge worker` — four runtime workers
  retain source entry/resume on the explicitly selected bridge worker.
- `ublk: a reused tag does not readmit a scheduler child` — after a first
  successful read, another read succeeds without consuming an armed native
  scheduler-admission rejection.
- `ublk: a warm tag rejects new reads after parent cancellation` — the reused
  handler completes with `-EIO` and does not reenter its source.
- `ublk: a tag returns to its bridge after source migration` — a source moves
  to another worker; successive requests still enter on the bridge worker.
- `ublk: a tag retries after its initial child admission is rejected` — native
  scheduler rejection completes the first request with `-EIO`; a later request
  starts a new handler and completes normally.
- `ublk: foreign resumptions retain tag state until owner restoration` —
  deterministic native-migration injection at warm event wakes, source returns,
  request retries and stop retries; real inbox-full/overflow-allocation rejection
  holds each continuation away from owner-only state until an independent OS
  controller releases pressure and its owned native owner-hold task. Hooks
  absent from normal use create no helper task/frame. This checks restoration
  boundaries, not an observed native steal or sanitizer race.
- `ublk: tag notification allocation failure retains reads and stop` — actual
  native event dispatch allocation failures on request delivery and stop are
  each observed once; the source completes and every handler drains.
- `ublk: stop drains a request before its tag consumes the wake` — cold and
  reused tags receive request and close notifications before consuming either;
  accepted reads complete and shutdown drains without a rescue notification.
- `ublk: recovery waits for quiescence before opening replacement queues` —
  injected command, steady clock and awaitable sleep return `EBUSY` three
  times before success; the continuation cannot open queues while START
  is still busy. No privileged device or actual timer wait is required.
- `ublk: recovery does not retry nonbusy failures` — `EINVAL`, `EACCES`
  and `ECANCELED` propagate on the first attempt with no sleep or queue open.
- `ublk: recovery busy retries share one monotonic deadline` — time spent
  in each control command consumes the original budget; persistent busy
  fails with the documented errno/message without opening queues.
- `ublk: recovery caps its final wait and never retries after expiry` —
  a final 20 ms budget produces only a 20 ms wait, then timeout with no
  post-deadline command.
- `ublk: replacement queue open failure never repeats accepted recovery` —
  even `EBUSY` from opening replacement queues propagates immediately;
  START may not be repeated once it has reset the old queue state.
- `ublk: failed recovery preparation and END retain one cleanup owner` —
  injected capacity-read, preparation-log allocation and END errors run
  through the same finish helper as attach. A deletion ledger immediately
  reuses the numeric id after deletion and verifies that only the caller's
  fallback deletes the original generation; adopted cleanup cannot delete
  it first and leave that fallback armed against the new generation.
- `ublk: recovery adoption is the final nonthrowing ownership transfer` —
  preparation precedes END and adoption; after successful return the caller
  disarms its fallback and Device cleanup deletes exactly once. The helper
  requires the final adoption callback to be `noexcept`.
- `ublk: control task releases its device owner before final destruction` —
  actual control-loop EOF and frame/capture teardown precede final off-worker
  Device destruction, with one worker and one blocking thread.
- `ublk: failed setup drains partial state before rethrowing its error` —
  the create/attach cleanup routine preserves the original exception and
  destroys partial state off-worker, before and after bridge registration.
- `ublk: blocking stop waits for a slow source to finish` — a normal read
  completes after eight seconds while a dedicated off-worker caller drains;
  stop must not return before completion, and owners remain valid throughout.
- `ublk: async stop drains an idle bridge before source destruction` — real
  bridge/eventfd shutdown on one worker, including a not-yet-started bridge.
- `ublk: async stop lets a source finish on the sole blocking thread` — a
  pending read completes through the single blocking thread after stop begins;
  the source remains owned until the handler posts its completion and exits.
  A test section injects a real native destruction-observer allocation failure
  and checks that the same owner still drains successfully.
- `ublk: async stop handles a partially initialized device` — cleanup before
  any queue is opened. These unit tests bypass kernel registration, command
  mappings, queue rings and queue threads; each has a 30-second CTest timeout.
- `ublk: task group drains every admitted tag including source failures` —
  two held reads remain owned during stop and use the sole blocking thread;
  both publish terminal completions, including `-EIO` for a throwing source.
- `ublk: a cancelled task group still completes a skipped tag` — a handler
  cancelled before invocation posts `-EIO` without calling the source.
- `ublk: failed completion publication wakes the queue before stop` — a
  private per-queue fault at completion insertion forces a skipped child's
  fallback, or both normal publication and fallback, to fail. A bounded plain
  thread observes the actual `done_efd` wake before stop; a separate section
  checks direct external failure publication. Owners drain before assertions.
  This uses eventfd readiness without registering a kernel ublk device or
  executing the queue's io_uring loop.
- `ublk: command buffer geometry matches the driver layout` — pins the mmap
  geometry against the driver formulas: stride = 98304 for the 24-byte
  descriptor, `cmd_buf_size(128, …)` = 4096, `cmd_buf_size(4096, …)` =
  98304. Guards against regressions in `cmd_buf_stride` / `cmd_buf_size`
  that would make `ublk_ch_mmap` reject our mappings.
- `ublk: queues own close-on-exec duplicates of one open file description` —
  uses a real temporary file and both queue-offset mappings to verify distinct
  owned descriptors, close-on-exec flags, shared file offsets, survival after
  the first queue and borrowed original close, and final descriptor closure.
  This is descriptor-lifetime coverage; it does not register a ublk device or
  exercise the kernel's ublk commands.
- `ublk: IoRequest byte math is sector based` — pins the 512-byte sector
  conversion (`start_sector 100` → offset 51200; 8 sectors → 4096 bytes)
  that every bridge handler relies on.
- `integration: ublk device serves sector reads from a blob` — privileged
  E2E: creates a real `Device` over an in-memory source, opens
  `/dev/ublkb<N>`, and verifies a 4 KiB `pread` at byte offset 1024 returns
  the exact source bytes with one and two hardware queues. Guards the whole
  path: control plane, queue
  thread, bridge dispatch, zero-copy buffer addressing, and teardown. It
  **self-skips** when the shared prerequisite probe finds the control node
  missing/unavailable, an `O_RDWR` open denied, or effective `CAP_SYS_ADMIN`
  absent. All privileged ublk tests perform this check before constructing
  fixtures or devices. It uses actual open access and effective capabilities,
  not UID: a nonroot process with the required access and capability is
  admitted, while root without the capability skips. The probe issues no
  kernel device commands. Unexpected probe errors (such as `EMFILE` or
  `EIO`) fail, as do subsequent device setup or I/O errors; the probe does
  not guarantee authority in the kernel's required user namespace. The
  integration test target is registered with
  CTest property `SKIP_RETURN_CODE 4` ("all selected tests skipped"), so an
  unprivileged run stays green. See `tests/CMakeLists.txt`.
- `integration: ublk device grows online and serves the new capacity` —
  privileged E2E: grows the writable data plane and kernel capacity, verifies
  `BLKGETSIZE64`, writes and reads in the added region, preserves original
  content, and rejects shrink attempts. Its dedicated privileged CI step
  accepts only the explicit unsupported-`UPDATE_SIZE` skip (kernel
  ENOTSUPP/524 or EOPNOTSUPP/95); unavailable control access fails that step.
  A supported pass and an unsupported skip are reported separately in the
  retained grow log. See `docs/testing.md` for timeouts and CI evidence.
- `integration: ublk device survives server death via USER_RECOVERY` —
  privileged E2E: the live child reports actual `GET_DEV_INFO` flags before
  any crash. Only a device that negotiated both USER_RECOVERY and REISSUE
  is killed and reaped, then immediately attached without a quiesce delay;
  source bytes remain readable. A child without those flags instead drains
  and deletes normally; the parent reaps it and verifies `GET_DEV_INFO`
  returns `ENODEV` before the precise unsupported-capability skip. A separate
  section explicitly disables recovery and requires that graceful cleanup
  path to pass on every kernel with ublk. Both sections run with one and two
  hardware queues. Fixture guards retain the child,
  pipe fds, client fd and recoverable device across assertions and failed
  attaches. Unexpected attach errors, including `EINVAL`, fail the test.
- `ublk prerequisites: missing access or capability skips before setup` —
  deterministic, unprivileged classification coverage for missing control
  nodes, denied read/write opens, and effective capability masks.
- `ublk prerequisites: unexpected probe errors remain failures` — verifies
  that resource exhaustion and unexpected open errors are not hidden as
  missing prerequisites. These two tests use synthetic observations and do
  not open a control device or create a kernel device.

Run: `ctest --test-dir build --output-on-failure` (the E2E test requires
control-node read/write access, effective `CAP_SYS_ADMIN` in the required
user namespace, and a loaded `ublk_drv` to actually execute).

## Limitations & TODO

- **WRITE_SAME**: never advertised, defensively rejected.
- **Recovery re-open data boundary**: `Device::attach` re-opens the image
  from disk; an unsealed LSMT-RW upper loses its unsealed writes on
  recovery while a sparse upper recovers via fiemap (ADR-0008, ADR-0010).
- **Single device per `Ctrl` instance**: the class remembers one added
  device; multi-device processes would need one `Ctrl` each.
- **START_DEV polling**: fixed 100 × 50 ms bound rather than an event-driven
  readiness signal; sufficient in practice, worth revisiting if queue counts
  grow large.
- **Failure blast radius**: a queue ring failure sets the sticky
  `failed_` flag and stops that queue's loop, but there is no automatic
  device-level teardown cascade yet — operators should treat
  `Queue::failed()` as fatal for the device.
- The supervisor-facing readiness signaling around a started device lives in
  `obd-device` (`src/cmd`), not in this module.
