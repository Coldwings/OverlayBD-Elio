# ADR-0010: ublk USER_RECOVERY crash recovery with bounded respawn

- Status: accepted
- Date: 2026-09-08
- Supersedes: none
- Binds: src/ublk/ctrl.cpp, src/ublk/device.cpp,
  src/cmd/obd_device_main.cpp, src/supervisor/child.cpp,
  src/supervisor/daemon.cpp, src/supervisor/protocol.cpp,
  src/image/config.cpp, docs/ublk.md, docs/supervisor.md,
  docs/operations.md, docs/binaries.md, docs/config.md,
  docs/design-assumptions.md

## Context

ADR-0004 isolates each block device in its own process so a crash cannot
take down siblings — but a crash still killed *that device*: the ublk
server was the dying process, so every in-flight and future I/O failed
and the mount became unusable until manual recreation. Kernel 6.x ublk
offers `UBLK_F_USER_RECOVERY` (the device survives its server in a
QUIESCED state) and `UBLK_F_USER_RECOVERY_REISSUE` (the kernel
automatically reissues outstanding I/O to the replacement server). The
supervisor is the natural place to detect the crash and start the
replacement; this also lands the previously-deferred supervisor
auto-restart.

Two design choices were made explicitly:

1. **REISSUE over FAIL_IO**: with `_REISSUE`, I/O that was in flight at
   the crash is re-driven to the new server; the alternative
   (`UBLK_F_USER_RECOVERY_FAIL_IO`) fails it and relies on upper layers
   to retry. Reissue is strictly more transparent for a block device —
   a guest filesystem should observe, at most, latency.
2. **Bounded respawn**: an unhealthy image (bad config, corrupt blob)
   would crash-loop forever without a bound. The supervisor attempts at
   most `max_recovery_attempts` (default 3) replacements per device,
   then leaves the device in its terminal state for operator
   inspection. Intentional destroys never respawn.

## Decision

- Devices are created with `UBLK_F_USER_RECOVERY |
  UBLK_F_USER_RECOVERY_REISSUE` (default on; `ublkConfig.enableRecovery`
  in the global config, default `true`). If `ADD_DEV` rejects the flags
  with `EINVAL` (older kernel), creation falls back to a non-recoverable
  device with a warning rather than failing.
- On an unexpected child exit (not a requested destroy, not daemon
  shutdown), the supervisor respawns the device process with
  `--recover --dev-id N` — the dev id learned from the ready status's
  bdev path by the supervising coroutine itself (closing the race where
  an instant crash would be observed before `create` could record the
  id). `obd-device --recover` re-opens the image and calls
  `Device::attach`: open `/dev/ublkcN`, `START_USER_RECOVERY`, re-park
  FETCH for every tag on every queue, `END_USER_RECOVERY` (EBUSY
  polling like `START_DEV`).
- `list`/`status` replies carry a `recoveries` count (additive wire
  field; older clients ignore unknown fields).
- The daemon shuts down cleanly: the reaper's `signal_fd` is
  constructed inside the pinned (`go_to(0)`) reaper coroutine —
  `signal_fd` caches the creating worker's `io_context`, and
  `sync::mutex` wakeups may otherwise migrate the coroutine to a worker
  where that context is invalid — and `run()` wakes the parked accept
  (with a connection, because `close()` does not cancel an in-flight
  accept SQE) and the parked reaper (with a synthetic SIGCHLD) and
  joins both before returning. Detached tasks must not outlive the
  scheduler; teardown drains forever on parked tasks.

## Consequences

- A device process crash is, from the guest's perspective, a latency
  spike: outstanding I/O is reissued and completes against the
  replacement server; the mount survives.
- Data-loss boundary for writable uppers (carried over from ADR-0008):
  the replacement re-opens the image from disk, so an *unsealed LSMT-RW*
  upper loses its unsealed writes on recovery (creation truncates),
  while a *sparse* upper recovers its extents via fiemap. Operators who
  need crash durability for writes should choose the sparse upper or
  accept the boundary.
- Recovery of a device created without recovery support (fallback
  path) simply fails its attach and consumes one attempt; after the
  bound is reached the device stays down and the event is logged.
- The restart policy is deliberately simple (immediate respawn, fixed
  bound). Exponential backoff or flapping detection can be revisited if
  operations show the need; the bound already prevents hot loops.
