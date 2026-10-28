# ADR-0034: Allow concurrent Hybrid reads with exclusive mutation

- Status: accepted
- Date: 2026-10-28
- Supersedes: ADR-0026
- Binds: src/format/hybrid_writer.hpp, src/format/hybrid_writer.cpp,
  src/format/merged_writable.hpp, src/format/merged_writable.cpp,
  src/format/writable.hpp, docs/format.md

## Context

Issue [#192](https://github.com/Coldwings/OverlayBD-Elio/issues/192) measures
Hybrid readers serialized twice: by the merged snapshot guard and by the
upper payload engine. Warm local-file measurements show useful throughput
improvement at concurrent queue depth, with a small single-reader cost.

## Decision

Carry forward every format, recovery, publication, durability, ownership and
lifecycle rule in ADR-0026. Replace only the implementation's mutually exclusive
read scheduling with shared read ownership. Use the pinned Elio shared mutex
at both the merger and the Hybrid writer. Keep a merged read's shared ownership
from grow admission and snapshot selection through all physical payload reads.
Direct logical and physical writer reads also hold shared ownership. All
mutations, FLUSH, checkpoint, compaction and seal remain exclusive; grow keeps
its existing nonwaiting rejection when operations are active. Lock order remains
merged operation, writer operation, then short snapshot mutexes.

`serialize_merged_reads()` means exclusion against mutations and grow admission;
it does not require readers to exclude one another. Other tops keep their
existing unguarded read behavior. Once a top belongs to a merger, mutation must
use that merged interface; direct lifecycle operations require drained merged IO.
Separate snapshot and physical-read calls do not create a read transaction.

Use native shared/unique RAII guards. Waits do not gain automatic cancellation;
callers must retain objects and buffers until operations complete. Exception
unwinding releases ownership. Elio's queued writer preference prevents incoming
readers from indefinitely bypassing a writer; sustained writers may delay reads.

## Alternatives

Changing only the merged guard leaves the writer's payload serialization.
Removing guards would permit payload overwrites against an old ownership
snapshot. A custom lock would duplicate an existing Elio primitive. Retaining
exclusive reads avoids the single-reader cost but forfeits measured concurrent
throughput. This decision does not change persisted formats or claim faster
cold storage, remote reads, or ublk throughput.

## Consequences

Read/read overlap needs directed witnesses at both layers. Existing exclusion,
recovery and format tests continue to apply. Directed tests hold multiple
readers, queue mutation, verify writer preference, drain on failure, and verify
shared-guard release after errors. Final measurements must bind to the reviewed
implementation; short shared-host measurements are not universal guarantees.
