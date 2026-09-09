# overlaybd-elio Documentation

This directory holds overlaybd-elio's **official documentation** (tracked in
git). It targets both implementers and users, and covers, for every module:
the public API, semantics, conventions, call permissions (concurrency
contract), testing status, and stability guarantees.

> Scope note: this `docs/` tree is the **living, tracked documentation** and
> must stay in sync with the code. All content here is written in **English**
> for community use.

## Index

| Document | Scope | Status |
|---|---|---|
| [architecture.md](./architecture.md) | system-wide — data flow, process model, thread model, module map | Living overview |
| [common.md](./common.md) | `src/common` — errors, logging glue, digest, byte ranges | Implemented, tests passing |
| [format.md](./format.md) | `src/format` — ZFile + LSMT readers, layer merge, fixture writers | Implemented, tests passing |
| [trace-format.md](./trace-format.md) | upstream OverlayBD prefetch trace blob — byte-exact wire specification (ADR-0013 step 0) | Specification of upstream format; codec implemented (`src/format/trace.hpp`), record/replay not wired |
| [source.md](./source.md) | `src/source` — blob sources: local, registry, layer store with background fill, DART proxy | Implemented, tests passing |
| [image.md](./image.md) | `src/image` — config.json parsing and image assembly (read-only or with a writable upper) | Implemented, tests passing |
| [ublk.md](./ublk.md) | `src/ublk` — ublk control plane, per-queue data plane, Elio bridge | Implemented, tests passing |
| [supervisor.md](./supervisor.md) | `src/supervisor` — daemon, per-device child lifecycle, control protocol | Implemented, tests passing |
| [binaries.md](./binaries.md) | `obd-supervisor`, `obd-device`, `obdctl`, `obd-mkimage` — usage and behavior | Implemented, tests passing |
| [config.md](./config.md) | configuration reference — global config, per-image config.json, compatibility notes | Living contract |
| [testing.md](./testing.md) | test strategy — unit, integration, privileged ublk E2E, fixtures | Living policy |
| [operations.md](./operations.md) | deployment and operations guide | Living policy |
| [design-assumptions.md](./design-assumptions.md) | cross-cutting — the minimalist assumptions every module relies on | Living contract |
| [adr/](./adr/README.md) | Architecture Decision Records — the legislative history behind contract-level rules | Living record |

(Register one row here for every new module.)

## Documentation Policy (mandatory)

**Any new or changed implementation code MUST come with an updated module
document.** This is a hard rule; documentation is treated as part of the code.

### Required sections for every module document

1. **Overview** — what problem the module solves and where it sits in the
   system.
2. **Concepts** — core terms and models used by the module (e.g. ZFile jump
   table, LSMT segment index, source stack layering).
3. **Public API** — for **every exported header-level type, function, and
   method**:
   - exact signature;
   - parameter and return-value semantics;
   - time/space complexity where non-trivial;
   - boundary and error behavior (empty input, out-of-range, zero values,
     short reads, etc.);
   - a minimal runnable example where useful.
4. **Invariants & Guarantees** — properties callers may rely on
   (determinism, byte-exactness of format parsing, ordering, idempotence…).
5. **Concurrency & Call Permissions**:
   - thread/coroutine safety, and which scheduler context a function must run
     in (Elio coroutine vs. plain thread vs. ublk queue thread);
   - presence of any instance-level mutable state;
   - whether inputs are mutated, and ownership of buffers and returned
     values;
   - preconditions callers must satisfy (who may call, in what order).
6. **Stability Contract** — which changes are breaking (e.g. any change to
   ZFile/LSMT parsing semantics is breaking for image compatibility);
   compatibility policy.
7. **Testing** — the test list with each test's intent; how to run; sources
   of any golden / cross-validated values.
8. **Limitations & TODO**.

### Sanctioned document shapes

The 8-section template above is the default **module** shape. Four shapes are
sanctioned; every document's shape is assigned here (unlisted documents
default to `module`). The machine-readable block is the enforcement source
for `scripts/check-docs.sh` — keep it in sync with the prose.

<!-- DOC-SHAPES
module: Overview; Concepts; Public API; Invariants & Guarantees; Concurrency & Call Permissions; Stability Contract; Testing; Limitations & TODO
cmd: Overview; Usage; Behavior & guarantees; Testing; Limitations & TODO
multi: Overview; per-module sections; Concurrency & Call Permissions; Stability Contract; Testing; Limitations & TODO
policy: free-form policy document (no required sections)
assignments: docs/architecture.md=multi; docs/binaries.md=cmd; docs/config.md=policy; docs/testing.md=policy; docs/operations.md=policy; docs/design-assumptions.md=policy; docs/trace-format.md=policy
heading-aliases: Concepts <=> Wire form; Concurrency & Call Permissions <=> Threading & Call Permissions; Stability Contract <=> Compatibility / Stability Contract
-->

- **module** (default): the 8 required sections in §"Required sections".
- **cmd** (assigned: `docs/binaries.md`): binary documents — `Usage` replaces
  the API section; Concurrency and Stability may live as subsections of
  "Behavior & guarantees".
- **multi** (assigned: `docs/architecture.md`): one document covering several
  modules uses per-module sections instead of unified
  Concepts/Public-API/Invariants sections.
- **policy** (assigned: `docs/config.md`, `docs/testing.md`,
  `docs/operations.md`, `docs/design-assumptions.md`): free-form policy
  documents; the section template does not apply.
- **Heading aliases**: the Concepts section may be titled by its domain
  content; the Concurrency section may be titled "Threading & Call
  Permissions".

## Enforcement tiers

The documentation floor is two-tier, checked by `scripts/check-docs.sh`
(local, ~1s) and mirrored by the CI `docs governance` job. The principle:
**a claim that is false or broken blocks; a demand for more documentation
advises.**

- **Hard gate (fails CI)**:
  1. this index and `docs/*.md` files are in sync (every file has a row,
     every row has a file);
  2. test names cited in backticks in `docs/` actually exist in `tests/`;
  5. ADR integrity: unique sequential numbering, complete metadata
     (Status/Date/Supersedes/Binds), symmetric supersession, index coverage,
     existing `Binds` paths;
  6. `CANONICAL-COPY` blocks are byte-identical to their `CANONICAL` source
     (modulo leading indentation).
- **Advisory (never fails CI; arrives as one sticky PR comment)**:
  4. every `docs/*.md` carries the required sections of its assigned shape;
  7. every non-superseded ADR is cited by number from at least one
     current-law document.

Advisory notes do not block merge, but each needs a **disposition** in the PR
thread — fixed, or waived with a stated reason. Waivers are explicit,
auditable decisions — never silent merges. Maintainers and agents working
inside the repository are held to the same standard.

The `Testing` sections must be credible: not "tests exist", but the list of
tests with the property each guards, how to run them, and where golden values
come from. Known limitation versus the DART original: the exported-symbol
coverage check (their check 3) has no cheap C++ analogue and is deliberately
not implemented; section completeness (check 4) is the floor instead.

## Evidence anchors

Contract-level entries may carry a backticked `"path::symbol"` anchor
(**never line numbers** — they drift on every edit; symbol names are stable
and grep-verifiable). Anchors are opt-in; `check-docs.sh` verifies only
anchors that are written (file exists and declares the symbol), never demands
them. Historical claims anchor a commit SHA. ADR bodies never carry live
anchors (see `docs/adr/README.md`).
