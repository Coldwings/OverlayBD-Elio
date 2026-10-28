# ADR-0030: Reuse writable snapshots only with exact unchanged mapping revisions

- Status: accepted
- Date: 2026-10-28
- Supersedes: none
- Binds: src/format/writable.hpp, src/format/lsmt_rw.hpp,
  src/format/lsmt_rw.cpp, src/format/merged_writable.cpp, docs/format.md

## Context

Issue [#178](https://github.com/Coldwings/OverlayBD-Elio/issues/178) addresses
index work on LSMT in-place DATA overwrites. The payload and mappings already
exist, but the writable layer reconstructs its vector and the merger rebuilds
its snapshot. The existing best-effort generation supports flush/grow
notifications and cannot prove that a successful write left all mappings
unchanged. A failed earlier rebuild may also leave no usable snapshot.

## Decision

Reuse a valid merged snapshot after a successful write only when an opt-in
exact mapping revision is available and equal before and after that write.

`WritableLayer::segments_revision()` returns unknown by default. For one live
instance with caller-serialized mutations, equal available revisions prove
that all segment fields stayed unchanged between observations. This proof
concerns mapping contents, not payload bytes, durability, other instances,
or concurrent unsynchronized mutation. Existing `segments_generation()`
semantics remain unchanged.

LSMT-RW opts in. It advances the revision before live mapping mutations,
including potentially partial updates and seal relocation. It permanently
returns unknown at counter saturation, avoiding wraparound equality. Wholly
in-place DATA overwrites retain the existing mappings without reconstruction.

The merger still rebuilds for unknown or changed revisions, negative write
results and a previously invalidated snapshot. It rebuilds or invalidates
before propagating exceptions, preserving partial-publication handling.
Other writable formats keep the conservative default. No payload ordering,
wire format, durability or read/write serialization rule changes.

## Consequences

Successful LSMT in-place overwrites avoid index-sized vector and merged-view
work. Ownership changes still require full rebuilding. New opt-in writable
implementations must account for every live mapping mutation and saturation;
unknown remains a safe fallback. Tests must cover both publication and the
repair of an invalidated snapshot, not only successful cached writes.

## Alternatives considered

- Reuse the best-effort generation: rejected because unchanged values do not
  prove write mappings unchanged for existing implementations.
- Infer stability from vector address or size: rejected because in-place
  updates and allocator reuse can preserve those values while mappings change.
- Compare full mapping vectors: correct under serialization but retains
  index-sized work on the hot path.
- Incrementally update changed ranges: separate work requiring a precise
  affected-range contract; it is not claimed by this optimization.
