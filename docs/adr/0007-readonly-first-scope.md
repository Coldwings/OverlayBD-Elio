# ADR-0007: Read-only first: writable layers and TurboOCI are out of scope

- Status: superseded by ADR-0008
- Date: 2026-09-07
- Supersedes: none
- Binds: src/image/config.cpp, docs/design-assumptions.md

## Context

At project start, the full OverlayBD feature surface was much larger than
the primary target required. Beyond the read-only path — ZFile and LSMT
readers plus multi-layer merge into one read-only block view — upstream
OverlayBD also carries writable upper layers (with read-write index files,
crash recovery, and garbage collection) and TurboOCI image variants. Each
of those multiplies the format surface, the failure modes, and the
validation burden.

The primary target for this project was lazy-loading read-only container
images: a block device that assembles a merged read-only view from remote
blobs. None of the writable machinery is needed for that target, and
building it before the read path was proven would double the format
surface under test at the riskiest moment of the project — while the
sealed-format readers and merge logic were still being validated against
upstream-generated images.

The decision recorded here was to stage the work: harden the read path
first, reject everything else explicitly at the boundary rather than let it
fail ambiguously deeper in.

**Historical framing:** this decision was later reversed for writable upper
layers by ADR-0008, when writable layers became a product requirement; the
writable design adopted there (an in-place-edit LSMT read-write layer)
deliberately deviates from upstream's append-only read-write format. This
record remains on file as an accurate frozen snapshot of the original
decision and its rationale — it is not the current rule.

## Decision

Restrict the initial scope to read-only: reject writable uppers and
TurboOCI variants at configuration parse time and defer them, so that the
read path hardens first.

## Consequences

- Images declaring a non-empty writable upper were rejected at parse time
  with an invalid-argument error — a loud, early failure instead of
  silent misbehavior partway through assembly.
- The sealed-format readers (ZFile, LSMT) and the multi-layer merge logic
  were built, tested, and validated against upstream-generated images
  without carrying any read-write complexity: no writable index files, no
  crash recovery, no garbage collection.
- TurboOCI support was never scheduled under this scope.
- The rejection lived at the configuration boundary, so relaxing the scope
  later (as ADR-0008 eventually did for writable uppers) required widening
  exactly one explicit gate rather than discovering implicit read-only
  assumptions scattered through the stack.

## Alternatives considered

- **Full read-write support from day one.** Rejected at the time: it would
  have doubled the format surface — writable indexes, crash recovery, and
  garbage collection on top of the readers — before the read path itself
  was proven against upstream images. ADR-0008 later revisited this
  trade-off once the read path had hardened and writable layers became a
  firm requirement; that reversal does not invalidate the original
  sequencing rationale recorded here.
