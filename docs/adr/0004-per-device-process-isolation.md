# ADR-0004: One isolated process per device

- Status: accepted
- Date: 2026-09-07
- Supersedes: none
- Binds: src/supervisor/, src/cmd/, AGENTS.md

## Context

A lazy-loading block device fronts untrusted remote content: registry blobs,
compressed indexes, and layer metadata that were produced by someone else's
build pipeline. Parsing and serving that content can crash — a malformed
header, a pathological index, a bug in decompression — no matter how
carefully the parsers are written. If one daemon serves every device, every
device shares that blast radius: one poisoned image takes down the block
devices of every container on the host, converting a single bad input into a
node-wide outage.

Isolation therefore has to come from the process boundary, the strongest
primitive the operating system offers for memory and fault separation. The
project accepts the cost of a real supervisor — lifecycle bookkeeping,
status reporting, reaping — as the price of making a device crash a local
event.

## Decision

Run exactly one child process per block device under a supervisor daemon:
devices never share a process, and the supervisor owns the full lifecycle —
spawning, reaping, and a status channel over a socketpair — while control
clients reach the supervisor over a Unix-domain socket.

## Consequences

- A device crash cannot take down its siblings; the fault domain of a
  malformed image is exactly one device.
- Memory and file-descriptor pressure are isolated per device: a leak or an
  FD-hungry image degrades one process, not the fleet, and resource limits
  can be reasoned about per device.
- The price is process-management complexity, concentrated deliberately in
  the supervisor: zombie reaping, reporting exec failures (a child that
  cannot start signals it with exit code 127), and bounded waits for
  ready and stop transitions so a wedged child cannot hang a control
  operation forever.
- The supervisor↔child status channel and the supervisor↔client control
  socket are wire contracts between binaries that may be upgraded
  independently; changes to either follow the ADR process (trigger T1).

## Alternatives considered

- **Single process, all devices.** Simplest to operate, but one bad image
  kills everything — the exact failure mode isolation exists to prevent.
  Rejected.
- **Thread-per-device in one process.** Cheaper context and no reaping, but
  threads share an address space, so a memory-corrupting bug or an abort in
  one device still ends the process and every sibling with it. No memory
  isolation means no fault isolation. Rejected.
- **systemd-managed per-device units.** The kernel's isolation is identical,
  but deployment complexity is pushed onto operators: unit templating,
  instance lifecycle, and socket hand-off become external configuration
  instead of shipped, tested behavior. Rejected — the supervisor keeps that
  complexity inside the project, where it is tested once for everyone.
