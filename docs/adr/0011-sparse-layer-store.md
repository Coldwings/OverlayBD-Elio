# ADR-0011: Unify layer persistence into a sparse-file LayerStore with sidecar bitmap and per-extent CRC

- Status: proposed
- Date: 2026-09-08
- Supersedes: none
- Binds: docs/design-assumptions.md, docs/source.md, docs/config.md,
  src/source/chunk_cache.hpp, src/source/downloader.hpp,
  src/source/switch_source.hpp

## Context

Two independent mechanisms currently stand in for overlaybd's file-based
layer cache, and neither can carry the planned prefetch work:

1. **An in-memory LRU chunk cache** (64 KiB chunks, bounded budget). It is
   per-device-process, invisible to siblings, and lost on restart — every
   process restart is a cold start.
2. **A whole-file background downloader with an atomic remote→local
   switch.** The switch is binary: until the entire blob is downloaded and
   verified, every read is served remotely, and the downloader re-reads
   bytes that on-demand reads already fetched (or will fetch). The
   "background download after N seconds" contract inherited from upstream
   is, in the maintainer's words, a good but awkward design.

Prefetch (ADR-0012, ADR-0013) needs a persistence point with a "write to
cache without reading out" verb — warming through ordinary reads would
materialize buffers nobody consumes. The natural shape is one sparse local
file per layer that every byte flows through, so that a layer's dependence
on the remote source shrinks monotonically instead of ending in a single
atomic event.

Two constraints shaped the format. First, **no eviction**: layer
directories are owned and reclaimed wholesale by the container facility
(containerd), so the store must never punch holes to reclaim space; it may
only degrade when the disk is full. Second, **`SEEK_HOLE`-based hole
tracking was explicitly rejected**: query performance on tmpfs is poor,
hole-reporting rules differ across filesystems (xfs vs ext4), and a crash
mid-write leaves a half-written region indistinguishable from a present
one.

## Decision

Each layer gets one sparse staging file plus one small sidecar file; the
pair replaces both the in-memory chunk cache and the whole-file
download/switch mechanism. The rules are:

- **Uniform 64 KiB extent granularity everywhere.** The on-demand remote
  fetch unit, the persistence accounting unit, and the sidecar bitmap bit
  are the same 64 KiB extent. Bulk paths (background fill, prefetch) do
  not issue 64 KiB requests: they coalesce contiguous missing extents into
  larger range reads (capped near 1 MiB) at the source client and split
  the result into extents for accounting.
- **Sidecar layout:** a fixed header (magic, format version, layer digest,
  blob size, extent size, pairing nonce) followed by the extent bitmap and
  a per-extent CRC32 array. Staging file and sidecar are paired by a
  shared nonce in their file names, so a stale sidecar can never be
  attached to a fresh staging file.
- **One consistency rule:** a bitmap bit is set only after the extent's
  data write has completed. Crashes may therefore lose bits whose data
  survived (safe: re-fetch overwrites), never the reverse. Reads of a
  "present" extent verify its CRC32 first; a mismatch demotes the extent
  to a hole and re-fetches from the remote, making torn writes a
  deterministic cache miss instead of silent corruption. The cache path
  performs no fsync.
- **Write-behind, always droppable.** Fetched bytes answer the reader
  first; persistence is an asynchronous, bounded queue. A full queue
  drops writes (they are only cache). `ENOSPC`/`EIO` moves the store into
  a bypass state: no further writes, prefetch and background fill switch
  off, reads continue purely remote. Bypass is a normal degraded mode,
  not an error path.
- **No eviction, ever.** Fullness triggers bypass, never hole-punching.
- **Completion keeps the existing contract:** when every extent is
  present, the staging file is sha256-verified against the image-config
  digest and atomically renamed to the committed layer file; this catches
  any corruption that escaped per-extent checks.
- The kernel page cache is the L1: reads from a present extent are plain
  local preads, so the in-memory LRU chunk cache is retired rather than
  layered on top.

## Consequences

- A layer's remote dependence ends gradually and survives process
  restarts; per-device crash recovery (ADR-0010) no longer implies a cold
  cache.
- The three writers (on-demand write-through, prefetch populate,
  background fill) share one extent map, which is what makes extent-level
  request coalescing (ADR-0012) well-defined.
- Restarted devices and re-created snapshots resume from disk state
  instead of re-warming from the registry.
- The store is a new source-chain component with its own tests
  (bit/CRC pairing, nonce mismatch, bypass transitions, crash-window
  simulation); the retired chunk cache and the downloader/switch sources
  are removed or reduced to compatibility shims, and
  `docs/design-assumptions.md` gains an entry for the single-granularity
  and droppable-write rules (trigger T2 rides the implementation PR).
- `docs/source.md` statements about memory-only caching and whole-file
  switching are rewritten when the implementation lands.

## Alternatives considered

- **`SEEK_HOLE` reconstruction instead of a sidecar.** Rejected: tmpfs
  query cost, filesystem-specific hole semantics, and the crash-window
  ambiguity between "present" and "torn" (the sidecar's CRC answers this
  deterministically).
- **Keep the in-memory LRU in front of the sparse file.** Rejected: the
  kernel page cache already is a larger, pressure-aware LRU; a user-space
  copy adds a memcpy and a second eviction policy for no hit-rate gain.
- **Two-level granularity** (large persisted extents with sub-extent
  coverage masks). Rejected: a uniform 64 KiB unit removes the mask
  entirely; 64 KiB is the maintainer's validated default for covering
  most on-demand patterns, while 1 MiB-class throughput is recovered by
  request coalescing on bulk paths rather than by coarser accounting.
- **Fsync-ordered durability** (data → fdatasync → bitmap). Rejected:
  the cache is rebuildable by definition; synchronous durability would
  tax every read for a crash window the CRC already detects.
- **LRU eviction under pressure.** Rejected: layer-directory lifetime
  belongs to the container facility; partial self-reclamation would
  fight its accounting and reintroduce remote dependence unpredictably.
