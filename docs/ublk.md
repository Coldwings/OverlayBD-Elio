# ublk — kernel control plane and fixed-worker queue owners

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
- **Elio queue owner** (`src/ublk/elio_bridge.hpp`) — one fixed-worker
  coroutine per queue creates and drives the raw ring, owns reusable tag
  handlers, and serves requests from the merged `BlobSource` stack
  (ADR-0023).

`src/ublk/uapi_compat.hpp` is the module's single view of the kernel uapi:
it includes `<linux/ublk_cmd.h>` and adds — `#ifndef`-guarded — only what a
given header version lacks, per the AGENTS.md rule that we never redefine
what the kernel header provides. Baseline: kernel headers >= 6.0.

One `Device` (`src/ublk/device.hpp`) ties the three layers together for one
image inside one `obd-device` process (ADR-0004 isolation).

## Concepts

### Production queue and worker topology

`obd-device` resolves `ublkConfig.queues` and `ublkConfig.workers` before
constructing its scheduler (ADR-0025; [config.md](./config.md)). Automatic
creation uses equal counts bounded by the process CPU budget and eight pairs.
Manual settings can provide extra workers, with workers at least equal to
queues. Queue `q` starts on worker `q % workers`, so production queues have
distinct owners. The lower-level `DeviceParams` default remains one queue;
the production entry point supplies its resolved count explicitly.

Recovery first reads the existing kernel queue count, depth and buffer size
without adopting the device. Automatic settings follow that layout, and
explicit incompatible counts fail before recovery admission. `Device::attach`
rechecks the supplied geometry before START recovery. Counts remain fixed
through service and retirement. Startup logs report requested counts,
resolved queues, actual runtime workers and the serving Device's queue count.
Each initialized queue also logs its owner worker and Linux TID once, allowing
operators to correlate owner placement with per-thread CPU observations.

### Queue rings and the ADR-0023 ownership rule

Every hardware queue has exactly one fixed **Elio worker owner**. That Linux
task creates the queue's raw io_uring and is the only task that submits
`FETCH` / `COMMIT_AND_FETCH` uring-cmds. This preserves both
`IORING_SETUP_SINGLE_ISSUER` and the ublk rule that binds a tag's FETCH and
COMMIT to the same task.

The raw ring registers a nonblocking eventfd. Its owner coroutine waits for
that descriptor through the worker's separate Elio-native I/O ring. With
`IORING_SETUP_DEFER_TASKRUN`, Linux `io_req_local_work_add()` signals the
registered eventfd when the ring's local-work list changes from empty to
nonempty, before that work publishes a CQE. The eventfd is therefore a hint,
not CQ evidence: after every wake the same Linux task enters the raw ring with
`GETEVENTS` and minimum zero to execute local work. Waiting never blocks the
worker.

A completed FETCH is admitted directly to that queue's reusable TagEngine.
Source operations can suspend and resume on another worker, but each handler
restores the queue owner before touching tag state or submitting
COMMIT_AND_FETCH. The normal request and completion path therefore has no
mutex deque or application eventfd handoff. Only stop/failure and exceptional
foreign handler retirement write the owner eventfd.

The pump consumes at most 32 raw CQEs per turn. If CQ work remains, it takes a
native-timer turn before continuing so older local tasks, Elio native I/O,
control work, and other queues can progress. An idle queue parks directly on
its eventfd, so QD1 adds no fairness timer.

`Device::create` and `Device::attach` open `/dev/ublkc<N>` once for the
device: the driver's open exclusion applies to the whole device, including
all hardware queues. The first `Queue` owns that descriptor; each additional
queue owns a close-on-exec duplicate sharing its open file description.
Every queue still has its own command-buffer mapping, tag buffers, eventfd,
raw ring, and fixed owner. Queue destruction closes only its owned descriptor;
the existing mapping and command lifetime barriers still apply. Recovery opens
the first descriptor only after `START_USER_RECOVERY` succeeds.

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

1. The queue owner parks one `UBLK_IO_FETCH_REQ` per tag. The driver refuses
   `START_DEV` with `EBUSY` until every tag of every queue is parked, so
   `Device::create` waits for all fixed owners before releasing initial FETCH
   publication and retrying START.
2. A completed FETCH CQE makes tag `t`'s shared `ublksrv_io_desc` valid. The
   owner copies it into a preallocated owner-local request ring and admits it
   to the reusable handler for that tag. Different tags remain independent
   TaskGroup children and can overlap remote or local source I/O.
3. After source I/O, the handler restores its fixed owner, validates the exact
   tag generation, and directly prepares and submits
   `UBLK_U_IO_COMMIT_AND_FETCH_REQ`. No foreign worker may inspect raw slot or
   ring state.
4. The next raw CQE retires the command's unique cookie and either delivers the
   next request for that tag or records its terminal kernel result.

Data `user_data` cookies combine an increasing command generation with the low
16-bit tag. Each completion must match its pending cookie. Duplicate, foreign,
or ambiguous completion facts make debt unknown rather than retiring another
command. The generation mailbox is separate: it covers only exceptional
handler retirement after a foreign scheduler rejection and is consumed by the
owner before any queue state changes.

### Op dispatch table

The tag engine (`src/ublk/tag_engine.cpp`, function `handle_io`) implements the
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

### Crash recovery (ADR-0010, ADR-0022)

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
- `END_USER_RECOVERY` and `START_DEV` share a five-second monotonic startup
  budget with fixed-owner preparation. Transient `EBUSY` retries yield between
  attempts. Queue failure, deadline or explicit release can request control
  cancellation; an original result must still settle before cleanup proceeds.

Other START errors fail its preparation; exhausting its retry budget reports
`ublk START_USER_RECOVERY not ready after 30s`. Successful original END adopts
the old device immediately, including success after cancellation intent.
A subsequent queue-health failure therefore follows adopted cleanup, not an
unadopted failed-recovery path. Ready publication additionally requires healthy
owners and atomic closure of startup-release admission. `ADD_DEV` falls back
to a non-recoverable device when the kernel rejects recovery flags with EINVAL.

### Failed startup ownership and explicit release (ADR-0022)

All queue rings and reusable TagEngines must become ready before the
coordinator opens initial FETCH publication. After publication, a failed queue
enters maintenance on its original fixed worker rather than destroying the ring.
Original raw command cookies and terminal CQEs account for kernel loans;
cancellation CQEs and native coroutine completion are separate facts.

A failed pre-END recovery with published work retains the old device and the
complete startup/caller/source/runtime graph. `StartupControl` exposes facts
and release admission; it owns none of those resources. Reporting failure or
cancelling an observer cannot unwind the owning startup task. Successful ADD
or original END supplies cleanup authority; failed unadopted recovery does not.

Original START/END success can race an earlier timeout cancellation request.
If the queues remain healthy and no operator release won admission, the
successful ready handoff clears that historical error reason. The supervisor
then accepts the coherent ready record and switches to ordinary lifecycle
management; historical cancellation intent cannot strand startup protection.

Explicit release settles startup control work, stops new source admission and
awaits tag-handler/source retirement while retaining the raw-ring owners.
Initial FETCH issuance must be fenced before the authorized STOP cancellation
pass: a prepared request must not be submitted afterward. The current fence
relies on synchronous, non-SQPOLL owner submission with no ASYNC/LINK/DRAIN
flags; Linux 6.6 FETCH has no io-wq EAGAIN retry path. SQ consumption under
these constraints is an issuance boundary, not proof of driver acceptance or
readiness. Each original CQE still has to be observed.

STOP does not replace the retirement ledger. Queue owners remain runnable to
COMMIT delivered requests and consume original completions. Only known raw retirement, tag-frame retirement, owner exit and final
owner-frame destruction permit ring/mapping/cdev closure. All queue cdev references close before one DEL;
its cached result prevents blind deletion of a reused numeric device ID.
Refused or unknown outcomes retain the owner and attempt ledger. The supervisor
also requires correlated release and actual process reap before erasing its
entry; see [supervisor.md](./supervisor.md).

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

These are the lower-level API defaults. Production startup sets `nr_queues`
from `ublkConfig`; recovery also supplies the kernel's existing depth and
maximum buffer size.

`Ctrl` — one instance per device process; owns the `/dev/ublk-control` fd
  and the SQE128 control ring
and remembers the added device for best-effort cleanup. Non-copyable.

- `Ctrl()` — opens `/dev/ublk-control` (`O_RDWR | O_CLOEXEC`); throws
  `obd::error` when unavailable (driver not loaded / no permission).
- `~Ctrl()` — retains pending control ownership until original/cancellation
  records settle. Unmanaged instances retain the legacy best-effort cleanup;
  Device disables it and performs explicit retirement/close-before-DEL.
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
- `void set_observer(CtrlObserver) noexcept` — installs cold-path cancellation,
  original-completion and progress callbacks; callbacks cannot throw or outlive
  their borrowed context. Successful original completion commits ownership
  before fallible reporting.
- `void disable_destructor_cleanup() noexcept` — makes the Device coordinator
  responsible for explicit teardown.
- `bool command_pending() const noexcept`, `int added_dev_id() const noexcept`
  — pending-command and added/adopted-ID facts.
- `int stop_dev_result(uint32_t) noexcept` — STOP with a retained result.
- `int delete_dev_once(uint32_t) noexcept` — one DEL attempt, returning the cached
  result on later calls; zero or negative errno.
- `static std::string cdev_path(uint32_t)` → `/dev/ublkc<N>`;
  `static std::string bdev_path(uint32_t)` → `/dev/ublkb<N>`.

All `Ctrl` commands are blocking cold-path calls. Coroutine callers
use the cold controller executor for startup/teardown. It runs each action
on an owned thread and retains its frame until join, leaving the sole source
blocking worker available. Other cold operations such as resize use
`elio::spawn_blocking`. Kernel control commands may sleep on our own
data plane — START_DEV's `add_disk` partition scan issues device reads
serviced by fixed queue owners, and END_USER_RECOVERY waits for reissued IO.
A synchronous control call on a scheduler worker deadlocks it (the
worker stalls, so queue-owner and source coroutines cannot progress). Never invoke them on
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
  IO buffers, and creates nonblocking close-on-exec wake descriptors. The
  production raw ring is created later by `init_owner_pump()` on its fixed
  worker and registers the owner descriptor. Throws `obd::error`.
- `uint16_t q_id() const`, `uint16_t depth() const`.
- `int cdev_fd() const noexcept` — borrows the queue's owned descriptor for
  subsequent queue setup; returns `-1` before descriptor acquisition. Do not
  close it or retain it past Queue destruction;
  duplicate it when independent ownership is needed.
- `void* io_buf(uint16_t tag) noexcept` — the tag's kernel-shared payload
  buffer (`tag * max_io_buf_bytes` into the allocation). Read handlers write
  into it; write handlers read from it. No bounds check — `tag < depth` is a
  kernel-guaranteed precondition.

Fixed-worker owner side (ADR-0023):

- `void init_owner_pump(size_t owner_worker)` — creates the raw ring on the
  current fixed worker with SINGLE_ISSUER and DEFER_TASKRUN, preallocates CQ
  and request storage, registers the queue eventfd, and publishes setup-ready.
  It prepares no ublk command before the publication gate.
- `int owner_eventfd() const noexcept` — registered raw-CQ and control wake
  descriptor. The pump awaits it through Elio native I/O.
- `void prepare_owner_fetches()`, `void submit_owner_fetches()` — prepare and
  synchronously submit the initial FETCH batch on the owner. The split lets
  the opt-in recovery fixture hold the exact boundary through an async gate
  without blocking the fixed worker; production calls them consecutively.
  Once preparation begins, even a fixture failure must reach submission before
  STOP. `initial_fetches_issued()` becomes true only after the SQ is consumed
  under the documented non-SQPOLL constraints.
- `bool drive_owner_cq(unsigned budget)` — treats the eventfd wake as a hint,
  enters GETEVENTS with minimum zero to execute owner task-work, consumes at
  most `budget` exact-cookie CQEs, and places delivered requests in
  preallocated owner-local storage. It returns whether CQ work remains.
- `bool try_pop_request(IoRequest&)` — owner-only pop from that storage.
- `void complete_on_owner(uint16_t tag, int32_t result)` — owner-only exact
  obligation check plus direct COMMIT_AND_FETCH preparation and submission.
- `void maintain_owner()` — after source admission closes, rejects unadmitted
  delivered requests, repairs missing handler results only after the source
  fence, prepares cancellation replies, submits them, and republishes the
  retirement ledger.
- `bool source_handlers_drained() const`,
  `void mark_source_handlers_drained()` — the TaskGroup/source-buffer fence.
  This is deliberately earlier than raw owner exit.
- `bool owner_may_exit() const`, `void finish_owner()` — exit requires explicit
  coordinator permission and positive raw retirement; finish publishes the
  final snapshot.
- `void fail(int err) noexcept`, `void wakeup() noexcept` — publish sticky
  failure/control facts and wake the registered owner eventfd. Saturated
  eventfd writes are harmless because an unread wake already exists.
- `Retirement retirement() const noexcept` — `published`, `retired`,
  `delivered`, `known`, `complete`; unknown debt prevents safe release.
- `void close_retired() noexcept` — only after owner-frame destruction and
  positive raw retirement, close ring/mapping/cdev before DEL.

The old `run` / request-completion bridge functions remain compiled only to
support deterministic coordinator fixtures with synchronous native-thread
gates. `Device` never selects them in production or in real-kernel fixtures.

### `src/ublk/tag_engine.hpp` — reusable tag processing

```cpp
struct OwnerResult { uint16_t tag; uint64_t generation; int32_t result; };
OwnerResultSink(void* context, Publish publish, Fail fail, Notify notify);
void OwnerResultSink::publish(OwnerResult) const;
void OwnerResultSink::fail(int) const noexcept;
void OwnerResultSink::notify_owner() const noexcept;

explicit ForeignResultMailbox(uint16_t depth);
bool ForeignResultMailbox::arm(uint16_t tag, uint64_t generation) noexcept;
bool ForeignResultMailbox::retire(uint16_t tag, uint64_t generation) noexcept;
bool ForeignResultMailbox::publish(OwnerResult) noexcept;
bool ForeignResultMailbox::take(uint16_t tag, uint64_t generation,
                                OwnerResult& out) noexcept;

TagEngine(Queue*, source::BlobSource*, OwnerResultSink, size_t owner);
void TagEngine::collect_foreign_results() noexcept;
bool TagEngine::admit(IoRequest);
bool TagEngine::retry_signal(uint16_t tag);
void TagEngine::begin_close() noexcept;
bool TagEngine::retry_close_signal(uint16_t tag);
elio::coro::task_group::join_awaitable TagEngine::join_handlers() noexcept;
bool TagEngine::handlers_drained() const noexcept;
```

`OwnerResult` is the internal completion fact `{tag, generation, result}`.
`OwnerResultSink(void*, Publish, Fail, Notify)` is a non-owning function-pointer
callback boundary: only the tag owner calls `publish(OwnerResult)`; a foreign
continuation may call the non-throwing `fail(int)` and `notify_owner()` callbacks
to publish sticky failure/wake facts. Callback context and functions must
outlive the engine.

`ForeignResultMailbox(uint16_t depth)` preallocates one atomic slot per tag and
rejects zero depth. `arm(tag, generation)` reserves an exact nonzero generation;
`publish(OwnerResult)` fills that generation once without allocating or
throwing; stale, duplicate, out-of-range and unarmed publications return false.
Only the owner calls `take(tag, generation, out)` to consume and clear an exact
ready result, or `retire(tag, generation)` after a normal owner-side result.
Both reject a mismatched generation without changing the slot. Generations are
bounded by the mailbox's packed atomic representation; the engine reports
exhaustion before wraparound.

`TagEngine(Queue*, BlobSource*, OwnerResultSink, size_t owner)` preallocates tag
state and the mailbox, and keeps long-lived handlers in a native collect-all
TaskGroup. The queue, source, callback context and selected scheduler worker
must remain live until `join_handlers()` completes. All methods are owner-only
except the foreign-safe sink callbacks. `admit(IoRequest)` rejects a closed
engine before any mutation, rejects an invalid or busy tag, assigns a fresh
generation, and reuses that tag's handler after its first admission. It returns
false only when the event wake allocation must be retried with
`retry_signal(tag)` from the caller's existing coroutine frame.
`begin_close()` closes admission and marks every handler terminal;
`retry_close_signal(tag)` wakes each live handler, and the caller then directly
awaits the non-coroutine `join_handlers()` awaitable before consuming foreign
results and destroying the engine. This split keeps the lifetime fence in the
already-allocated bridge frame: an allocation failure cannot create a cleanup
task that unwinds before the TaskGroup is joined. Child failures are reported
only after the group drains; the bridge fail-stops if an internal join contract
error is ever observed while a handler remains live.

Normal handler returns restore and verify the owner, then publish directly
through the sink. A rejected/skipped handler or a handler destroyed on another
worker can only publish `-EIO` into its prearmed generation and wake the owner.
The owner validates that generation before clearing busy state or invoking the
sink, so a delayed result cannot complete a reused tag. TaskGroup frame lifetime
and the per-request completion obligation remain separate facts.

The production sink calls `Queue::complete_on_owner`, so a normal handler
submits COMMIT_AND_FETCH directly after restoring the fixed worker. The legacy
fixture sink still targets `Queue::push_completion`; it is not selected by a
production or real-kernel Device.

### `src/ublk/elio_bridge.hpp` — `obd::ublk::run_owner_pump`

```cpp
elio::coro::task<void> run_owner_pump(Queue* q, source::BlobSource* src,
                                      std::atomic<bool>* stop);
```

`Device` launches one joinable owner with `go_joinable_to`; queue IDs are
spread round-robin across the runtime's existing workers. The task creates the
raw ring on that worker, constructs one reusable `TagEngine`, publishes both
setup facts, and waits asynchronously for the initial-FETCH gate.

In service, each wake restores and verifies the owner before raw-ring access,
drives at most 32 CQEs, admits fixed-storage requests, and parks again when the
CQ is empty. Normal handler results go straight to `complete_on_owner`. A
foreign rejection can publish only into the generation mailbox and write the
owner eventfd. The next owner turn validates that exact generation before any
tag or raw state changes.

Stop closes admission and signals every reusable handler. The pump awaits the
TaskGroup's non-coroutine join awaitable in its already-allocated owner frame;
an internal join error with a live child fail-stops rather than freeing tag
state. Once all children and exceptional mailbox results have retired, it
publishes `source_handlers_drained`. The Device can then issue STOP while this
same task remains alive to drive terminal raw CQEs. The task returns only after
an explicit exit request and a known-complete ledger.

`run_bridge` is retained for deterministic legacy unit fixtures. Production
and real-kernel Device startup always use `run_owner_pump`.

### `src/ublk/device.hpp` — `obd::ublk::Device`

Full lifecycle of one image as one ublk device. Non-copyable.

- `using FailureCleanup = std::function<elio::coro::task<void>()>` — caller
  cleanup hook for parking image fills after native source drainage. Its
  captures must outlive the awaited startup, including retained failure.
- `static elio::coro::task<std::unique_ptr<Device>> create(const DeviceParams&,
  source::BlobSourcePtr, FailureCleanup = {})` and the overload with
  `StartupControl&` before `FailureCleanup` — await on the Elio scheduler.
  Open Ctrl, ADD/SET_PARAMS, prepare all fixed-worker queue owners, publish FETCH,
  then START with the shared startup deadline. Null source, zero sectors or
  zero queues fail with EINVAL. Failures return only after eligible cleanup;
  unresolved ownership keeps the operation and its callers alive.
- `static elio::coro::task<std::unique_ptr<Device>> attach(uint32_t,
  const DeviceParams&, source::BlobSourcePtr, StartupControl&,
  FailureCleanup = {})` — prepare recovery before opening cdev,
  preserve actual capacity via GET_PARAMS, prepare owners, publish FETCH,
  then END. Queue geometry must match the old device. Published unadopted
  failure is retained until explicit supported release.
- Recovery requires a caller-supplied `StartupControl` accessible to a
  concurrent status/release service. Keep the controller and runtime alive
  until attach finishes, including retained failure. After an exception,
  disarm fallback deletion only with positive deletion/absence evidence
  (original DEL success/ENODEV or STOP ENODEV and released phase). END may
  have adopted the device before a later failure caused automatic cleanup;
  conversely, released phase alone can mean only that an unadopted,
  pre-publication frame has retired without deleting the old device. Destroying a suspended task or its runtime
  is not cleanup. The create convenience overload retains automatic cleanup
  authority over its newly added device and does not need operator permission
  to release it; supply a controller when external startup observation is needed.
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
- `void stop() noexcept` — blocking: signals each owner, waits for every
  grouped source handler to release its buffer, drives STOP/retirement through
  the still-running owner tasks, and waits for their final frame destruction
  before closing ring resources. Idempotent. Call off an Elio worker with the
  scheduler and source executors still able to progress.
- `elio::coro::task<void> stop_async()` — scheduler-side equivalent: awaits the
  source-drained fence without occupying a worker, then routes blocking control
  operations off-worker while fixed owners finish raw retirement. Retain the
  Device until the await completes. Use this from coroutines, including
  setup-error cleanup.
- `StartupSnapshot released_snapshot() const noexcept` — final queue/control
  ledger; read only after stop/stop_async completes.
- `~Device()` — blocking stop and explicit proven teardown. Managed Ctrl
  destruction does not replay STOP/DEL.

### `src/ublk/startup_control.hpp` — startup observation/admission

- `explicit StartupControl(uint64_t generation)` — rejects zero; non-copyable.
- `StartupSnapshot snapshot() const noexcept` — allocation-free coherent facts:
  phase/operation, identity/revision, original/cancel results and known flags,
  control/raw debt, and release sequence/result. An accepted mailbox request
  projects an odd revision between the owner's even publications so protocol
  consumers can accept the changed pending facts. It transfers no ownership.
- `ReleaseRequestResult request_release(uint64_t generation, uint64_t sequence)
  noexcept` — one outstanding increasing sequence in `[1, 2^63)`; returns
  accepted, stale, busy or not_startup. Ready closure and acceptance use one
  atomic decision.
- `bool release_before_start() noexcept` — owner-only completion before any
  Device invocation/publication. On true the caller must leave startup and
  never create a device afterward.
- `bool owner_entered() const noexcept` — the Device body accepted responsibility;
  constructing its coroutine task alone does not establish that handoff.
- `StartupReleased` — exception indicating completed explicit startup release;
  it carries no resource graph and is thrown only after safe release.

Retain StartupControl and all caller captures through the awaited startup.
Concurrent snapshot/request operations are permitted; publication is a cold
single-writer coordinator responsibility. Successful ready handoff closes the
startup mailbox; the returned Device's ordinary owner handles later shutdown.

## Invariants & Guarantees

- **Ring ownership**: a queue ring is touched (submit, enter, peek, advance)
  only on its recorded fixed Elio worker and Linux task. FETCH and its matching
  COMMIT therefore retain exact task identity (ADR-0023).
- **Full-request reads**: a READ completion always reports the requested
  length on success; EOF short reads from the source are zero-filled in the
  tag buffer. The kernel copies exactly `nr_sectors * 512` bytes out.
- **Sector math**: all request offsets and lengths are 512-byte sector based;
  sources see byte-granular positional I/O.
- **Startup ordering**: every owner ring and TagEngine is ready before FETCH
  publication. START/END readiness is decided by the original kernel control
  result; publication or SQ consumption alone is not readiness.
- **Teardown ownership**: source handler join precedes STOP, but owner lifetime
  extends beyond it. Original CQEs, a known-complete ledger, owner exit, and
  final owner-frame destruction all precede ring/mapping/cdev closure. Failure
  may intentionally retain the device process indefinitely (ADR-0022).
- **Exactly-once completion**: one raw cookie identifies each command and one
  TagEngine generation identifies each admitted source obligation. Duplicate,
  stale, foreign, or mismatched facts fail the queue instead of completing a
  later request.
- **Error discipline**: setup/cold paths throw `obd::error`; per-I/O results use
  the ublk convention `>= 0` bytes and `< 0` negative errno. Source exceptions
  are normalized to `-EIO` before owner submission.

## Concurrency & Call Permissions

| Context | Execution | May touch |
|---|---|---|
| Control path | owned cold-controller thread, coordinated by startup coroutine | Ctrl commands and original/cancel ledger; blocking, cold path only |
| Queue owner | one fixed Elio worker per Queue; workers may host multiple queues | that queue's raw ring, CQ/SQ, command mmap, raw slots, request ring, TagEngine owner state |
| Tag handler | TaskGroup child, restored to its queue owner before queue access | tag buffer and BlobSource while suspended; raw ring only after verified owner restoration |
| Foreign failure path | any worker during rejected/destroyed handler cleanup | atomic generation mailbox, sticky failure, owner eventfd wake only |

- The registered queue eventfd is the only cross-thread wake in production.
  Raw completions and stop/failure increment it; normal request and completion
  data stays in owner-local preallocated state.
- Tag buffers transfer ownership with the protocol: kernel before FETCH CQE,
  one source handler until it publishes a result, then kernel after the owner
  submits COMMIT_AND_FETCH. Reuse waits for both exact-generation and raw-cookie
  retirement.
- `Device::create` requires a running Elio scheduler. The worker count remains
  stable for the Device lifetime; current `obd-device` does not resize it.
- Control methods remain blocking and never run on scheduler workers. `stop()`
  and destruction are off-worker; coroutine callers await `stop_async()`.
- Device owns the source graph until all owner tasks are destroyed. A
  source-drained flag permits STOP but does not permit object or ring teardown.

## Stability Contract

- **Kernel ABI view** (`src/ublk/uapi_compat.hpp`): the `#ifndef`-guarded
  shim and the asserted geometry (24-byte descriptor, field offsets, 16-byte
  `ublksrv_io_cmd`, depth 4096, mmap stride/size formulas) are the project's
  contract with the kernel. We add, never redefine; any change to this view
  is a wire-format change and requires an ADR (T1, per AGENTS.md and
  ADR-0023).
- **Op dispatch semantics** are user-visible contract: READ zero-fill at
  EOF; WRITE → writable root `pwrite` else `-EROFS`; FLUSH → writable root
  `flush` else `0`; DISCARD → writable root `discard` else `-EROFS`;
  WRITE_ZEROES → `discard`, or a real zero-write under `UBLK_IO_F_NOUNMAP`;
  WRITE_SAME / unknown → `-EOPNOTSUPP` (ADR-0009). Changing any of these
  is breaking for callers that depend on the errno surface.
- **Owner completion contract**: `Queue::complete_on_owner` accepts the ublk
  result convention (`>= 0` bytes, `< 0` negative errno) only on the fixed
  ring owner. Source modules depend on the read/write semantics through
  `BlobSource` / `WritableBlobSource` (ADR-0008).
- **DeviceParams defaults** (queue depth 128, 128 KiB max IO buf, 512 B
  logical / 4 K physical block, read-only by default) are behavioral
  contract; changing defaults is breaking.
- Not contractual: `user_data` bit encoding, retry counts, internal deque
  types — free to change without notice.

## Testing

Startup-specific ordinary tests include:

- `ublk: ready closure wins after release has read open admission` and
  `ublk: release wins after ready has read idle admission` pin both atomic
  handoff interleavings.
- `ublk: startup observations distinguish cancellation from retirement`
  keeps original and cancellation facts separate.
- `ublk: cold controller leaves the sole blocking worker available` checks
  controller/source progress with one worker of each kind.

These helper/process tests are not kernel retirement proof. The Linux 6.6
START-cancellation and single-FETCH probes cover only their individual cases.
Complete failed multi-queue recovery, late END success, native source loans,
original CQEs, cdev-close-before-DEL and actual process reap require separate
owned-fixture evidence on Linux 6.6 and a current kernel before ADR-0022 is
accepted. No full validation result is asserted here.

The privileged case `integration: supervisor retains failed partial ublk recovery until explicit release`
now exercises the real supervisor/exec'd caller with a two-queue pre-END gate,
actual original/cancellation CQEs, explicit release, native retirement and reap.
It uses opt-in per-instance hooks and preserves unresolved owners rather than
force-killing them on failure. See `docs/testing.md` for event meanings, retained
artifacts and the owned VM runner requirement. Its implementation is not a claim
that the kernel matrix has passed; it covers pre-END partial publication.

The separate `integration: adopted ublk recovery drains source frames before explicit release`
case exercises a successful END followed by a real block request holding an
actual queue buffer in the source coroutine, a local pre-ready failure and
explicit release. Its async gates preserve the native borrower until controlled
release, and its observations distinguish delegate bytes, source scope exit,
queue-owner frame destruction and block-client completion. Runtime results remain
bound to their recorded source head and kernel; this case does not establish
every reporting or release-error variant.

The narrow `integration: retained ublk caller survives report failure and retries refused release`
case combines an exception during the real caller's retained-failure report
construction with a local release-controller setup exception. The first explicit
release settles as refused sequence 1 with `ENOMEM` before any STOP preparation;
a fresh sequence 2 uses the normal kernel teardown and actual reap path. The
fault is a local setup exception, not a fabricated kernel failure. See
`docs/testing.md` for the evidence and one-kernel composition-check boundary.

Pending background fills have component and inspected composition coverage:
`source: concurrent fill parking retains a slow remote past warning deadlines`
and `source: native fill notifications survive allocation failure and stop races`
exercise LayerStore retention; `ublk: retained recovery waits for source drain and explicit release`
retries a throwing cleanup callback. The real caller keeps `OpenedImage` outside
its protected body and passes `park_opened_fills` to Device; after source drain,
`Device::fail_startup` retries this callback, and `park_image_fills` stops then
awaits the retained LayerStores. This credits those tests and the source wiring,
not an executed whole-caller pending-fill scenario. An additional remote-fill
kernel matrix is not a separate acceptance requirement absent a concrete wiring
defect or an explicitly expanded scope.


Unit tests live in `tests/unit/test_ublk*.cpp` (built only when
`OBD_ENABLE_UBLK` is on); the kernel-dependent end-to-end test lives in
`tests/integration/test_ublk_e2e.cpp`.

- `ublk: fixed worker pump retires rejected commands through raw eventfd` —
  launches a real two-worker Elio runtime, creates the raw ring on worker one,
  registers its eventfd, and issues URING_CMD against an eventfd stand-in. The
  rejecting kernel path must wake the native waiter, preserve the recorded
  worker/Linux task, and retire every exact cookie without a ublk device. It
  also pins SINGLE_ISSUER plus DEFER_TASKRUN and exercises the required
  wake-then-GETEVENTS pump. The stand-in does not exercise ublk's
  COMP_IN_TASK local-work path; that conclusion still requires the hosted
  ublk E2E.
- `ublk: STOP preserves a delivered request's COMMIT obligation` — pins the
  late-CQE interleaving: STOP has settled before the owner observes a successful
  FETCH CQE, so the delivered slot must still transition to a prepared
  COMMIT_AND_FETCH rather than becoming unretirable debt.
- `ublk: repeated tag reads stay on the bridge worker` — four runtime workers
  retain source entry/resume on the explicitly selected bridge worker.
- `ublk: foreign result mailbox enforces tag generations` — one preallocated
  tag slot accepts its armed generation exactly once, rejects stale/duplicate
  results and can be rearmed only after owner consumption.
- `ublk: foreign handler destruction publishes only through owner sink` — a
  real `TagEngine` handler migrates to another Elio worker and unwinds there;
  its production guard records only an exact-generation mailbox fact and wake,
  while the selected owner invokes the sink exactly once.
- `ublk: tag engine rejects admission after terminal close` — cold and warm
  reusable tags reject before creating a new obligation; compile-time checks
  pin synchronous admission/close preparation and direct TaskGroup joining.
- `ublk: admission failure still joins the accepted handler` — an exception
  immediately after event publication cannot bypass request drain or the
  handler lifetime fence.
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
- The first eight public startup coordinator cases (`[startup-coordinator]`,
  listed in [testing.md](testing.md#ublk)) use the deterministic legacy-thread
  fixture and cover create/attach setup failures, a paused
  second owner with all-zero publication, the prerequisite-to-START/END
  admission gap, post-START queue failure with sole blocking-pool source
  progress, and the admitted START deadline. Their real coordinator and Ctrl
  observer retain the original after a cancellation CQE, preserve `EIO` or
  `ETIMEDOUT`, and distinguish new-device cleanup authority from explicit
  release of a published, unadopted recovery. The queue rings retire real
  eventfd-rejected commands; scripted control CQEs do not establish driver
  acceptance. A ninth production-topology case,
  `ublk: failed second fixed owner allocation seals its ownerless queue`, forces
  the second join-state allocation to fail. It requires the admitted first
  owner to drain, seals the ownerless queue with zero raw publication, and
  closes both cdevs before one STOP/DEL. These ordinary tests complement the
  privileged #137 fixtures. A tenth production-topology case,
  `ublk: release admission is published while initial FETCH remains held`,
  proves that an accepted sequence becomes observable while initial submission
  still fences STOP. An eleventh,
  `ublk: release admission is visible while failure cleanup remains held`,
  admits after source drain while caller cleanup is suspended and requires the
  mailbox-backed snapshot to expose that same sequence immediately. Both use
  scripted control CQEs and rejecting eventfd queue stand-ins rather than real
  ublk-driver acceptance.
- `ublk: failed startup drains sources before propagating its original error` —
  the production failure coordinator drains partial bridge registration and
  publishes released state before rethrowing the original failure. Source
  graph destruction remains with the owner after the operation resolves.

- `ublk: retained recovery waits for source drain and explicit release` —
  the production Device failure coordinator, bridge and Ctrl command loop
  retain an outstanding source-buffer loan. Neither source completion nor
  cleanup callback exceptions grant release authority. An admitted release
  drains the native frames, performs STOP, closes the stand-in cdev, then
  issues one DEL. A refused STOP retains the graph until another release.
- `ublk: retained release waits for every queue to issue and retire originals` —
  two real queue threads/rings publish URING_CMD against rejecting eventfds.
  Pausing one queue before submission prevents the real Device coordinator
  from issuing STOP; after submission, both original-CQE ledgers must retire,
  both owners must exit, and cdev stand-ins must close before one scripted DEL.
  This proves the coordinator's issuance/retirement ordering with real rings;
  it does not prove ublk-driver STOP behavior.
- `ublk: startup failure keeps the admitted cleanup callback without copying` —
  the public create API handles a pre-admitted release without opening a kernel
  device, invoking a movable callback whose copy constructor throws. The
  original startup frame retains it through failure coordination.
- `ublk: original END completion decides recovery adoption` — scripted CQEs
  through the real Ctrl loop invoke Device's original-completion callback;
  success adopts the old device, failure does not. These composed tests use
  synthetic queue delivery and control transport, not a kernel FETCH ledger.
- `ublk: actual control loop waits for original and cancellation CQEs` —
  actual command-loop tests distinguish original and cancellation CQEs,
  including cancellation ENOENT and original success before cancellation.
- `ublk: actual control loop retains payload across ambiguous and short submissions` —
  scripted submit results preserve the actual command payload until terminal
  completions, including an error after publication and short submission.
- `ublk: actual control loop retains poisoned completion ownership` — unknown,
  duplicate and multishot CQEs cannot end command ownership even after both
  expected completions arrive; bounded kernel-free subprocesses observe this
  intentional retention without unwinding live resources.

- `ublk: control task releases its device owner before final destruction` —
  actual control-loop EOF and frame/capture teardown precede final off-worker
  Device destruction, with one worker and one blocking thread.
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
  mappings, queue rings and fixed owner tasks; each has a 30-second CTest timeout.
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
  that every tag handler relies on.
- `integration: ublk device serves sector reads from a blob` — privileged
  E2E: creates a real `Device` over an in-memory source, opens
  `/dev/ublkb<N>`, and verifies a 4 KiB `pread` at byte offset 1024 returns
  the exact source bytes with one and two hardware queues. Guards the whole
  path: control plane, fixed-worker queue-owner dispatch, zero-copy buffer
  addressing, and teardown. It
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
- **Startup deadline**: owner preparation and START/END retries share a
  five-second monotonic budget. Expiry requests cancellation; it does not
  bound original-command retirement or retained-owner lifetime.
- **Queue failure**: a published queue fault keeps its original owner in
  maintenance. Release can remain unresolved if the original terminal CQEs
  cannot be proven; a sticky failure is not permission to free its resources.
- **Release ingress**: library recovery requires a reachable `StartupControl`.
  Standalone `obd-device` uses the startup control endpoint documented in
  [binaries.md](binaries.md); supervised recovery uses its private channel.
- The supervisor-facing readiness signaling around a started device lives in
  `obd-device` (`src/cmd`), not in this module.
