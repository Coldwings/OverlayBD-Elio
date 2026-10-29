# ADR-0036: Provide the current merged view during copy-up

- Status: accepted
- Date: 2026-10-29
- Supersedes: none
- Binds: src/format/writable.hpp, src/format/merged_writable.hpp,
  src/format/merged_writable.cpp, docs/format.md

## Context

Issue [#197](https://github.com/Coldwings/OverlayBD-Elio/issues/197) requires
hole-aware 4 KiB copy-up to replace normal Sparse coverage authority. Partial
writes need the current merged bytes, including existing upper DATA and ZERO,
not just the immutable lower. Recursively calling public merged `pread` while
holding exclusive mutation ownership would deadlock once reads are serialized.

## Decision

Provide an opt-in, mutation-scoped reader of the current merged mapping under
the existing exclusive merged operation lock. The borrowed reader lives until
the awaited write or discard completes; it must not escape or start detached
work. The mapping snapshot is immutable, but upper payload is not copied:
implementations must read bytes they need before overwriting those locations.
They must not hold a layer lock that `data_source().pread()` reacquires.

`requires_current_view()` is false by default. Opted-in layers receive
`pwrite_with_view` and `discard_with_view`; default implementations forward
without an additional coroutine frame. Opt-in also excludes public merged
readers for the entire mutation. Existing non-opt-in dispatch is unchanged.

Before entering an opted-in mutation, repair an invalid merged index or fail
without calling the layer. The private reader never recursively takes the
merged lock. It preserves DATA, ZERO and holes from the snapshot, propagates
source errors, and returns `-EIO` for an incomplete mapped-source read. Only
mapping holes and explicit ZERO may synthesize zeros; unexpected source EOF
cannot become persisted copy-up content. Logical EOF still follows BlobSource
short-read semantics. The Sparse copy-up caller requires a 4 KiB-aligned disk
and a backing file covering its full size; normal copy-up never extends EOF.

Mapping changes outside the guest interval do not satisfy ADR-0035's local
update contract. Such layers retain full rebuilding after mutation. Existing
exception/error repair and grow admission remain mandatory.

## Consequences

This interface supplies the merged-view boundary; it does not independently
select a persistence format. ADR-0037 governs Sparse extent ownership.

Tests must cover mixed lower/DATA/ZERO reconstruction, strict source failures,
invalid-index repair, suspension lifetime, conflicting reader/writer exclusion
and grow rejection while an operation is active.
