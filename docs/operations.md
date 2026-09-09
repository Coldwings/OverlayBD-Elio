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
   (`p2pConfig`), download defaults, bring-up warm-up (`prefetch`:
   `enable` plus the structural `head_kb`/`tail_kb` windows — the
   defaults warm 1 MiB at each end of every layer at scavenger priority;
   shrink them on very small layers or when bring-up time matters more
   than first-read latency), and `logConfig.logLevel`.
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
  reopen by this stack, but do not ship the file elsewhere. On a **graceful
  shutdown** (SIGTERM, e.g. via `destroy` or `commit`) obd-device
  checkpoints the upper's in-memory index into the file; that checkpoint is
  what the offline `commit` seal consumes. A crashed or SIGKILLed device
  leaves no checkpoint and its unsealed upper is unsealable.
- `type: "sparse"` → a sparse file (`<dir>/overlaybd.sparse`); after an
  unclean shutdown the written extents are recovered via fiemap scanning.
  Sparse uppers never seal (ADR-0014 upstream parity).

With an upper present the device is created read-write; without one it is
read-only and write attempts fail with `EROFS`. See ADR-0008 for the
decision and its durability contract.

## Committing a writable device (ADR-0014)

`commit` seals a device's LSMT-RW upper offline into a standard sealed
LSMT layer and reports its content digest:

```bash
obdctl commit myimg --tag "build-2026-09-08"
# → {"ok":true,"id":"myimg","path":".../overlaybd.rw",
#    "sha256":"<hex>","size":<bytes>}
```

Contract and runbook notes:

- **The device is stopped, then sealed.** A live device is SIGTERMed and
  reaped (bounded; SIGKILL on timeout) before any byte is sealed — there
  is no live seal. The device entry remains afterwards (`obdctl status`
  still works, state `exited`); `destroy` removes it as usual.
- **Graceful shutdown is required.** The seal consumes the index
  checkpoint obd-device writes on graceful shutdown. If the device crashed
  or was SIGKILLed, commit fails with "no valid shutdown checkpoint" and the
  unsealed upper's writes are lost (the ADR-0008 durability rule).
- **Deterministic output.** The sealed file is a pure function of the
  upper's content plus the `--tag` string: identical content and tag seal
  to identical bytes, so the reply's `sha256` is suitable for
  content-addressed layer reuse. See docs/format.md for the invariant.
- **Sparse uppers never seal** (upstream parity): commit fails with
  "sparse uppers cannot be sealed". Upper-less (read-only) devices fail
  with "no writable upper".
- A second commit of the same upper fails with "already sealed" — commit
  seals in place (atomic rename over `overlaybd.rw`); copy the file
  beforehand if you need the unsealed form. A commit issued while another
  commit of the same device is still running fails with "commit already
  in progress" — retry after it returns.
- **The upper is the one recorded at create time.** Commit seals the
  upper path/kind the supervisor recorded when the device was created;
  editing the image config afterwards does not redirect it.
- The sealed file is a standard LSMT layer: reference it as a `lowers[]`
  entry (with its sha256 as the digest) in subsequent image configs.
- **Re-baseline (D3).** `obdctl commit myimg --virtual-size <bytes>`
  seals with that value as the layer's declared virtual size, so the
  next `create` from this layer yields the larger device. The override
  is grow-only: it must be a positive multiple of 512 and at least both
  the layer's declared size and its content extent — a smaller value
  fails with a precise error and the upper stays committable at its
  declared size. (Deterministic output caveat: the virtual size feeds
  the content digest, so a re-baselined seal hashes differently from an
  identical-content seal at the old size.)

## Recording a prefetch trace (ADR-0013)

`trace_start` records the layer-blob access pattern of a device's remote
reads into an upstream-compatible prefetch trace blob
(docs/trace-format.md); a replay-capable image build can later warm the
same extents without touching the network:

```bash
obdctl trace_start myimg /var/lib/overlaybd-elio/traces/myimg.trace --duration 300
# → {"ok":true,"id":"myimg","path":"...","duration_sec":300}
obdctl status myimg
# → {"ok":true, ..., "trace":{"state":"recording","path":"...","duration_sec":300}}
obdctl trace_stop myimg
# → {"ok":true,"id":"myimg","path":"...","sha256":"<hex>",
#    "size":<bytes>,"records":<n>,"dropped":<n>}
obdctl status myimg
# → {"ok":true, ..., "trace":{"state":"stopped","reason":"stopped",
#    "sha256":"...", ...}}
```

Runbook notes:

- **Record from a throwaway container.** The recording is only as clean
  as the workload: run the cold-path you want warmed later (app start,
  dependency load) once, then stop. Structural warm-up, trace replay,
  and background-fill traffic are remote reads too and would be recorded
  — for a pristine workload trace, create the recording device with
  `prefetch_enable: false` and `download.enable: false` in the global
  config.
- **The duration bound is enforced device-side** (1..3600 s, default
  300). A crashed or disconnected CLI can never leak a recording: on
  expiry the device finalizes exactly like an explicit stop, and the
  `trace` object in `status`/`list` reports `"state":"stopped",
  "reason":"expired"` with the finalize stats.
- **Stopping is idempotent.** A stop that races (or follows) an expiry
  returns the same `{sha256,size,records,dropped}` — operators can
  always learn the outcome of their recording. A stop with no recording
  in progress fails with "no trace recording in progress".
- **The blob is finalized by the codec's conforming writer** (header
  checksum rewritten on finalize, records pre-split to the 1 MiB count
  cap, zero padding), so it replays against overlaybd and against this
  project's own `prefetch.trace` path. `dropped` counts reads shed
  under extreme pressure (bounded in-memory buffer); the blob remains
  valid and replayable, just with holes in its coverage.
- **Only fully-satisfied REMOTE reads are recorded** (local cache hits
  produce no record); offsets are payload offsets (the tar wrapper is
  translated out) matching what the replay path consumes.
- **Packaging into an image is external** (ADR-0014's tar bundle,
  member name `trace`, plus an `acceleration-layer` config entry); the
  daemon deliberately never rewrites image configs. When deriving a new
  image from one that already embeds a trace, remove the old trace
  member and acceleration-layer entry first — a stale trace references
  layer indices that no longer match.
- `obdctl trace_start <id> <output.trace> [--duration SEC]` and
  `obdctl trace_stop <id>` require supervisor protocol ≥ 3 (`hello`'s
  `features` lists `"trace"`). obdctl performs no handshake gate of its
  own: against an older supervisor the command is sent and the daemon
  answers a clean `unknown cmd 'trace_start'` protocol error.
- **Crash mid-record loses the window.** Queued records live in device
  memory until finalize, so a device crash or SIGKILL mid-recording
  loses them: the supervisor marks the `trace` status `"state":"lost",
  "reason":"device_exit"` (never a stale "recording" — the mark also
  survives a recovery respawn, which records nothing), and the output
  file — created O_TRUNC at start — remains a 0-byte non-blob. Treat a
  lost window as "no trace" and record again.

## Growing a device (D3: dev_size is the quota boundary)

The ADR-0014 resize model treats `dev_size` as the hard quota boundary:
**shrink is unsupported** — a device (and any layer derived from it)
only ever grows. Growth is a three-step chain:

1. **Resize the live device** — runtime headroom for the guest:
   ```bash
   obdctl resize myimg 17179869184        # 16 GiB, bytes
   # → {"ok":true,"id":"myimg","size":17179869184}
   ```
   The device's grow-only executor validates against its current size
   (a request at or below it fails with a "grow-only" error) and issues
   the kernel's `UBLK_U_CMD_UPDATE_SIZE` (docs/ublk.md). The size must
   be a positive multiple of 512.
2. **Grow the filesystem inside the device** (guest side, e.g.
   `resize2fs`/`growpart` + fs-specific grow) to use the new capacity.
3. **Re-baseline with a commit** so the growth is durable:
   ```bash
   obdctl commit myimg --virtual-size 17179869184
   # → {"ok":true,...,"path":".../overlaybd.rw","sha256":"<hex>","size":<bytes>}
   ```
   The sealed layer declares the larger size; a later `create` from
   that layer is created with the larger `dev_size` (skip `--virtual-size`
   at commit and the seal keeps the layer's declared size — for most
   workflows, commit only what the filesystem actually grew into, i.e.
   the same value you resized to).

Contract and runbook notes:

- **Grow-only everywhere.** Resize rejects a request at or below the
  current device size; `create --virtual-size` rejects an override below
  the assembled image size (device-side, where the image size is known);
  `commit --virtual-size` rejects an override below the layer's declared
  size or its content extent. There is no shrink path.
- **Runtime dev-size only.** A resize changes the block device's
  capacity at runtime and touches NO layer metadata: after a device
  crash-recovery respawn (ADR-0010) the device re-attaches at its
  original create-time size, and the block device's data plane only
  covers what the underlying image provides — content writes beyond the
  data plane's extent are out of the resize command's scope. Make
  headroom durable with the three-step chain above; do not rely on a
  resized-but-uncommitted device surviving a crash.
- **Create-time headroom (optional).** `obdctl create myimg config.json
  --virtual-size 17179869184` creates the device directly at the larger
  size (sanctioned headroom) instead of the image's declared size —
  useful when you know the workload will grow the filesystem later.
  Default remains dev_size = image-declared size. The override must be
  a positive multiple of 512.
- `obdctl resize <id> <size-bytes>` and the `--virtual-size` options
  require supervisor protocol ≥ 4 (`hello`'s `features` lists
  `"resize"`); against an older supervisor, `resize` is answered as a
  clean `unknown cmd` protocol error.
- **Requires a recent kernel driver** for the online grow itself:
  `UBLK_U_CMD_UPDATE_SIZE` landed in the 6.16 development cycle; an
  older driver rejects it with `-EINVAL` (the resize reply reports the
  failure — `dev_size` is unchanged). Create-time headroom and commit
  re-baseline need no kernel support.

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
| A device child crashed | Siblings and the supervisor are unaffected (ADR-0004), and the device itself survives: the supervisor respawns the child with `--recover` and the kernel reissues outstanding I/O (ADR-0010). Check `obdctl status <id>` — the `recoveries` counter increments per respawn; after `max_recovery_attempts` (default 3) the device is left down for inspection (`destroy` + `create`). Note the data boundary: an unsealed LSMT-RW upper loses its unsealed writes on recovery (ADR-0008), and commit of such an upper fails with "no valid shutdown checkpoint". |
| `commit` fails with "no valid shutdown checkpoint" | The device crashed or was SIGKILLed instead of shutting down gracefully, so its LSMT-RW index never reached the disk. The unsealed upper is unsealable (ADR-0014); start over from the lowers. |
| `commit` fails with "sparse uppers cannot be sealed" | Sparse uppers never seal (upstream parity, ADR-0014). Use `type: "lsmt"` uppers for content you intend to commit. |
| `discard`/`fstrim` fails with EROFS | The image is read-only (no writable upper configured). Discard is supported only on writable devices (ADR-0009). |
| `resize` fails with a "grow-only" error | The requested size is at or below the device's current capacity; shrink is unsupported (ADR-0014). Grow to a larger size, or create with `--virtual-size` headroom if you need to plan ahead. |
| `resize` fails with an `UPDATE_SIZE`-related error | The kernel driver predates `UBLK_U_CMD_UPDATE_SIZE` (needs the 6.16 cycle); the device capacity is unchanged. `create --virtual-size` and `commit --virtual-size` do not need kernel support. |
| `create --virtual-size` fails with a grow-only error | The override is smaller than the image's declared size (that would shrink the device below its content). Use a larger value or drop the option. |
| `commit --virtual-size` fails with a grow-only error | The override is below the layer's declared size or its content extent; the upper is untouched and still committable at its declared size. |

## Known operational limitations

- **Recovery loses unsealed LSMT-RW writes.** Crash recovery re-opens the
  image from disk; a sparse upper recovers via fiemap, an unsealed LSMT-RW
  upper does not (ADR-0008, ADR-0010). Choose the sparse upper when write
  durability across crashes matters.
- **Discard masks, it does not punch through.** A discarded range reads
  back as zeroes even if lower layers have data there (ADR-0009); this is
  the upstream LSMT trim semantics, intentional.
- **Read-first scope.** The stack serves OverlayBD images; it does not push
  or mutate registry content (ADR-0007). Writable uppers are local-only;
  `commit` (ADR-0014) seals an upper into a local layer file —
  publishing it as an OCI artifact is the external CLI's job.
- **Credentials**: only `credentialConfig` `mode=file` is honored; other
  modes are ignored with a warning (see [config.md](./config.md)).
- **One supervisor per node** is the expected topology; multiple
  supervisors must use distinct `--socket` paths and will compete for ublk
  device ids unless `--dev-id` is managed externally.
