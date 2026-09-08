# ublk — kernel control plane, per-queue data plane, Elio bridge

## Overview

The `ublk` module is the data plane of overlaybd-elio: it exposes one merged
OverlayBD block view as a real Linux block device (`/dev/ublkb<N>`) through
the kernel **ublk** driver (the ADR-0003 decision: ublk, not tcmu). It has
three layers:

- **Control plane** (`src/ublk/ctrl.hpp`) — synchronous `/dev/ublk-control`
  ioctls driving the device lifecycle: `ADD_DEV` → `SET_PARAMS` → (queues
  park their FETCH commands) → `START_DEV` → … → `STOP_DEV` → `DEL_DEV`.
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
| `READ` | `BlobSource::pread` into the tag's IO buffer; short reads at EOF are **zero-filled** so the device always answers the full request; success result = requested length. |
| `WRITE` | If the root source is a `WritableBlobSource` (writable image, ADR-0008): `pwrite` from the tag's IO buffer; success result = requested length. Otherwise **-EROFS**. |
| `FLUSH` | If writable root: `WritableBlobSource::flush()` (0 or -errno). Otherwise immediate **0** (read-only device, nothing to persist). |
| `DISCARD` | If writable root: `WritableBlobSource::discard()` with mask-with-zeroes semantics (ADR-0009); success result = 0. Otherwise **-EROFS**. |
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
the handshake and `Device::attach` is the replacement-side path: open
`/dev/ublkcN`, `START_USER_RECOVERY`, re-park FETCH for every tag on every
queue, `END_USER_RECOVERY` (same EBUSY polling as `START_DEV`). `ADD_DEV`
falls back to a non-recoverable device (with a warning) when the kernel
rejects the flags with `EINVAL`.

## Public API

### `src/ublk/uapi_compat.hpp`

- `#ifndef UBLK_F_URING_CMD_COMP_IN_TASK` → defined as `(1ULL << 1)` for
  pre-6.0 headers. This is the *only* compat addition today.
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
and remembers the added device for best-effort cleanup. Non-copyable.

- `Ctrl()` — opens `/dev/ublk-control` (`O_RDWR | O_CLOEXEC`); throws
  `obd::error` when unavailable (driver not loaded / no permission).
- `~Ctrl()` — if a device was added: best-effort `STOP_DEV` + `DEL_DEV`,
  then closes the fd. Never throws.
- `uint32_t add_dev(const DeviceParams&)` — `UBLK_U_CMD_ADD_DEV` with
  `UBLK_F_URING_CMD_COMP_IN_TASK` and `ublksrv_pid = getpid()`; returns the
  driver-assigned device id. Throws `obd::error`.
- `void set_params(uint32_t dev_id, const DeviceParams&)` —
  `UBLK_U_CMD_SET_PARAMS` with BASIC | DISCARD types (see the params table
  above). Throws `obd::error`.
- `void start_dev(uint32_t dev_id)` — `UBLK_U_CMD_START_DEV` with this
  process as the ublk server (pid passed in `data[0]`). Throws `obd::error`;
  the kernel answers `EBUSY` until every queue tag has a parked FETCH —
  callers poll (see `Device::create`).
- `void stop_dev(uint32_t) noexcept`, `void del_dev(uint32_t) noexcept` —
  best-effort teardown ioctls; errors are deliberately ignored.
- `static std::string cdev_path(uint32_t)` → `/dev/ublkc<N>`;
  `static std::string bdev_path(uint32_t)` → `/dev/ublkb<N>`.

All `Ctrl` ioctls are **synchronous cold-path** calls — never invoke them on
a coroutine hot path.

### `src/ublk/queue.hpp` — `obd::ublk::IoRequest`, `obd::ublk::Queue`

`IoRequest` — one command extracted from a shared `ublksrv_io_desc`:
`tag`, `op` (`UBLK_IO_OP_*`), `start_sector`, `nr_sectors`;
`byte_offset() = start_sector * 512`, `byte_len() = nr_sectors * 512`.

`Queue(uint32_t dev_id, uint16_t q_id, uint16_t depth, uint32_t
max_io_buf_bytes)` — throws `obd::error(EINVAL)` for `depth == 0` or
`depth > UBLK_MAX_QUEUE_DEPTH`. Non-copyable; destructor tears down ring,
mapping, buffers, fds.

- `void open()` — opens `/dev/ublkc<N>`, mmaps the command buffer at
  `q_id * cmd_buf_stride(...)` with length `cmd_buf_size(depth, 24)`
  (PROT_READ, MAP_SHARED | MAP_POPULATE), allocates and zeroes the per-tag
  IO buffers, creates the two eventfds (`EFD_CLOEXEC | EFD_NONBLOCK`), and
  initializes the ring with `depth * 2` entries. Throws `obd::error`.
- `uint16_t q_id() const`, `uint16_t depth() const`.
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
becomes true (or the efd watch errors out on teardown). Spawn with
`elio::go()` from the device process; one bridge per queue. Waits on
`q->elio_efd()`, drains `try_pop_request`, and fans each request out as its
own `handle_io` coroutine implementing the op dispatch table above.
`-EINTR` / `-EAGAIN` on the eventfd read are retried.

### `src/ublk/device.hpp` — `obd::ublk::Device`

Full lifecycle of one image as one ublk device. Non-copyable.

- `static elio::coro::task<std::unique_ptr<Device>> create(const
  DeviceParams&, source::BlobSourcePtr src)` — **must be awaited on the
  Elio scheduler** (bridge coroutines spawn with `elio::go()`). Order:
  open `Ctrl` → `ADD_DEV` → `SET_PARAMS` → open all queues → launch queue
  threads (they park the FETCH commands) → spawn one bridge coroutine per
  queue → `START_DEV`, retrying `EBUSY` up to 100 × 50 ms (5 s). Throws
  `obd::error(EINVAL)` for a null source or `dev_sectors == 0`, and
  `obd::error` on any setup failure (partial state is torn down via
  `stop()` first).
- `uint32_t dev_id() const`, `std::string bdev_path() const` →
  `/dev/ublkb<N>`.
- `bool started() const` — true once the kernel gendisk is live
  (`START_DEV` succeeded).
- `void stop() noexcept` — signals queue threads and bridges, wakes and
  joins all queue threads. Idempotent.
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
| Control path | caller of `Device::create` (an Elio coroutine) | `Ctrl` ioctls (blocking; cold path only) |
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
  where blocking is acceptable.
- `run_bridge` and `handle_io` are Elio coroutines; all source IO goes
  through the Elio IO backend (no blocking syscalls on the scheduler).
- Inputs are not mutated: `DeviceParams` is read-only after `create`;
  `src` ownership moves into `Device` and outlives all bridges (destruction
  order: `stop()` joins threads before members die).

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

- `ublk: command buffer geometry matches the driver layout` — pins the mmap
  geometry against the driver formulas: stride = 98304 for the 24-byte
  descriptor, `cmd_buf_size(128, …)` = 4096, `cmd_buf_size(4096, …)` =
  98304. Guards against regressions in `cmd_buf_stride` / `cmd_buf_size`
  that would make `ublk_ch_mmap` reject our mappings.
- `ublk: IoRequest byte math is sector based` — pins the 512-byte sector
  conversion (`start_sector 100` → offset 51200; 8 sectors → 4096 bytes)
  that every bridge handler relies on.
- `integration: ublk device serves sector reads from a blob` — privileged
  E2E: creates a real `Device` over an in-memory source, opens
  `/dev/ublkb<N>`, and verifies a 4 KiB `pread` at byte offset 1024 returns
  the exact source bytes. Guards the whole path: control plane, queue
  thread, bridge dispatch, zero-copy buffer addressing, and teardown. It
  **self-skips** when `/dev/ublk-control` is unavailable (no root /
  `ublk_drv` not loaded); the integration test target is registered with
  CTest property `SKIP_RETURN_CODE 4` ("all selected tests skipped"), so an
  unprivileged run stays green. See `tests/CMakeLists.txt`.

Run: `ctest --test-dir build --output-on-failure` (the E2E test requires
root or `CAP_SYS_ADMIN` plus a loaded `ublk_drv` to actually execute).

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
