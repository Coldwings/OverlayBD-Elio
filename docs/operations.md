# Operations Guide

Deployment and day-2 operations for overlaybd-elio. This is a policy
document: it describes how to build, configure, run and troubleshoot the
system. The normative field reference for both configuration files is
[config.md](./config.md); binary CLIs and exit behavior are specified in
[binaries.md](./binaries.md); the test strategy is in
[testing.md](./testing.md).

## Building

Requirements (from AGENTS.md, "Build & test environment"):

- GCC 12+ or Clang 15+ with C++20; CMake ≥ 3.20; Linux, with io_uring
  **strongly preferred** (Elio's IO backend is built around it; a fallback
  without io_uring is not a supported deployment target).
- System packages: `liburing-dev`, `zlib1g-dev`, OpenSSL dev headers
  (pulled in via Elio TLS/HTTP), and kernel headers ≥ 6.0 providing
  `<linux/ublk_cmd.h>`.
- Network access on first configure: CMake FetchContent downloads Elio
  (pinned by commit in the top-level `CMakeLists.txt`), nlohmann/json, lz4,
  zstd and Catch2.

CMake options (all declared in the top-level `CMakeLists.txt`):

| Option | Default | Effect |
|---|---|---|
| `OBD_BUILD_TESTS` | `ON` | Build the Catch2 unit and integration tests. |
| `OBD_ENABLE_UBLK` | `ON` | Build the ublk backend. Fails at configure time when `<linux/ublk_cmd.h>` is missing; obd-device cannot be built without it. |
| `OBD_ENABLE_ZSTD` | `ON` | Zstd compression support in ZFile (OverlayBD algo 2). |
| `OBD_WARNINGS_AS_ERRORS` | `OFF` | Treat compiler warnings as errors (CI/developer setting). |

Typical build:

```bash
cmake -S . -B build -DOBD_BUILD_TESTS=ON -DOBD_ENABLE_UBLK=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
bash scripts/check-docs.sh
```

The ublk end-to-end tests require root (or `CAP_SYS_ADMIN` in a suitable
user namespace) and a loaded `ublk_drv`; they **self-skip** otherwise. Never
wire the default test run to privileged kernel state, and do not run the
suite under sudo just to force them — a skip is a pass for CI purposes.

## Runtime prerequisites

- **Kernel**: `ublk_drv` loaded (`modprobe ublk_drv`) and `/dev/ublk-control`
  present. Device nodes appear as `/dev/ublkb<N>` once a device is created.
- **Privileges**: the supervisor and its obd-device children need
  `CAP_SYS_ADMIN` (for ublk control) plus read access to the image blobs,
  config files and the control-socket directory. In practice: run
  obd-supervisor as root, or in a user namespace with the capability.
- **Control socket directory**: the default UDS path is
  `/run/overlaybd-elio/supervisor.sock`; create `/run/overlaybd-elio/`
  (tmpfs — recreate on boot) writable by the supervisor.
- **io_uring**: Elio's async IO and the ublk per-queue rings both ride on
  io_uring. Kernels with io_uring uring-cmd support are required for ublk
  operation at all (kernel ≥ 6.0 baseline).

## Configuration

Two files, both overlaybd-compatible (unknown fields ignored, known fields
keep their overlaybd meaning):

1. **Global `overlaybd.json`** (default `/etc/overlaybd-elio/overlaybd.json`,
   overridable with obd-supervisor `--global`): registry credentials
   (`credentialConfig`, `mode=file` only), the DART P2P proxy
   (`p2pConfig`), download defaults, and `logConfig.logLevel`.
2. **Per-image `config.json`** (as written by the overlaybd-snapshotter;
   passed via `obdctl create <id> <config.json>`): `repoBlobUrl`,
   `lowers[]` (digest/size/file), optional per-image `download` overrides,
   and an optional writable `upper` (see below).

Field-by-field reference: [config.md](./config.md).

## Running

Start the daemon in the foreground (under your init system of choice):

```bash
obd-supervisor --socket /run/overlaybd-elio/supervisor.sock \
               --global /etc/overlaybd-elio/overlaybd.json
```

Then drive it with obdctl:

```bash
obdctl create myimg /var/lib/overlaybd-elio/images/myimg/config.json
# → {"ok":true, ... "device":"/dev/ublkb0"} once the child reports ready

obdctl list
obdctl status myimg
obdctl destroy myimg
```

Each `create` spawns one isolated obd-device child process (ADR-0004); the
reply to `create` carries the kernel block device path. The device node is
`/dev/ublkb<N>` — partition scanning, udev rules and mounts operate on that
path as with any other block device. `destroy` stops the child (SIGTERM,
SIGKILL after the stop timeout) and the node disappears.

To produce a local test image without a registry, use obd-mkimage and paste
its stdout snippet into a per-image `config.json`:

```bash
obd-mkimage --input raw.img --out-dir /var/lib/overlaybd-elio/blobs \
            --name base --zfile --zstd
```

## Writable upper layers and sealing (ADR-0008)

A per-image config may add a writable upper:

```json
"upper": { "dir": "/var/lib/overlaybd-elio/upper/myimg", "type": "lsmt" }
```

- `type: "lsmt"` (default) → an in-place-edit LSMT-RW file
  (`<dir>/overlaybd.rw`). **Durability rule: LSMT-RW data is not durable as
  a standard layer until it is sealed.** `seal()` compacts the writable
  layer into a standard sealed LSMT that ordinary OverlayBD tooling can
  consume; before sealing, the file is an intermediate format that only
  this stack reopens. A device destroyed without sealing keeps its data for
  reopen by this stack, but do not ship the file elsewhere.
- `type: "sparse"` → a sparse file (`<dir>/overlaybd.sparse`); after an
  unclean shutdown the written extents are recovered via fiemap scanning.

With an upper present the device is created read-write; without one it is
read-only and write attempts fail with `EROFS`. See ADR-0008 for the
decision and its durability contract.

## Logging

Logging goes to stderr (journald when run under systemd). The level comes
from the global config's `logConfig.logLevel`: `0`=debug, `1`=info
(default), `2`=warn, `3`=error. Each obd-device child logs independently;
correlate by device id and by the supervisor's spawn logs.

## Troubleshooting

| Symptom | Likely cause / action |
|---|---|
| `obdctl: cannot connect to /run/overlaybd-elio/supervisor.sock` | Supervisor not running, or wrong `--socket` on either side. Both default to the same path; check the socket directory exists. |
| `create` returns `{"ok":false,...}` with an assembly error | The child failed to assemble the image (bad `config.json`, unreadable blob, registry auth). The child reports `{"state":"failed","error":...}` and exits 1; the supervisor relays the error string in the reply. Fix the config and retry — the failed child is already gone. |
| `create` fails with "virtual size not sector aligned" | The merged image size is zero or not a multiple of 512 bytes; the image is malformed for block serving. |
| No `/dev/ublkb<N>` after a successful `create` | `ublk_drv` not loaded or missing udev; check `/dev/ublk-control` and `lsmod`. |
| Slow first reads, DART warnings in the log | DART proxy configured but unreachable. This is handled: the source logs a warning and falls back to direct registry reads (guarded by `integration: enabled-but-unreachable DART falls back to direct reads`). Fix the `p2pConfig` address or disable P2P. |
| A device child crashed | Siblings and the supervisor are unaffected (ADR-0004), and the device itself survives: the supervisor respawns the child with `--recover` and the kernel reissues outstanding I/O (ADR-0010). Check `obdctl status <id>` — the `recoveries` counter increments per respawn; after `max_recovery_attempts` (default 3) the device is left down for inspection (`destroy` + `create`). Note the data boundary: an unsealed LSMT-RW upper loses its unsealed writes on recovery (ADR-0008). |
| `discard`/`fstrim` fails with EROFS | The image is read-only (no writable upper configured). Discard is supported only on writable devices (ADR-0009). |

## Known operational limitations

- **Recovery loses unsealed LSMT-RW writes.** Crash recovery re-opens the
  image from disk; a sparse upper recovers via fiemap, an unsealed LSMT-RW
  upper does not (ADR-0008, ADR-0010). Choose the sparse upper when write
  durability across crashes matters.
- **Discard masks, it does not punch through.** A discarded range reads
  back as zeroes even if lower layers have data there (ADR-0009); this is
  the upstream LSMT trim semantics, intentional.
- **Read-first scope.** The stack serves OverlayBD images; it does not push
  or mutate registry content (ADR-0007). Writable uppers are local-only.
- **Credentials**: only `credentialConfig` `mode=file` is honored; other
  modes are ignored with a warning (see [config.md](./config.md)).
- **One supervisor per node** is the expected topology; multiple
  supervisors must use distinct `--socket` paths and will compete for ublk
  device ids unless `--dev-id` is managed externally.
