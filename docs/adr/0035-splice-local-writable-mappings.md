# ADR-0035: Splice proven-local writable mapping changes

- Status: accepted
- Date: 2026-10-28
- Supersedes: ADR-0030
- Binds: src/format/writable.hpp, src/format/lsmt_rw.hpp,
  src/format/merged_writable.hpp, src/format/merged_writable.cpp,
  docs/format.md, docs/testing.md

## Context

Issue [#195](https://github.com/Coldwings/OverlayBD-Elio/issues/195) evaluates the
remaining changed-map work after unchanged revision reuse. Rebuilding a full
merged index repeats mapping merge work for sectors unaffected by a successful
small mutation. Exact revision changes alone do not identify an affected range.

## Decision

Preserve ADR-0030's exact revision contract, saturation behavior, unchanged
snapshot reuse and conservative error/exception repair. Add a separate
`WritableLayer::supports_local_mapping_updates()` capability, false by default.
For a fully successful write or discard, it guarantees unchanged resolved
per-sector DATA/ZERO ownership and physical offsets outside the requested
interval, including preparatory/retry work. Segment partitioning may differ.
Only LSMT-RW opts in initially. Its metadata compaction does not move payload
or change ownership; failed/partial operations do not invoke this guarantee.

Under the existing exclusive mutation guard, a complete successful write or
successful discard may splice its interval into a valid old merged snapshot.
Copy the current top snapshot and verify complete coverage of that interval.
If coverage is incomplete, use the full merger to resolve lower ownership.
Otherwise copy unaffected prefix/suffix mappings in bulk, clip intersections,
assign the changed range to the top, and coalesce compatible boundaries while
respecting the mapping length and merged index limits. Publish a new immutable
vector under the snapshot mutex; retained readers keep their existing vectors.

Unknown locality, invalid prior snapshots, partial/negative results and
exceptions retain full rebuilding or invalidation. Allocation failure during
splice construction invalidates the index and returns `-ENOMEM`; the successful
top mutation cannot be rolled back. A later successful operation repairs an
invalidated view by full rebuilding. Exact revision reuse remains the first
choice for an already-valid unchanged snapshot, independently of locality.
No persisted format, payload ordering, durability or read-exclusion rule changes.

## Alternatives

- Infer locality from changed exact revisions: rejected because revisions
  describe equality, not the location of changes.
- Apply a range update to every writable type: rejected because pending
  ownership/retry work may affect other sectors.
- Re-merge every unaffected mapping: the first bounded prototype showed weak
  or negative benefit; bulk-copying the unchanged runs materially reduced
  changed-map write/discard latency in the tested larger-index workload.
- Replace contiguous snapshots with a persistent tree: a separate design,
  unnecessary for this bounded improvement and not claimed here.

## Consequences

The update still performs linear snapshot copying; it reduces merge work,
not asymptotic memory cost. Tests compare the live result with the full merger
and byte models, including multiple lowers, split/capped boundaries, masks,
unknown capability, holes, failed publication and recovery. Performance claims
must distinguish changed mappings from the unchanged-revision control.
