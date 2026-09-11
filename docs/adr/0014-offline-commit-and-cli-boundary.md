# ADR-0014: Commit offline with deterministic seal, grow-only resize, and an external CLI boundary

- Status: accepted
- Date: 2026-09-08
- Supersedes: none
- Updated by: ADR-0019 (converter backend scope)
- Binds: docs/supervisor.md, docs/operations.md, docs/binaries.md,
  docs/format.md, docs/design-assumptions.md, src/supervisor/protocol.hpp

## Context

The project should run standalone, not only under containerd: a CLI
(possibly in Go, in a separate repository) should be able to bring up
devices (read-only or writable), commit them, and push the result as OCI
artifacts, with trace recording (ADR-0013) driven the same way. Three
upstream findings bound the design:

1. **Upstream commit is offline.** Their commit tool is a file-to-file
   userspace operation over the writable layer's data and index files;
   it never talks to a live device. In the containerd flow it runs after
   the device is gone. Upstream also does not support sealing sparse
   writable layers — only the non-sparse LSMT-RW form seals.
2. **Image building must be deterministic.** Converting the same tar
   layer twice must yield the same output layer, or content-addressed
   layer reuse collapses. Calling host mkfs tooling cannot provide this:
   each run randomizes UUIDs, hash seeds, timestamps, and layout
   heuristics. Upstream's answer is a pinned fork of e2fsprogs used as a
   pure library, which also enables their streaming converter (tar stream
   in, layer out, no device, no mount).
3. **Device size versus filesystem size is a productization hazard in
   both directions:** content that outgrows the filesystem layout cannot
   be stored, while a filesystem much smaller than its block device lets
   any runtime user grow into unaccounted space.

## Decision

- **Commit is offline.** The supervisor's commit command requires the
  device stopped (or stops it), then seals the writable upper's files and
  replies with the sealed layer's path, sha256, and size. Live online
  seal is not offered; the kernel's quiesce control command remains
  available as a future primitive if a live-commit design is ever
  accepted. Sparse writable uppers reject commit, matching upstream.
- **Seal output is a pure function of the upper's content.** Sealed
  layers contain no wall-clock timestamps, random identifiers, or
  process-derived fields; identical upper content seals to identical
  bytes. A regression test seals the same content twice and requires
  equal digests. This invariant is what makes build-side determinism
  achievable end to end.
- **Resize is grow-only, in three explicit steps.** The device size is
  the hard quota boundary: filesystems live inside it and may grow only
  within it; shrinking is unsupported. The sanctioned growth chain is:
  control-plane size update (live) → filesystem grow (live) → commit with
  an explicit virtual-size parameter to re-baseline the sealed layer.
  Image-created devices default to the image-declared size (no implicit
  headroom); headroom exists only when a create-time virtual-size
  override declares it, making runtime growth a sanctioned consumption of
  declared quota rather than a loophole. Build-side, the converter grows
  the filesystem image as content streams in, so "content does not fit"
  is a build-time auto-grow, not a failure.
- **Three device-creation modes.** From an image (filesystem already in
  the layers); blank raw disk with a mandatory size (the caller formats
  it — covering non-ext4 filesystems and custom layouts); blank disk with
  a runtime-only mkfs convenience (host tooling, explicitly excluded from
  image building because it is non-deterministic). The zero base beneath
  a blank disk is an empty LSMT layer.
- **The streaming converter lives in this repository**, beside the
  format writers it must share. ADR-0019 narrows the first accepted
  implementation, and the corresponding issue #41 optional-backend acceptance
  criterion for this PR, to a dependency-free built-in ext2 backend for default
  builds and CI; a pinned e2fsprogs/libe2fs backend may still be added later as
  a converter-local extension with its own build flag and CI matrix. The
  external CLI repository owns everything that merely talks to a registry:
  reference resolution UX,
  digest and manifest construction, blob upload, artifact push, and
  trace-layer packaging. The C++ side's only obligation is that the
  supervisor protocol can express every capability the CLI needs.
- **The supervisor protocol evolves additively, with a version
  handshake.** New commands and fields may be added; existing field
  meanings never change; a hello exchange lets non-C++ clients detect
  capability instead of discovering breakage at runtime.

## Consequences

- A Go (or any language) CLI can implement the full standalone lifecycle
  against the JSON-lines protocol without C++-side changes beyond the
  commands listed here; the additive-only rule plus version handshake is
  the standing guarantee that keeps it unbroken (trigger T1 for each new
  command).
- Deterministic seal plus a pinned-library converter makes converted
  layers bit-reproducible per converter version; a converted-from
  annotation (mapping source tar-layer digest to converted layer, with
  converter version) extends reuse across versions and hosts.
- `docs/operations.md` gains the commit/resize runbooks; the config and
  protocol schemas grow additively; `docs/format.md` records the seal
  determinism invariant.
- The writable-upper documentation (ADR-0008) gains the parity note:
  sparse uppers never seal, by design.

## Alternatives considered

- **Live online seal of a running device.** Deferred, not rejected: it
  needs freeze semantics for in-flight I/O that the offline model simply
  does not have; upstream's offline parity covers the known use cases,
  and the quiesce primitive keeps the door open.
- **Host mkfs tooling for image building.** Rejected: per-run randomness
  (UUIDs, hash seeds, timestamps, layout heuristics) destroys layer
  reuse; the pinned-library converter is the upstream-proven
  deterministic path. Host tooling survives only as a runtime convenience
  whose output is never published as an image layer.
- **Placing the converter in the external CLI repository.** Rejected:
  it would force a second implementation of the LSMT/ZFile writers in
  another language, with two wire-format implementations to keep
  byte-exact. The converter belongs beside the writers it uses; the CLI
  orchestrates it.
- **Supporting device shrink.** Rejected: filesystem shrink relocates
  data and fails destructively; the cost/benefit does not close.
- **A synthetic zero BlobSource as the blank-disk base.** Rejected in
  favor of an empty LSMT layer, which reuses an existing, tested format
  path and adds no new source type.
