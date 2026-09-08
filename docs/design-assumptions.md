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
Discard follows mask-with-zeroes semantics (ADR-0009): a discarded range
reads back as zeroes and never falls through to the lowers — LSMT-RW
records it as zeroed segments, sparse performs a real punch-hole.

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
