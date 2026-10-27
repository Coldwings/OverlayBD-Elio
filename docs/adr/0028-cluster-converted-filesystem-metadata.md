# ADR-0028: Cluster finalized filesystem metadata in ordinary LSMT payloads

- Status: accepted
- Date: 2026-10-26
- Supersedes: none
- Binds: src/format/writer.hpp, src/format/writer.cpp, tools/ext2_layout.hpp,
  tools/ext2_layout.cpp, tools/obd_convert_main.cpp, docs/format.md, docs/binaries.md

## Context

Issue [#160](https://github.com/Coldwings/OverlayBD-Elio/issues/160) asks for
native conversion to cluster live filesystem metadata immediately before the
ordinary index. Logical-order payload packing scatters this metadata across
remote ranges. Physical proximity alone does not prove a startup improvement;
the existing structural warm-up and compression/cache boundaries must be measured.

The transient packing interface, finalized-filesystem classifier and converter
selection are implemented. The [measurement report](../../benchmarks/metadata-layout.md)
separates complete-view compatibility, controlled measurements and application
workload coverage; geometry alone does not satisfy performance acceptance.

## Decision

Use build-time sector ranges to select payload for stable tail placement.
Preserve all logical bytes and full coverage in the native raw-image writer.
Pack ordinary ranges in ascending logical order, then selected tail ranges in
ascending logical order. Keep ordinary index entries in logical order, updating
only their existing physical offsets. No new flags, tags, versions, sidecars or
runtime annotations describe the classes. Empty ranges retain the old layout.

Validate sorted, nonempty, disjoint ranges and geometry before output mutation.
Coalesce adjacent selected ranges before splitting at wire segment-length
limits. Bound the resulting index before creating or truncating the destination.
Equivalent normalized range sets and identical content/header inputs produce
identical bytes, independent of the range fragmentation supplied by the caller.

The classifier inspects the finalized filesystem after all writes
and final flush/close updates, accounts for actual live block ownership, and keeps
structural and uncertain blocks conservative. Current-tar payload tracking alone
cannot classify a final native filesystem. Differential workflows retain lower
inheritance and must not copy lower-only bytes merely to arrange a tail.
Unsupported classification retains a documented correctness-preserving fallback.
The current classifier reads finalized 4 KiB ext2 allocation/inode bitmaps,
visits each allocated inode once, and records regular-file leaf blocks as DATA.
Directories, symlink blocks, reserved-inode storage, indirect blocks, external
xattrs, superblocks/group descriptors, inode tables and allocation bitmaps stay
FSMETA. Allocated blocks not proven DATA stay FSMETA; free blocks retain their
bytes in the earlier payload class. Structural ownership wins ambiguous claims.

Classification currently supports FILETYPE, LARGE_FILE, SPARSE_SUPER and EXT_ATTR,
with at most 2^26 filesystem blocks and 2^20 inodes. Unsupported geometry/features,
inconsistent references and excessive resulting index fragmentation fall back
to logical packing with a diagnostic. Native conversion exposes
`--lsmt-layout logical|metadata-tail`; logical remains the default. Controlled
and application measurements show workload-dependent gains and warm-up costs,
so tail placement stays opt-in. Explicit layout selection is rejected for
TurboOCI conversion/import, whose differential coverage remains unchanged.

## Consequences

Reader compatibility depends only on existing ordinary mappings. Physical
reordering can increase index fragmentation, conversion cost or compressed size;
these must be measured alongside complete-byte correctness, independent reader
and filesystem checks. Required performance evidence compares old/new placement
with structural prefetch disabled/enabled under equal budgets, including raw,
ZFile and layered workloads. No speedup or completed optimization is claimed by
the presence of this packing primitive.
