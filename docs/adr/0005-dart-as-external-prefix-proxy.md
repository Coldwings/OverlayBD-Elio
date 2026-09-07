# ADR-0005: DART integrates as an external prefix proxy

- Status: accepted
- Date: 2026-09-07
- Supersedes: none
- Binds: src/source/dart.cpp, src/image/config.cpp, docs/operations.md

## Context

DART (data-accelerator) provides peer-to-peer blob distribution for
container image layers, offloading registry bandwidth in dense clusters.
Taking advantage of it forces a design choice about where the P2P machinery
lives.

Pulling a P2P engine into the device process in-process would import a
large dependency surface — transport stacks, peer discovery, piece
schedulers, seeding policy — together with its failure modes, directly onto
the block-device data path. Every stall, wedge, or resource leak in that
engine would present as block IO latency or failure on a mounted container
image. That is a poor trade for a feature whose benefit is bandwidth
economics, not correctness.

The integration point DART already offers is trivially proxyable: it speaks
plain HTTP with a prefix shape, serving
`GET http://<dart-host>/<prefix>/<full upstream URL>` and returning exactly
what the upstream registry would have returned. The device therefore needs
no P2P protocol knowledge at all; it needs only to decide, per configured
image, whether to talk to the registry directly or to the DART prefix in
front of it.

Upstream overlaybd treats its accelerator the same way: optional, probed at
open time, with silent fallback to direct registry access when the
accelerator does not answer.

## Decision

Integrate DART strictly as an optional, external prefix proxy: when the
P2P configuration is enabled and the proxy passes a bounded reachability
probe, rewrite registry requests to the DART prefix URL, and on any probe
failure, malformed configuration, or disabled configuration, fall back to
direct registry access with a warning.

## Consequences

- P2P is never on the required path. An image that opens successfully with
  DART also opens successfully without it, given the same registry access;
  DART changes performance, never semantics.
- The device behaves identically with and without DART at the semantics
  level: bytes served are the upstream registry's bytes either way, so
  format validation, merge logic, and read behavior need no P2P-aware
  branches.
- No P2P code runs in-process. The device process carries no peer
  discovery, piece scheduling, or seeding machinery, and none of their
  failure modes can stall the IO hot path.
- The reachability probe is bounded: a silently-dropping network must fall
  back to direct reads quickly rather than stall image open behind
  transport retransmits. Fallback is a first-class outcome, not an error
  path.
- Operators enable P2P purely by deploying and configuring the external
  proxy; the device binaries and their dependency set are unchanged.

## Alternatives considered

- **Embed a P2P engine in-process.** Rejected: it would place a large
  dependency surface and its failure modes on the hot IO path of every
  device, for a feature whose value is bandwidth economics rather than
  correctness.
- **Make DART mandatory.** Rejected: it breaks standalone and
  registry-only deployments where no proxy exists, and upstream overlaybd
  itself treats P2P acceleration as optional; requiring it would diverge
  from the ecosystem's operator expectations.
