# ADR-0012: Admit all remote reads through one priority funnel with scavenger-class prefetch

- Status: accepted
- Date: 2026-09-08
- Supersedes: none
- Implemented-by: [#26](https://github.com/Coldwings/OverlayBD-Elio/pull/26)
- Binds: docs/design-assumptions.md, docs/source.md, docs/ublk.md,
  docs/config.md, src/source/registry.hpp, src/ublk/elio_bridge.hpp

## Context

Three traffic classes compete for one resource: on-demand reads (a ublk
request missed every local tier and is blocking a guest), prefetch reads
(trace replay or structural warm-up — useful soon, useless if they delay
a miss), and background fill (completing the layer — useful eventually).
The resource they share is the throughput of the *same* data source, and
that source offers no priority mechanism: neither a registry, nor object
storage behind it, nor DART peers can be asked to serve one class first.

A traffic split between sources was considered and rejected by the
maintainer: in large-scale container starts, both on-demand and prefetch
traffic must flow through P2P (DART) to spread hotspot load; sending any
class directly to the registry re-creates the registry/object-storage
bottleneck the P2P layer exists to remove. Priority must therefore be
enforced entirely client-side, at the point where remote requests are
admitted — and a static budget is wrong because source capacity is
unknown and time-varying.

The maintainer's latency observation anchors the granularity: one remote
read's latency almost always dwarfs disk latency, so the minimum remote
read unit must be larger than one block; 64 KiB covers most scenarios,
1 MiB buys throughput at the cost of on-demand purity. ADR-0011 fixes the
uniform fetch/persist unit at 64 KiB; this ADR fixes who gets to issue
the next remote request.

## Decision

All remote range requests of a device — regardless of class — pass
through a single admission funnel before reaching the source client
(registry or DART). The funnel's rules are:

- **On-demand requests are admitted immediately and unconditionally**,
  even if that momentarily exceeds the concurrency window. Delaying a
  guest-visible miss to protect a window is never correct.
- **Prefetch and fill are a scavenger class.** They are admitted only
  when no on-demand request is in flight and total in-flight requests are
  below an adaptive window. The window is not configured; it is
  additively increased while observed on-demand latency stays flat and
  multiplicatively decreased when latency rises (a LEDBAT-style scavenger
  that consumes only spare capacity and yields on the first congestion
  signal). Trace replay outranks background fill within the class,
  because traced data is needed soon while fill has the whole runtime.
- **Scavenger requests are size-capped** (near 1 MiB, assembled by
  coalescing contiguous missing 64 KiB extents), which bounds the
  head-of-line delay an arriving on-demand read can suffer behind an
  already-issued prefetch request.
- **All classes deduplicate at extent granularity.** The funnel consults
  the LayerStore's in-flight map (ADR-0011): a request for an extent
  already being fetched joins the in-flight fetch instead of issuing a
  duplicate, whatever class issued the first one.
- The source chain grows a **populate(offset, len) verb** — "fetch into
  the local store without delivering data to the caller" — with a default
  no-op implementation; the LayerStore-backed layer implements it, and
  prefetch machinery speaks only this verb, never read-and-discard.
- **Structural prefetch needs no trace:** at device bring-up, a bounded
  window at the head and the tail of each layer (index regions, tar
  headers, filesystem metadata neighborhoods) is populated at scavenger
  priority. It is the cold-start floor; trace replay (ADR-0013) refines
  it and is absent-or-stale tolerant.

Because persistence is asynchronous and droppable (ADR-0011), the funnel
is one-dimensional: classes contend only for remote admission, never for
disk.

## Consequences

- On-demand latency is invariant under prefetch load by construction;
  prefetch throughput automatically expands into idle capacity and
  collapses under contention, with no operator tuning.
- The funnel is a cross-module convention (trigger T4): it sits between
  the ublk bridge's source chain and the remote clients, so every remote
  read path — present and future — is bound by it, including DART
  (ADR-0005 keeps its fallback semantics; the funnel governs admission,
  not source selection).
- Prefetch through DART makes the node a seeder for the extents it
  pulled, a positive externality for concurrent starts on other nodes.
- In-flight prefetch requests are not cancelled on a miss in v1; the
  size cap bounds their damage. Cancellation remains a documented
  refinement if measurements demand it.
- Config gains a small `prefetch` surface (enable, structural window
  size, fill enable) whose schema lands with the implementation PR
  (trigger T1 for config semantics).

## Alternatives considered

- **Registry/DART traffic split by class.** Rejected by the maintainer:
  bypassing P2P for any class re-centralizes hotspot load on the
  registry or its object storage.
- **Static concurrency budgets per class.** Rejected: source capacity
  varies by deployment, time of day, and P2P hit rate; a static number
  is simultaneously too small (wasted idle capacity) and too large
  (miss latency regressions). The AIMD window replaces the number with
  a measurement.
- **Priority queues inside each remote client connection.** Rejected as
  insufficient: it orders what has been admitted but cannot prevent
  scavenger admissions from saturating the source; admission control is
  the only lever that works against a QoS-less peer.
- **Cancelling in-flight prefetch on competing misses.** Deferred: adds
  request-lifecycle complexity whose benefit is bounded by the
  already-small size cap.
- **Read-and-discard warming via ordinary pread.** Rejected: it
  allocates buffers and copies data nobody consumes, and it bypasses
  extent accounting; the populate verb exists precisely to avoid it.
