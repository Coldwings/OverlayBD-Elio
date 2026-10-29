# ADR-0037: Reconstruct Sparse ownership from materialized extents

- Status: accepted
- Date: 2026-10-29
- Supersedes: ADR-0024
- Binds: src/format/sparse_rw.cpp, src/format/sparse_rw.hpp,
  src/format/writable.hpp, src/format/merged_writable.cpp,
  src/image/image_file.cpp, docs/design-assumptions.md, docs/architecture.md,
  docs/config.md, docs/format.md, docs/image.md, docs/operations.md,
  docs/testing.md, docs/ublk.md

## Context

Issue [#197](https://github.com/Coldwings/OverlayBD-Elio/issues/197) replaces
normal Sparse sector coverage with hole-aware 4 KiB copy-up. Exact coverage
protects neighboring inherited sectors, but adds metadata publication and can
fragment subsequent reads between upper and lower sources. Whole-block
materialization trades an initial merged read and larger payload write for
simpler ownership and subsequent reads. The total performance benefit requires
measurement, including cold remote lowers.

This decision replaces ADR-0024 for normal Sparse ownership. Its implementation,
current-law updates and validation belong in the same PR.
The old record remains intact as the rationale for the legacy format.

## Decision

Use buffered, hole-aware 4 KiB materialization and filesystem extents for
normal Sparse ownership, without `.coverage`, an equivalent ownership bitmap,
or an additional recovery journal.

The virtual disk is 4 KiB aligned and its backing file covers the full size
before I/O admission. Ordinary writes do not grow or round the file. Explicit
resize is a separate operation maintaining this invariant. The device server
exclusively owns the upper; external mutation and conflicting mmap access are
unsupported. Require FIEMAP-capable backing storage, with ext4/XFS as concrete
validation targets and other
filesystems eligible under the same semantics.
Do not infer eligibility from filesystem names alone or impose a kernel-version
whitelist. tmpfs without the required extent interface is outside deployment.

Admission requires 4 KiB filesystem allocation blocks and host pages. It rejects
file extent-size hints larger than 4 KiB and realtime allocation policies.
An inherited XFS 64 KiB extent hint was observed to allocate UNWRITTEN neighbors
around a single 4 KiB write within a full-sized file; interpreting those as ZERO
would mask untouched lower bytes on reopen. Check the opened inode before
enabling writes; do not silently change its allocation policy.
Smaller filesystem blocks have not established the single-block allocation
error boundary. CoW reservation hints are separate from data-fork ownership
and are not excluded by this check.

A hole represents INHERIT, an initialized extent represents DATA, and an
UNWRITTEN extent represents explicit ZERO. UNWRITTEN is never reclassified as
INHERIT merely because it lacks initialized payload. Recovery first requests FIEMAP_FLAG_SYNC under exclusive, drained ownership.
This waits for buffered data before interpreting UNWRITTEN as ZERO and avoids
classifying stale allocation state over newer cached DATA. Subsequent FIEMAP
batches need no repeated synchronization while mutations remain excluded.
Writeback/query failures stop recovery rather than falling back to an
unsynchronized guess.

Split guest writes at 4 KiB boundaries. A partial block touching inherited space
is reconstructed from the current merged view, including existing DATA and
ZERO, overlaid with caller bytes, and written as a complete block. A full
aligned block skips the merged read. Already materialized blocks need no
repeated copy-up. Partial updates to ZERO must preserve zero neighbors.
The guest remains 512-byte addressable; changing the advertised logical block
size is a separate decision, not a prerequisite for host materialization.

Use ADR-0036's borrowed current-view interface under exclusive mutation
ownership. Preserve its lifetime across suspension and avoid recursive locks.
Readers and conflicting mutations cannot observe intermediate materialization.
Refresh mapping over the complete affected blocks, not just guest sectors.

Recovery scope is device-server process exit with the host kernel and page
cache intact. Whole-machine power loss and kernel crashes are excluded from
this decision and its validation. Completed writes must survive process
restart; interrupted operations retain the supported replay/error boundary
without publishing invalid neighboring bytes. Buffered I/O is allowed and
`O_DIRECT` is not mandatory. Correct filesystem interfaces are trusted;
demonstrated filesystem violations are compatibility/defect findings. The
implementation remains responsible for short writes, errors and truthful
completion reporting.

Advertise write caching and correctly execute guest FLUSH, synchronizing the
upper and required metadata before success and propagating errors. Match FUA
advertisement to implementation. Do not add per-write or recovery-triggered
synchronization solely for the excluded power-loss model. FLUSH cannot repair
an incorrect process-interruption publication protocol.

Preserve ZERO across discard/write-zeroes, partial blocks and NOUNMAP. Full-block
discard uses ZERO_RANGE, falling back to actual zero DATA if unsupported;
partial blocks preserve merged neighbors. Discard can reserve storage and does
not promise physical reclamation. Hole punching must not reveal masked lower
bytes.

This is an unpublished format change without deployed users. Normal Sparse
opens use extent ownership directly; there is no xattr or separate format
marker, and no automatic migration or downgrade protocol. Recreate old
development uppers. Ordinary opens reject old coverage/zero sidecars rather
than silently reinterpret their mixed-sector data. The pre-existing offline
legacy reader remains available for exporting old development artifacts to
sealed LSMT, but does not participate in normal extent-owned operation.
Export/seal preserves the reusable source and can include unchanged lower bytes
that copy-up materialized into the upper.

## Consequences

First partial writes can require lower reads and amplify payload writes;
already-owned writes and later reads can avoid coverage work and fragmentation.
Remote-lower availability becomes relevant to first materialization.

The [bounded comparison](../../benchmarks/sparse-copyup.md) records all twelve
production timing runs and separate submitted-I/O accounting. It shows less
coverage work/read fragmentation in the selected warm workload, but slower
first writes with a delayed lower and slower synchronized reopen. Those costs
are part of this decision; no general remote-performance or scalability claim
is made. Functional ext4/XFS and kernel ublk validation are separate evidence.

This decision does not assert that implementation or validation is complete.
Acceptance requires verification of cache-aware extent reconstruction,
interrupted buffered materialization and ZERO transitions.
Use real ext4/XFS behavior plus directed short/error/process-exit tests,
concurrency tests and end-to-end measurements. A concrete failure must be
resolved explicitly; do not silently substitute coverage, direct I/O or a log.
No power-cut replay or device-persistence-order model is an acceptance gate.

## Alternatives considered

- Keep exact coverage or substitute another ownership bitmap: does not achieve
  the requested retirement of ownership publication machinery.
- Copy up every partial write: repeats work after a block is already valid.
- Infer cached DATA by combining unsynchronized FIEMAP with SEEK_DATA:
  recovery-time FIEMAP_FLAG_SYNC gives a simpler classification boundary without
  adding normal-write synchronization.
- Require direct I/O or a recovery journal for power-loss publication: imposes
  an excluded failure model on this process-recovery design.
- Treat UNWRITTEN as INHERIT or normalize it to holes: destroys explicit ZERO.
- Require 4 KiB guest sectors: conflates guest addressing with host copy-up and
  unnecessarily changes the existing device ABI.
