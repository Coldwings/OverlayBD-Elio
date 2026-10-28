# ADR-0031: Reuse verified LayerStore extent buffers

- Status: accepted
- Date: 2026-10-28
- Supersedes: none
- Binds: src/source/layer_store.hpp, src/source/layer_store.cpp, docs/source.md

## Context

Partial reads from a present LayerStore extent read and checksum the entire
extent. Small repeated reads therefore repeat local I/O and CRC work on
immutable layer bytes. Issue [#180](https://github.com/Coldwings/OverlayBD-Elio/issues/180)
tracks bounded reuse and its measured benefit.

## Decision

While Filling, retain at most four immutable buffers totaling at most 256 KiB
from successful exact-length, CRC-verified partial local reads. Tail buffers
contain only their actual bytes. Larger extents are excluded. Whole-extent
reads continue directly into the caller buffer and always verify local bytes.

Identify each buffer by extent, packed CRC/presence record, retry generation,
and staging descriptor. Check this identity on lookup and before publication.
A descriptor is essential because retry generation advances before the new
staging file is published. Retired descriptors stay open until destruction.
Clear retained buffers on retry, completion and bypass; invalidate demoted
extents. Shared ownership permits an active reader to finish after eviction.
The cap bounds retained cache payload, not all concurrently active readers.

Cache hits copy previously verified immutable memory. Every actual local disk
read still checks exact length and CRC; this does not promise immediate
observation of subsequent external disk corruption. A miss or whole-extent
read detects corruption and follows the existing demotion/refetch path.
Whole-layer SHA verification, sidecar durability and remote admission remain
unchanged. Optional cache allocation failure must not fail a successful read.

## Consequences

Repeated partial reads can avoid disk I/O and CRC work. Four-entry searches
and a short mutex add overhead; a working set exceeding the bounds may thrash.
No coroutine suspends with the cache mutex held. No cached mutable bytes escape.
Completion and Bypass paths retain their existing read semantics.

## Alternatives considered

- Unbounded decoded extents: rejected because large layers would consume
  unbounded memory.
- Skip CRC on newly read disk bytes: rejected because torn staging writes
  must remain detectable.
- Share one mutable scratch buffer: rejected because concurrent reads may
  suspend and overwrite each other's data.
- Use retry generation alone: rejected because old staging reads can begin
  during the generation-change/new-descriptor publication interval.
