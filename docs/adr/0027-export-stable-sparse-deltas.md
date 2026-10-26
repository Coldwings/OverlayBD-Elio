# ADR-0027: Export stable sparse ownership as an independent LSMT delta

- Status: proposed
- Date: 2026-10-26
- Supersedes: ADR-0014
- Binds: src/format/sparse_rw.hpp, src/format/sparse_rw.cpp,
  src/format/sparse_export_plan.hpp, src/format/sparse_export_plan.cpp,
  src/format/sparse_export_output.hpp, src/format/sparse_export_output.cpp,
  src/image/image_file.cpp, src/supervisor/protocol.hpp, src/supervisor/protocol.cpp,
  src/supervisor/daemon.cpp, src/cmd/obd_device_main.cpp, docs/format.md, docs/supervisor.md,
  docs/operations.md, docs/binaries.md, docs/design-assumptions.md

## Context

Issue [#161](https://github.com/Coldwings/OverlayBD-Elio/issues/161) requires
exporting exact sparse ownership into a separate sealed LSMT layer. Exporting
the entire raw virtual file would incorrectly replace inherited parent bytes
with zeroes. An instance-local checkpoint alone also cannot exclude another
writer that independently opened the same inode.

This record replaces ADR-0014's unconditional sparse-commit rejection. Its
offline commit boundary, deterministic output, grow-only resize, and external
CLI decisions remain in force, including ADR-0019's converter backend update.

## Decision

Export a stable, exclusively held sparse source as an ordinary deterministic
LSMT delta, copying DATA, encoding explicit ZERO, and omitting INHERIT.

Every ordinary Sparse open acquires a nonblocking exclusive advisory lock on
the data inode before recovery, metadata installation or growth. Hold it until
the layer's destruction, including after a successful terminal checkpoint.
Separate opens through hardlinks contend on the same inode. Return `EBUSY`
when another cooperating project writer owns it. This is not protection from
arbitrary external writes or pathname replacement; operators must keep source
paths and sidecars stable while a layer owns them.

The existing-only recovery entrypoint opens and pins a regular data file
without creation, derives capacity from that descriptor, and uses the existing
exact-coverage and compatibility-mask recovery rules. A missing source fails
without creating data or sidecars. An empty or invalid existing source is
rejected. Recovery can perform the same supported metadata repairs/import as
ordinary open; existing-only does not mean read-only.

The exporter must reach a successful persistence/checkpoint boundary before
snapshotting mappings and copying payload. Checkpoint refuses active operations
and preserves its existing retry behavior on failure. Standalone export owns
and closes its own instance; it does not freeze another caller's instance.
The source remains reopenable read-write after the exporter closes it.

Normalize logical ownership independently of write history, copy live payload
in bounded chunks, and derive deterministic bytes/identity with explicit
virtual-size, parent and tag inputs. Output is a separate standard LSMT artifact,
with no dependency on sparse sidecars for owned contents. Stage and sync it
before atomic publication; reject source data/metadata aliases before output
mutation. Publish with a no-replace rename; preserve an already published
artifact when its directory sync fails. Retry accepts an existing destination
only after full byte equality with the deterministic candidate and renewed file
and directory synchronization; conflicting content returns EEXIST unchanged.
Unpublished temporary files left by abrupt process exit are not authoritative.
Supervisor integration must use its existing stop/reap boundary. Source
quiescence and publication are required even when called outside the supervisor.

Protocol version 6 adds the `sparse_export` feature, optional commit `output`,
and child-ready `upper_parent_uuid`. Missing identity differs from a known
empty bottom-layer parent. Capture the UUID from the actually opened top data
layer, excluding acceleration layers; compare ready generations and refuse
missing or changed identity before stopping a child. Capture source paths at
create time. Sparse commit exports to a separate default sibling `.lsmt` file
or the explicit output, using the existing stop/reap boundary. An optional
aligned grow-only virtual size changes exported geometry without growing the
source. The newly declared tail remains inherited.

## Consequences

Two simultaneous cooperating sparse owners are rejected instead of silently
mutating shared ownership metadata. Existing callers must release their layer
before reopening it. Export does not scan inherited virtual capacity and does
not require Hybrid. Tests must distinguish DATA/ZERO/INHERIT with nonzero
parents, exercise independent readers, deterministic histories, publication
faults, source reuse, aliases, process recovery and the actual operator paths.
Acceptance includes the standalone CLI and supervisor workflow, ordinary tests,
independent upstream readback, and sparse-capacity scaling measurements. The ADR
remains proposed until the carrying PR merges.
