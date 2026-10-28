# ADR-0033: Reuse repeatedly read decoded ZFile blocks

- Status: accepted
- Date: 2026-10-28
- Supersedes: none
- Binds: src/format/zfile.hpp, src/format/zfile.cpp, docs/format.md

## Context

Repeated partial reads decode an entire immutable compression block each
time. Issue [#190](https://github.com/Coldwings/OverlayBD-Elio/issues/190)
measures this cost and the cost of admitting blocks that will not be reused.
Small memory-source experiments show substantial hot-set savings, but
unconditional admission adds allocation/eviction costs on streaming misses.

## Decision

Cache only successfully decoded single-block partial reads after a repeated
access among sixteen recent miss keys. Each ZFile instance retains at most
eight immutable decoded blocks, no more than 512 KiB of payload, with FIFO
replacement. Cache hits do not update admission history or replacement order.
Transfer scratch ownership into the cache instead of copying a decoded block.

Exact compressed reads, the configured CRC checks, and exact full-block
decode validation precede publication. Never cache errors or partial results.
Whole-block direct reads and multi-block batching bypass this cache. Shared
ownership protects a hit across eviction. Do not hold a cache mutex across
source I/O, decompression, or copying into caller buffers. Cache allocation
failure falls back to ordinary request-local scratch; publication is optional.

## Consequences

Hot partial reads can avoid source reads and decompression. A miss pays bounded
lookup/history costs; admission is a heuristic, not a promised hit rate. The
retention cap excludes buffers still owned by concurrent requests. A hit uses
previously validated bytes and does not promise immediate detection of later
external corruption. Underlying blobs must remain immutable for the source
lifetime, as already required by BlobSource. A miss or whole-block read still
validates newly fetched bytes under the configured verification policy.

## Alternatives considered

- Copy every decoded block into a cache: measured miss costs were excessive.
- Retain every partial decode: ownership transfer reduced copy costs but
  still regressed directed no-hit scans; require repeated access instead.
- Share mutable scratch between requests: unsafe across concurrent awaits.
- Unbounded retention: incompatible with predictable per-device memory use.
- Cache multi-block requests: deferred to preserve existing batched I/O and
  avoid adding lookups/publication work to large sequential requests.
