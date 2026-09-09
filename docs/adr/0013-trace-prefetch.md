# ADR-0013: Record and replay prefetch traces in upstream-compatible format without a photon dependency

- Status: accepted
- Date: 2026-09-09
- Supersedes: none
- Binds: docs/image.md, docs/source.md, docs/config.md, docs/format.md

## Context

Upstream OverlayBD's trace prefetch is the strongest known answer to
cold-start latency: record the block-layer I/O pattern of a container
start (per layer blob, time-ordered offset+length records), store the
trace as an extra image layer that is always the uppermost, and replay it
at device bring-up so recorded ranges land in cache before the guest
asks. Upstream's published numbers (25 Mbps cross-region link) show cold
start improving from ~60 s to ~15 s. The mechanism's design facts, from
upstream documentation:

- the trace blob travels as an independent image layer, transparent to
  snapshotters; the storage backend must recognize it and replay;
- records are exact byte ranges from the layer blob's perspective, not
  files — finer-grained than stargz's prioritized-file landmarks;
- when building a new image from a base that carries a trace layer, the
  old trace layer must be removed (a new one may be recorded later).

The maintainer requires **full bidirectional compatibility**: this
project must consume traces produced by upstream tooling and produce
trace layers that upstream runtimes can replay. Upstream serializes its
trace with photon's RPC serializer; the maintainer explicitly does not
want a photon dependency here (design assumption A10 keeps the dependency
surface minimal), and judges the format simple enough to analyze from
code and re-implement.

## Decision

Implement trace record and replay with the upstream trace blob as the
interchange format, serialized and parsed by a hand-rolled, dependency
free codec. The supporting rules are:

- **Specification before code.** The first implementation step reads the
  upstream overlaybd and photon serialization sources and lands
  `docs/trace-format.md`: a byte-exact specification of the trace blob
  (this is a wire-format contract, trigger T1). If the format proves too
  entangled with photon to re-implement faithfully, the design returns to
  the maintainer before any codec is written; the premise of this ADR is
  that it will not.
- **Replay path:** a trace layer is recognized as the uppermost layer per
  upstream convention; its records are translated per layer blob into
  populate calls (ADR-0012) in recorded order, at scavenger priority,
  deduplicated against on-demand reads by the admission funnel. Replay is
  opportunistic: a missing, malformed, or stale trace never blocks or
  fails device bring-up — the structural head/tail warm-up (ADR-0012)
  remains as the floor.
- **Record path:** recording taps the source chain at the remote-read
  site, which *is* the layer-blob perspective upstream records from; no
  ublk or filesystem visibility is required. Records carry offset,
  length, and ordering; adjacent records may be coalesced within a bounded
  window as long as replay order is preserved.
- **Record control travels the supervisor protocol** (new
  additive commands, per the ADR-0014 protocol-evolution rule): start with
  an output path and a supervisor-side duration bound, so a dead client
  never leaks a recording device; stop returns the trace file's digest
  and size. Packaging the trace blob as an image layer and pushing it is
  the external CLI's job (ADR-0014), mirroring upstream's split where
  `ctr record-trace` — not the runtime — does packaging.
- **Operational conventions are inherited and documented:** trace layer
  always uppermost; old trace layers removed when deriving images;
  recording happens against a throwaway container, not production.
- A simplified native trace variant may be added later for internal
  experiments, but the upstream format remains the only interchange
  representation this project emits by default.

## Consequences

- Cold-start latency for re-started images approaches warm-start for
  traced workloads, and replay traffic rides the scavenger class, so a
  bad trace costs bandwidth, never miss latency.
- The codec and its golden tests pin a new wire format (golden vectors
  must come from upstream-produced traces, per the byte-exact
  compatibility rule in `docs/design-assumptions.md` A4).
- Image assembly learns to recognize and set aside the trace layer
  instead of merging it into the block view — a visible change to
  `docs/image.md` semantics when the implementation lands.
- The maintainer's dropped alternative (prioritized-files mode) stays
  dropped; this ADR is its anti-relitigation record (trigger T5).

## Alternatives considered

- **Vendoring photon for serialization fidelity.** Rejected: an entire
  coroutine runtime as a dependency to encode (offset, length) pairs
  fails A10's minimal-surface rule; the fork risk of tracking upstream's
  photon for one serializer is worse than maintaining a small codec.
- **A simplified, incompatible trace format only.** Rejected: it strands
  every image already carrying an upstream trace layer and every tool
  that produces them; compatibility was the maintainer's explicit
  requirement.
- **Prioritized-files prefetch (upstream's other mode).** Rejected: it
  requires parsing ext4 inside the runtime, works only for ext4 images,
  and its value is largely covered by structural head/tail warm-up plus
  trace replay.
- **Recording at the ublk request layer.** Rejected: block requests
  interleave across layers after merging, while the trace contract is
  per layer blob; the source-chain tap is the semantically correct and
  simpler point.
