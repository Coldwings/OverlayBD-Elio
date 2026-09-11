# Design Assumptions (Current Law)

This document is the **top-precedence current-law document** of
overlaybd-elio (see the precedence order in `AGENTS.md`). It records the
minimalist cross-module assumptions every module relies on (ADR-0001). An
assumption written here is contract: code that violates it is a bug, and
adding, dropping, or weakening an entry requires an ADR (trigger T2).

Entries describe the rule **as it stands today**; the history behind a rule
lives in `docs/adr/`.

## A1. Read path first, writes are layered on top

The system is a read-optimized lazy-loading block device. Everything below
the topmost layer is immutable: sealed lowers are never modified, and
remote blobs are treated as immutable content addressed by digest. Writes
exist only through the writable upper mechanism (A8) and never reach a
lower or a registry.

## A2. Elio coroutines on all IO paths

All IO on request paths runs as Elio coroutines against the Elio IO
backend (ADR-0002). No coroutine ever blocks on a synchronous syscall;
synchronous syscalls are confined to setup/cold paths. The ublk queue
threads are the single documented exception: they are plain threads that
own their rings and hand work to the scheduler (A7).

## A3. Sector alignment

Device-visible reads and writes are 512B-sector aligned. The ublk request
granularity guarantees it; every format layer may assume it and reject
unaligned requests with `EINVAL`. Holes in any layer read as zeroes.

## A4. Format compatibility is byte-exact

ZFile and LSMT parsing follows the upstream OverlayBD on-disk layouts
byte-exactly; images built by upstream tooling must keep loading
identically. Golden values in tests are sourced from upstream format
semantics (cross-validated independently), never from this project's own
writers alone. Any format-semantics change is breaking (ADR trigger T1).

## A5. Errors: exceptions on cold paths, negative errno on hot paths

Setup and assembly failures throw `obd::error` / `obd::format_error`. Hot
IO paths (`pread`/`pwrite` of sources and layers) return negative `-errno`
values and never throw. A device that cannot assemble must not come up
half-broken: assembly failure fails the create request.

## A6. One process per device

Each block device is served by exactly one child process spawned by the
supervisor (ADR-0004). Devices never share a process; a device crash
cannot take down siblings. The supervisor owns lifecycle over the
documented wire protocols and never serves device IO itself. A crashed
device process is replaced via ublk USER_RECOVERY with a bounded respawn
(ADR-0010): the kernel keeps the device QUIESCED and reissues outstanding
IO to the replacement, so a crash is a latency spike, not a mount failure.

## A7. Ring ownership and the bridge

Each ublk per-queue ring is owned exclusively by its queue thread; the
Elio scheduler never submits to a queue ring (ADR-0006). Requests cross
into the scheduler through the eventfd bridge; completions cross back
through the per-queue completion queue.

## A8. Writable uppers are optional and copy-on-write

A writable upper engages only when the per-image config names it
(`upper.dir` non-empty); otherwise the device is read-only (ADR-0008). Two
sanctioned types exist: `sparse` (fiemap-recovered sparse file) and
`lsmt` (in-place-edit LSMT-RW that seals into a standard LSMT lower).
Writes are copy-on-write into the upper; unsealed LSMT-RW data is durable
only at flush level and is not recoverable across restarts until sealed.
A graceful device shutdown checkpoints the LSMT-RW index into the file so
the supervisor can seal the upper offline (`commit`, ADR-0014); sparse
uppers never seal.
Discard's intended device contract is mask-with-zeroes semantics
(ADR-0009): a discarded range should read back as zeroes rather than fall
through to the lowers. LSMT-RW satisfies this by recording zeroed segments;
sparse satisfies it by first publishing sidecar zero-mask metadata for
lower-layer masking, then punching holes as a best-effort space reclamation.
`flush()`/`checkpoint()` sync the sparse data file and republish dirty sidecar
state after later writes or grows.

## A9. DART is never on the required path

P2P acceleration through DART is optional (ADR-0005): disabled config,
malformed config, or an unreachable proxy all degrade to direct registry
access with a warning. No P2P engine runs in-process.

## A10. Minimal dependency surface

Dependencies are fetched by CMake FetchContent and pinned (Elio by commit
in the top-level `CMakeLists.txt`). New dependencies need clear
justification in review. The kernel ABI surface is limited to ublk and
io_uring; `<linux/ublk_cmd.h>` is never redefined — only `#ifndef`-guarded
additions for what a given header version lacks.

## A11. Layer persistence: one extent map, droppable writes, no eviction

Every remote layer with a per-layer directory persists through one
sparse-file `LayerStore` (ADR-0011). The cross-module rules:

- **Single granularity** — one uniform 64 KiB extent is the remote-fetch,
  persistence-accounting, and sidecar-record unit at once. Bulk paths
  (background fill, prefetch) do not issue 64 KiB requests: they coalesce
  contiguous missing extents into larger range reads (capped near 1 MiB)
  and split the result back into extents for accounting.
- **Bitmap-after-data** — a sidecar bit is set only after the extent's
  data write completed, and local reads verify the extent's CRC32 first:
  crashes and torn writes degrade to re-fetches, never to bad data.
- **Write-behind is always droppable** — fetched bytes answer the reader
  first; a full persistence queue drops writes (they are only cache).
  `ENOSPC`/`EIO` bypasses the store: writes and background fill stop,
  reads continue remotely — a normal degraded mode, not an error path.
- **No eviction, ever** — layer directories are owned and reclaimed
  wholesale by the container facility; the store never punches holes to
  reclaim space.
- **Persistence never gates reads** — an unusable layer directory
  degrades the layer to remote-only reads at assembly (ADR-0016), and a
  layer without a directory has no persistence at all. The
  completed-layer file is `<dir>/overlaybd.commit`, installed by atomic
  rename after sha256 verification.

## A12. All remote reads pass one admission funnel; prefetch/fill are scavengers

Every remote range request of a device is admitted through a single
per-device funnel before reaching the source client (ADR-0012). Priority
is enforced entirely client-side — registries, object storage, and DART
peers offer no QoS, and splitting traffic classes across sources is
rejected (ADR-0012 records the maintainer's decision: every class must
keep flowing through P2P). The rules:

- **On-demand reads are unconditional** — a guest-blocking miss is
  admitted immediately, even past the concurrency window; it is never
  delayed to protect the window.
- **Prefetch (structural warm-up and trace replay) and fill are a
  scavenger class** — admitted
  only when no on-demand request is in flight and total in-flight
  requests are below an AIMD window. Prefetch outranks fill within
  the class. The window is not operator-configured: it grows additively
  while observed on-demand latency stays flat against an EMA baseline
  and shrinks multiplicatively on a latency rise (LEDBAT-style — consume
  spare capacity, yield on the first congestion signal).
- **Scavenger requests are size-capped near 1 MiB**, bounding the
  head-of-line delay an arriving on-demand read can suffer behind an
  already-issued scavenger request.
- **Extent-granular dedup happens below the funnel** (A11's in-flight
  map): a request for an extent already being fetched joins that fetch,
  whatever class started it. Queued scavenger admission is not published
  as in-flight work, so a later on-demand miss never inherits prefetch's
  wait or local skip result.
- **Source clients stay class-agnostic** — the funnel governs admission,
  never source selection (A9's fallback semantics are untouched).
