# AGENTS.md

Guidance for AI agents contributing to **overlaybd-elio** — an
OverlayBD-compatible lazy-loading block device implemented on the
[Elio](https://github.com/Coldwings/Elio) C++20 coroutine runtime, exposing
images through **ublk** (not tcmu), accelerated by **DART** as a P2P data
source, with **per-device process isolation**. Read this before making
changes.

## What this project is

A read-first block-device stack for OverlayBD-format container images:

- **format** — readers (and test-fixture writers) for the OverlayBD on-disk
  formats: ZFile (compressed, indexed) and LSMT (log-structured merge tree
  layers), plus multi-layer merge into one read-only block view.
- **source** — pluggable blob sources behind one async interface: local
  files, OCI registry HTTP range reads (with bearer-token auth), a
  sparse-file layer store with background fill (ADR-0011), and a
  **DART proxy source** (prefix passthrough:
  `GET http://<dart>/<prefix>/<full upstream URL>`).
- **image** — assembly: an OverlayBD-compatible `config.json` becomes a
  merged read-only block source.
- **ublk** — the data plane: per-queue io_uring rings speaking
  `UBLK_IO_FETCH_REQ` / `UBLK_IO_COMMIT_AND_FETCH_REQ` uring-cmds, bridged to
  the Elio scheduler that runs the image stack.
- **supervisor** — the process model: one daemon supervises one isolated
  child process per block device; a device crash never takes down its
  siblings.

Binaries: `obd-supervisor` (daemon), `obd-device` (per-device server, spawned
by the supervisor), `obdctl` (control CLI), `obd-mkimage` (test-image
generator), `obd-convert` (deterministic rootfs-tar converter).

## Repository layout

```
src/common/       shared utilities: errors, logging glue, digest, byte ranges
src/format/       OverlayBD on-disk formats: zfile, lsmt, layer merge (+ fixture writers)
src/source/       blob sources: local, registry, layer store with background fill, dart proxy
src/image/        image assembly from overlaybd-compatible config.json
src/ublk/         ublk control + per-queue data plane + Elio bridge
src/supervisor/   daemon, child-process lifecycle, control-socket protocol
src/cmd/          binary entry points (obd-supervisor, obd-device)
tools/            obdctl, obd-mkimage, obd-convert
tests/unit/       Catch2 unit tests
tests/integration/  end-to-end tests (mock registry; ublk tests self-skip without /dev/ublk-control)
docs/             official, git-tracked documentation (English)
docs/adr/         Architecture Decision Records (legislative history)
scripts/          governance and helper scripts (check-docs.sh)
cmake/            project CMake modules
```

Every module above is implemented with tests and a `docs/<module>.md`.

## Documentation

- **`docs/`** is the tracked, **English** documentation for the community.
  Every module must have a `docs/<module>.md`. See `docs/README.md` for the
  mandatory section template and the documentation policy. Documentation is
  part of a change, not optional.
- **Contract-level changes need an ADR.** If your change touches an on-disk
  or wire format (ZFile/LSMT headers, the supervisor control protocol, the
  config schema), a design assumption, any Stability Contract section,
  establishes a cross-module convention, or rejects a significant
  alternative, include an Architecture Decision Record in the same PR. See
  `docs/adr/README.md` for the trigger list (T1–T5), the template, and the
  lifecycle. **Search first:** before changing an existing convention or
  proposing a new direction, look through `docs/adr/` — including rejected
  records, which exist precisely so rejected alternatives are not
  re-proposed (T5).
- When you touch `docs/`, `AGENTS.md`, or `CONTRIBUTING.md`, also run
  `bash scripts/check-docs.sh` — the documentation floor is two-tier: a
  **hard gate** that fails CI (index sync, documented test names, ADR
  integrity, canonical-copy lockstep) and **advisory notes** (required
  sections, ADR back-references) that arrive as a non-blocking PR comment.
  Each advisory note needs a disposition in the PR thread — fixed, or waived
  with a stated reason. Canonical statement: `docs/README.md`, "Enforcement
  tiers".

## Mandatory workflow for any code change

1. **Implement** following the surrounding style (match naming, comment
   density, idioms). Prefer Elio primitives over raw syscalls on coroutine
   paths; avoid new dependencies unless clearly justified.
2. **Test**: add/extend Catch2 tests. Format-level behavior is pinned by
   round-trip tests against `obd-mkimage`-generated fixtures and by golden
   byte values taken from the OverlayBD specification documents — never from
   this project's own writers alone.
3. **Verify** (all must pass before you consider the change done):
<!-- CANONICAL id="verify-trio" -->
   ```bash
   cmake -S . -B build && cmake --build build -j"$(nproc)"
   ctest --test-dir build --output-on-failure
   bash scripts/check-docs.sh
   ```
<!-- /CANONICAL id="verify-trio" -->
4. **Document**: add/update `docs/<module>.md` per the required sections, and
   add a row to the `docs/README.md` index for a new module. Documentation is
   part of the change, not optional.
5. **Commit** locally (see Git rules below).

## Build & test environment

- Compiler: GCC 12+ or Clang 15+ with C++20; CMake >= 3.20; Linux (io_uring
  strongly preferred). OpenSSL dev headers are required (via Elio TLS/HTTP).
  System packages: `liburing-dev`, `zlib1g-dev`; kernel headers >= 6.0 for
  `<linux/ublk_cmd.h>`.
- Dependencies are fetched by CMake FetchContent: Elio (pinned by commit in
  the top-level `CMakeLists.txt`), nlohmann/json, lz4, zstd, Catch2 (tests),
  and, by default, the pinned e2fsprogs/libext2fs converter backend. First
  configure needs network access for FetchContent; the default converter backend
  also needs ordinary C build tools (`make`, a C compiler and binutils).
- ublk end-to-end tests require root (or `CAP_SYS_ADMIN` in the right user
  namespace) and a loaded `ublk_drv`; they **self-skip** otherwise. Never
  make the default test run depend on privileged kernel state.
- If you are working in a sandbox that only permits writes under the
  workspace, keep build trees inside the repo (`build/` is gitignored) and
  point `TMPDIR` there for tests that spawn processes.

## Wire formats and kernel ABIs are load-bearing — do not break them

- The ZFile and LSMT on-disk layouts (magic numbers, header fields, index
  encodings, compression framing) are the **OverlayBD wire format**: images
  built by upstream `overlaybd-*` tools must keep loading byte-identically.
  Any change here is breaking and needs an ADR (T1).
- ublk structures come from `<linux/ublk_cmd.h>` (kernel ABI). We never
  redefine what the header provides; we only add what a given header version
  lacks, guarded by `#ifndef`, in `src/ublk/uapi_compat.hpp`.
- The supervisor↔obdctl control protocol and the supervisor↔obd-device
  lifecycle protocol are wire contracts between binaries that may be upgraded
  independently; changes need an ADR (T1).
- `config.json` compatibility with overlaybd-snapshotter output is an
  operator contract: unknown fields are ignored, known fields keep their
  meaning.

## Coding conventions

- C++20, exceptions for error paths on setup/cold routes, `task<T>` +
  result values on hot IO routes; never block a coroutine on a synchronous
  syscall — use the Elio IO backend.
- The ublk per-queue rings are owned exclusively by their queue thread; the
  Elio scheduler never submits to them (see ADR-0006). Cross-boundary
  handoffs go through the documented bridge interfaces only.
- No hidden global mutable state; singletons are explicit and constructed in
  `main` only.
- Comments and identifiers in code: English.

## Git rules

- Do **not** modify git config. Use whatever identity the working copy
  already has; if you must set one for a single commit, use `-c` flags
  rather than persisting config.
- Commit messages: concise English, imperative subject line, body explaining
  the why. Keep commits focused.
- Never run destructive/irreversible git commands. Never commit build
  directories (they are gitignored; keep it that way). The Elio pin lives in
  the top-level `CMakeLists.txt` (`FetchContent_Declare(elio ... GIT_TAG)`):
  bump it in a dedicated commit with a reason.

## Safety

- Do not run destructive shell commands (`rm -rf` outside `build/`, etc.) or
  anything requiring `sudo`. Use the dedicated file tools for file
  operations.
- Read-only image semantics are a core assumption; do not introduce
  write-back to registries or mutable-image behavior without explicit design
  agreement (an ADR).

## Authority and canonical sources

Normative rules have exactly one canonical home, marked
`<!-- CANONICAL id="..." -->`; marked copies (`CANONICAL-COPY`) exist only
for safety-critical redundancy and must stay identical with their source,
modulo leading indentation (edit both or neither — `scripts/check-docs.sh`
enforces this). If repository documents disagree, precedence is:
`docs/design-assumptions.md` → `docs/README.md` (the documentation policy) →
the module documents (`docs/<module>.md`) → this file → `CONTRIBUTING.md`.
Decisions behind contract-level rules live in `docs/adr/`; the newest
accepted ADR resolves ambiguity.
