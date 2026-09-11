# ADR-0018: Treat invalid download tryCnt as a structural configuration error

- Status: proposed
- Date: 2026-09-11
- Supersedes: none
- Binds: docs/config.md, docs/image.md, src/image/config.cpp,
  src/image/image_file.cpp

## Context

ADR-0016 makes persistence best-effort when a layer directory is unusable:
environmental failures in `LayerStore::open` degrade to remote-only serving.
That boundary does not cover malformed operator input. `download.tryCnt`
sets the completion-verification retry bound for the background download
contract; a value of zero gives the `LayerStore` no valid retry budget, and
negative or out-of-range JSON values can silently become huge unsigned counts
if they are converted before validation.

## Decision

The effective `download.tryCnt` value must be an integer in
`1..4294967295`; zero, negative, out-of-range, and programmatic zero values
are structural configuration errors that fail before persistence degradation.

## Consequences

- Global and per-image JSON parsing rejects malformed `tryCnt` before
  converting it to `uint32_t`.
- Programmatically constructed `ImageConfig` values are rechecked at
  `open_image()` entry so no local-file, no-dir, or remote-only assembly path
  can bypass the structural boundary.
- ADR-0016 remains unchanged: actual persistence-environment failures still
  degrade to remote-only serving after structurally valid config has been
  accepted.

## Alternatives considered

- **Treat `tryCnt=0` as "disable retry".** Rejected: `download.enable` already
  controls proactive fill, and a zero verification budget makes completion
  semantics ambiguous.
- **Let `LayerStore::open` reject zero and let ADR-0016 degrade.** Rejected:
  that hides malformed configuration as a cache-environment problem and lets
  no-dir or local-only paths evade validation.
- **Clamp invalid values to one retry.** Rejected: silently rewriting operator
  input masks configuration mistakes and differs from the fail-loud treatment
  of other structural numeric bounds.
