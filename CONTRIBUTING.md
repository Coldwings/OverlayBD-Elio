# Contributing to overlaybd-elio

Thanks for your interest! This project implements an OverlayBD-compatible
lazy-loading block device on the Elio coroutine runtime, with a ublk backend,
DART P2P acceleration, and per-device process isolation.

## Getting started

- Read [AGENTS.md](./AGENTS.md) — it is the operational law for both humans
  working with AI agents and the agents themselves (workflow, verification,
  documentation policy).
- Read [docs/README.md](./docs/README.md) for the documentation index and the
  mandatory documentation policy.
- Skim [docs/design-assumptions.md](./docs/design-assumptions.md): the
  load-bearing assumptions every module relies on.
- Before proposing a direction change, search [docs/adr/](./docs/adr/README.md)
  — rejected alternatives are recorded there precisely so they are not
  re-proposed.

## Build

```bash
git clone --recursive <repo-url>   # third_party/elio is a submodule
cmake -S . -B build && cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Requirements: GCC 12+ / Clang 15+ (C++20), CMake 3.20+, Linux with io_uring,
OpenSSL dev headers, `liburing-dev`, `zlib1g-dev`, kernel headers >= 6.0 for
`<linux/ublk_cmd.h>`. The first configure fetches nlohmann/json, lz4, and
Catch2 (network required).

ublk end-to-end tests need `ublk_drv` loaded and privileges to create
`/dev/ublk-control` devices; they self-skip when unavailable, so the default
test run works unprivileged.

## Pull requests

Every PR is expected to carry:

- **Tests** — new behavior ships with Catch2 tests; format behavior is
  pinned by round-trip fixtures and golden bytes from the OverlayBD
  specification, not from our own writers alone.
- **Documentation** — every module has a `docs/<module>.md` with a fixed set
  of sections; a change to a module's behavior is expected to update its
  document, and a new module adds a row to the `docs/README.md` index.
  Documentation has a floor, checked by `bash scripts/check-docs.sh`
  (runnable locally, takes ~1s): a hard gate (index sync, documented test
  names, ADR integrity, canonical-copy lockstep) that fails CI, plus advisory
  notes that need a disposition in the PR thread — fixed, or waived with a
  stated reason. Never a silent merge.
- **An ADR when the change is contract-level** — see the trigger list
  (T1–T5) in [docs/adr/README.md](./docs/adr/README.md). The ADR rides in
  the same PR as the change it governs.

Verification before marking a change done:

<!-- CANONICAL-COPY source="AGENTS.md" id="verify-trio" -->
```bash
cmake -S . -B build && cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
bash scripts/check-docs.sh
```
<!-- /CANONICAL-COPY source="AGENTS.md" id="verify-trio" -->

## Code style

- C++20; match the surrounding style. Comments and identifiers in English.
- Async IO goes through Elio `task<T>` coroutines; never block a coroutine
  on a synchronous syscall.
- On-disk/wire format code cites the OverlayBD specification for every
  magic number and field offset.

## Conduct

Be constructive and specific in reviews. Security issues: see
[SECURITY.md](./SECURITY.md).
