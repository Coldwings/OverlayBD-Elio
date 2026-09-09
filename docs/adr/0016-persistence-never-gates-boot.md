# ADR-0016: Layer persistence is best-effort — an unusable layer dir degrades to remote-only, never fails assembly

- Status: accepted
- Date: 2026-09-09
- Supersedes: none
- Binds: docs/image.md, docs/config.md, docs/design-assumptions.md,
  src/image/image_file.cpp

## Context

ADR-0011 part 2 wired every dir-configured remote layer through the
`LayerStore` and made a `LayerStore::open` failure (missing, unwritable,
full, or otherwise unusable `lower.dir`) a fatal image-assembly error.
That is a behavior regression against the pre-ADR-0011 mechanism, whose
background download degraded non-fatally: an operator whose cache disk
is full, read-only, or misconfigured cannot boot an otherwise perfectly
pullable image. Block devices are on the boot path of containers; a
cache must never be on the required path — the same shape as ADR-0005
(DART is never on the required path) and ADR-0011's own rule that bypass
is a normal degraded mode, not an error path.

## Decision

Persistence is best-effort: when `LayerStore::open` throws for a remote
layer, image assembly logs a warning and falls back to the plain
registry source for that layer (remote-only reads, no local caching)
instead of failing `open_image`. A remote layer with an empty
`lower.dir` takes the same remote-only chain. Structural failures —
malformed config, unreachable registry, corrupt layer data — still
throw; only the loss of persistence degrades.

## Consequences

- A device always boots when its blobs are pullable; cache-disk trouble
  surfaces as a warning plus cold reads, never as a failed create.
- The remote-only chain has no user-space cache at all: reads are plain
  registry range reads, so a degraded layer pays full remote latency per
  read (DART, when enabled, still accelerates it).
- `docs/image.md`'s assembly-failure semantics gain this bounded
  exception; the fail-loud rule is unchanged for structural errors.
- The empty-`lower.dir` case is documented honestly as the
  compatibility path: the overlaybd-snapshotter always sets `dir`.

## Alternatives considered

- **Keep fail-loud.** Rejected: it strands operators with a degraded
  cache disk even though every byte remains pullable — availability of a
  cache must not exceed availability of the remote it mirrors.
- **Degrade only on a whitelist of environment errnos** (EACCES, EROFS,
  ENOSPC, ENOTDIR, ENOENT). Rejected: the list can never be complete
  (EDQUOT, ELOOP, EIO on mount failure), and misclassifying a
  persistence problem as fatal reintroduces the boot failure the
  decision exists to prevent; the log line records the precise errno for
  diagnosis either way.
- **Bind the no-dir case to a default cache directory.** Rejected:
  inventing a persistence location the operator never configured hides
  disk usage from the container facility that owns layer-directory
  accounting (ADR-0011 no-eviction rule).
