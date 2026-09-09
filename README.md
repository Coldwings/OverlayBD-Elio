# overlaybd-elio

An [OverlayBD](https://github.com/containerd/overlaybd)-compatible
lazy-loading block device for container images, built on the
[Elio](https://github.com/Coldwings/Elio) C++20 coroutine runtime.

Container images start in seconds: the block device serves reads on demand
from a remote OCI registry through OverlayBD's ZFile/LSMT formats, instead
of pulling and unpacking full image layers first.

## Highlights

- **OverlayBD format compatibility** — byte-exact ZFile (compressed,
  indexed) and LSMT (log-structured merge tree) readers; images built by
  upstream `overlaybd-*` tooling load identically.
- **Elio coroutine runtime** — the whole IO path is C++20 coroutines on
  io_uring; no callbacks, no blocking syscalls on request paths.
- **ublk, not tcmu** — the device backend is the kernel ublk interface
  with per-queue io_uring rings (ADR-0003).
- **DART P2P acceleration (optional)** — registry reads can be proxied
  through a [DART](https://github.com/data-accelerator/dart) node via
  prefix passthrough; unreachable or disabled DART falls back to direct
  registry access (ADR-0005).
- **Per-device process isolation** — a supervisor spawns one child process
  per block device; a device crash never takes down its siblings
  (ADR-0004).
- **Writable uppers** — optional copy-on-write upper layers: a sparse
  file, or an in-place-edit LSMT-RW layer that seals into a standard LSMT
  lower (ADR-0008).

## Architecture

```
            obdctl ──UDS(JSON)──▶ obd-supervisor
                                      │ fork+exec per device (fd 3 = status)
                                      ▼
            /dev/ublkbN ◀── ublk rings ── obd-device (one per device)
                                      │
   queue threads ──eventfd──▶ Elio scheduler
                                      │
        MergedWritable (optional upper) / MergedLsmt
        ├─ LsmtLayer ◀─ ZFileSource? ◀─ TarOffsetSource?
        │        ◀─ LayerStore ◀─ RegistrySource
        │                             │ (or DART prefix)
        ▼                             ▼
     local blobs              OCI registry (HTTP range)
```

See [docs/architecture.md](docs/architecture.md) for the full data flow,
process model, and thread model.

## Building

Prerequisites: Linux, GCC 12+ or Clang 15+ (C++20), CMake ≥ 3.20,
`liburing-dev`, `zlib1g-dev`, OpenSSL headers, kernel headers ≥ 6.0
(`<linux/ublk_cmd.h>`). Dependencies (Elio, nlohmann/json, lz4, zstd,
Catch2) are fetched by CMake FetchContent — the first configure needs
network access.

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The ublk end-to-end test self-skips without `/dev/ublk-control`; nothing in
the default test run needs privileges.

## Running

```bash
# 1. start the supervisor (needs privileges for ublk)
./build/src/cmd/obd-supervisor /etc/overlaybd-elio/overlaybd.json

# 2. create a device from an image config
./build/tools/obdctl create /path/to/image/config.json

# 3. use the device
lsblk /dev/ublkb0
```

See [docs/operations.md](docs/operations.md) for deployment, configuration
([docs/config.md](docs/config.md)), writable uppers, and troubleshooting.

## Repository layout

```
src/common/       shared utilities: errors, crc32c, digest, byte ranges
src/format/       ZFile/LSMT readers, layer merge, writable uppers (+ fixture writers)
src/source/       blob sources: local, registry, layer store with background fill, DART proxy
src/image/        image assembly from overlaybd-compatible config.json
src/ublk/         ublk control + per-queue data plane + Elio bridge
src/supervisor/   daemon, child lifecycle, control protocol
src/cmd/          obd-supervisor, obd-device
tools/            obdctl, obd-mkimage
tests/            Catch2 unit + integration tests
docs/             official documentation (see docs/README.md)
docs/adr/         Architecture Decision Records
```

## Documentation and governance

- [docs/README.md](docs/README.md) — documentation index and policy.
- [docs/design-assumptions.md](docs/design-assumptions.md) — the
  cross-module contract every module relies on.
- [docs/adr/](docs/adr/README.md) — the legislative history behind
  contract-level decisions.
- [CONTRIBUTING.md](CONTRIBUTING.md) and [AGENTS.md](AGENTS.md) — the
  contribution workflow (verify-trio, test conventions, ADR triggers).

## Status

Implemented and tested: read path (local + registry + layer store with
background fill + DART), ZFile/LSMT formats, multi-layer merge, writable uppers (sparse and
in-place-edit LSMT-RW with seal), discard/punch-hole with mask-with-zeroes
semantics (ADR-0009), ublk data plane, supervisor process model with ublk
USER_RECOVERY crash recovery (ADR-0010). Deferred by design: TurboOCI,
prefetch.
