# ADR-0025: Resolve device queues and workers together before startup

- Status: proposed
- Date: 2026-10-23
- Supersedes: none
- Binds: docs/design-assumptions.md, docs/config.md, docs/common.md, docs/ublk.md, docs/operations.md, docs/binaries.md, src/common/device_topology.hpp, src/image/config.hpp, src/cmd/obd_device_main.cpp, src/ublk/device.cpp

## Context

Fixed-worker queue ownership removes steady-state thread handoffs while keeping
the raw command issuer stable. Multiple queues can use different workers, but
production startup needs an explicit policy connecting scheduler size to queue
count. A single queue leaves additional runtime workers without queue-owner
work; independently selecting too many queues can instead share owners and
allocate substantial per-device memory. Each device has its own process.

The maintainer selected matched automatic counts, optional extra manual workers,
and an automatic ceiling of eight pairs in [issue #155](https://github.com/Coldwings/OverlayBD-Elio/issues/155).
Recovery must serve an already existing kernel layout even if the process CPU
budget or configuration has changed since creation.

## Decision

Resolve production ublk queue and Elio worker counts once before scheduler startup from `ublkConfig`, enforce workers at least equal to queues, select at most eight matched pairs automatically from the visible process CPU budget, and preserve the existing kernel queue geometry during recovery.

## Consequences

- `ublkConfig.queues` and `ublkConfig.workers` are integers from zero through
  256. Zero means automatic. Both zero select at least one matched pair,
  bounded by eight and the CPU budget. If exactly one is explicit, its
  counterpart matches it. When both are explicit, additional workers are
  permitted and fewer workers than queues are rejected. Explicit counts may
  exceed eight. Unknown configuration fields retain their existing behavior.
- CPU budget combines process affinity with the tightest finite cgroup CPU
  quota visible through the process mount namespace. Fractional quotas round
  up. Unavailable quota information falls back to affinity; an unavailable
  affinity probe falls back to hardware concurrency, then one. This does not
  infer quota restrictions hidden outside the process namespace.
- Configuration is read once before scheduler construction. The same resolved
  counts govern image and blank devices and standalone and supervised entry
  points. Worker count remains fixed while the device serves requests.
- Invalid topology values fail as usage errors before runtime construction.
  Operational configuration and recovery-query errors retain the existing
  owned startup reporting/cleanup path and assembly-diagnostic precedence.
- Recovery queries existing queue count, depth and buffer geometry before
  constructing the runtime. Automatic queues and workers follow that existing
  layout, including layouts above the new-creation automatic ceiling. An
  explicit queue mismatch, insufficient worker count or unsupported geometry
  fails before recovery admission. Geometry is rechecked before START recovery.
- With the default depth and maximum request size, request buffers alone cost
  16 MiB per queue, or 128 MiB at eight queues. Rings, workers, handlers and
  sources add further memory. The automatic ceiling bounds this multiplication;
  manual counts make larger resource commitments explicit.
- Startup logs distinguish requested configuration, resolved queue count,
  actual runtime worker count and the queue count of the serving Device.
  Existing fixed-owner restoration, admission fairness and retirement rules
  remain binding.

## Alternatives considered

- Keep a single production queue while starting a hardware-sized runtime.
  Rejected: it does not expose the implemented multi-queue execution capacity.
- Automatically use every host CPU for every device. Rejected: affinity and
  cgroup limits may give the process a smaller budget, and per-device queue
  buffers grow with the count.
- Require equal counts even in manual mode. Rejected: additional workers can
  provide scheduling capacity for source and control work.
- Recompute queue topology during recovery. Rejected: an existing kernel
  device retains its queue geometry; changing it is a different lifecycle.
