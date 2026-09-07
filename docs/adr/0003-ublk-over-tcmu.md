# ADR-0003: ublk, not tcmu, is the block-device backend

- Status: accepted
- Date: 2026-09-07
- Supersedes: none
- Binds: src/ublk/, AGENTS.md

## Context

Serving an image as a host block device requires a kernel mechanism that
forwards block IO to a userspace process. The realistic candidates are tcmu
(the choice of upstream OverlayBD), NBD, vhost-user, and ublk (mainline since
kernel 6.0).

ublk speaks io_uring uring-cmds from userspace: each device exposes per-queue
command rings, requests arrive as uring-cmd operations, and completions return
through the same ring. This is the same completion model the rest of the data
plane already uses, it scales per-queue, and it is the direction the kernel
community is actively developing for userspace block devices. tcmu's
userspace path, by contrast, is effectively in maintenance: functional, but
not where performance work or new capability lands.

The deciding constraint is that this project exists to be a fast lazy-loading
device, so per-IO overhead on the dispatch path is the metric that matters;
deployment on kernels older than 6.0 was accepted as out of scope.

## Decision

Make ublk the only block-device backend: devices are added through
`/dev/ublk-control`, parameterized, and served by per-queue io_uring rings,
and neither tcmu nor NBD is supported.

## Consequences

- A running deployment requires a kernel of at least 6.0 with the ublk driver
  loaded; this is a documented prerequisite, and users on older kernels are
  unsupported rather than degraded onto a second backend.
- The queue-thread/ring ownership model that ublk imposes — each queue ring
  owned by exactly one serving thread — follows as its own decision and is
  recorded in ADR-0006.
- All block-backend effort concentrates on one code path: there is no
  abstraction layer hedging between backends, and no second backend's quirks
  leaking into the image stack.
- Tests that need real ublk require privileges and the kernel driver, so the
  default test run must self-skip without them rather than depend on host
  state.

## Alternatives considered

- **tcmu.** Upstream OverlayBD's backend. Its userspace path is stagnant
  upstream and carries higher per-IO overhead than uring-cmd dispatch;
  choosing it would mean inheriting a maintenance-mode interface on the
  hottest path in the system. Rejected.
- **NBD.** Socket-based request ferrying with worse locality and no zero-copy
  path comparable to ublk's shared-ring design; the socket also inserts a
  transport the block layer never needed. Rejected.
- **vhost-user.** Designed for handing devices to virtual machines, with a
  control plane built around that world; it is the wrong fit for exposing
  block devices to containers on the host. Rejected.
