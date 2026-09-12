# Binaries: obd-supervisor, obd-device, obdctl, obd-mkimage, obd-convert

## Overview

The repository builds five executables:

- **obd-supervisor** — the per-node daemon. It owns the control plane: a
  Unix domain socket accepting JSON-lines commands, and one isolated child
  process per block device (ADR-0004). A device crash never takes down its
  siblings or the daemon.
- **obd-device** — the per-device server. Spawned by obd-supervisor (once per
  `create` request), it assembles one OverlayBD image from a per-image
  `config.json` and serves it as one ublk block device (`/dev/ublkb<N>`).
  It is not meant to be run by hand, but it can be, for debugging.
- **obdctl** — the control CLI. One invocation sends one JSON-lines command
  to the supervisor over its UDS and pretty-prints the reply. It uses plain
  blocking IO; the Elio runtime is not involved.
- **obd-mkimage** — a test-image generator. It converts a raw disk image
  into a sealed single-layer LSMT file, optionally ZFile-compressed, and
  prints a ready-to-use `lowers[]` config snippet on stdout. It exists to
  produce fixtures for tests and local bring-up, not for production image
  builds.
- **obd-convert** — the deterministic rootfs-tar converter (ADR-0019). It
  reads a ustar archive from a file or stdin, builds a bounded ext2-compatible
  filesystem image without a device, mount, or host mkfs subprocess, seals it
  as an LSMT-RO layer, and prints manifest metadata.

Interactions: `obdctl → obd-supervisor` over the supervisor UDS (default
`/run/overlaybd-elio/supervisor.sock`); `obd-supervisor → obd-device` by
fork/exec per `create` request, with a socketpair as the lifecycle status
channel; `obd-device → kernel` over ublk. The wire protocols are specified
in `src/supervisor/protocol.hpp` and documented in
[supervisor.md](./supervisor.md); configuration files are documented in
[config.md](./config.md). Operational procedures live in
[operations.md](./operations.md).

## Usage

### obd-supervisor

```
obd-supervisor [--socket PATH] [--global PATH] [--device-bin PATH]
               [--ready-timeout SEC] [--stop-timeout SEC]
               [--blank-dir PATH] [--mkfs-timeout SEC]
```

Runs in the foreground until SIGTERM/SIGINT. Options (defaults from
`src/supervisor/daemon.hpp::DaemonConfig`):

| Option | Default | Meaning |
|---|---|---|
| `--socket PATH` | `/run/overlaybd-elio/supervisor.sock` | Control UDS path listened on for obdctl commands. |
| `--global PATH` | `/etc/overlaybd-elio/overlaybd.json` | Global config handed to children when a `create` command does not carry its own `--global`. |
| `--device-bin PATH` | empty = sibling of the supervisor executable | obd-device binary to exec for each device. |
| `--ready-timeout SEC` | `60` | How long `create` waits for the child's `ready` status before failing the request. |
| `--stop-timeout SEC` | `10` | `destroy` grace period: SIGTERM first, SIGKILL after this many seconds. |
| `--blank-dir PATH` | `/var/lib/overlaybd-elio/devices` | ADR-0014: root of the per-device workspaces for blank (raw) devices — each owns `<blank-dir>/<id>/` with `overlaybd.zero` (sealed empty LSMT zero base) and `overlaybd.rw` (writable upper). |
| `--mkfs-timeout SEC` | `300` | ADR-0014 mode 3: bound for the host `mkfs.<type>` run on a new blank device. |
| `--help`, `-h` | — | Print usage and exit 0. |

### obd-device

```
obd-device (--config PATH | --blank-size BYTES --blank-dir PATH)
          [--global PATH] [--control-fd N] [--dev-id N] [--recover]
```

| Option | Default | Meaning |
|---|---|---|
| `--config PATH` | — | Per-image `config.json` (overlaybd-snapshotter format). Mutually exclusive with the blank form. |
| `--blank-size BYTES` | — | ADR-0014 modes 2/3: create a blank (raw) device of this many bytes (no config) — the empty LSMT zero base + a writable LSMT-RW upper are assembled in `--blank-dir`. Strictly validated here too (positive, 512-aligned, ≤ 16 TiB; a negative or overflowing value is a usage error, exit 2), not only by the supervisor's `parse_blank_spec`. |
| `--blank-dir PATH` | — | ADR-0014: per-device workspace for a blank device (files `overlaybd.zero` / `overlaybd.rw`). |
| `--global PATH` | empty = built-in defaults | Global `overlaybd.json`; when omitted, a default-constructed `GlobalConfig` is used. |
| `--control-fd N` | `-1` (no reporting) | Inherited fd for JSON-lines lifecycle status reports (the socketpair end installed by the supervisor). |
| `--dev-id N` | `-1` (auto-assign) | Requested ublk device id; the kernel picks a free id when negative. |
| `--recover` | off | ADR-0010: attach to the existing `--dev-id` device via `START_USER_RECOVERY` instead of creating a new one (requires `--dev-id`). Used by the supervisor when respawning a crashed device. |
| `--help`, `-h` | — | Print usage and exit 0. |

Requires a build with `OBD_ENABLE_UBLK=ON` (the only supported block-device
backend; the source does not compile otherwise).

### obdctl

```
obdctl [--socket PATH] hello
obdctl [--socket PATH] create <id> <config.json> [--global PATH] [--dev-id N]
obdctl [--socket PATH] create-blank <id> --size BYTES [--mkfs TYPE]
                                      [--global PATH] [--dev-id N]
obdctl [--socket PATH] destroy <id>
obdctl [--socket PATH] list
obdctl [--socket PATH] status <id>
obdctl [--socket PATH] commit <id> [--tag TAG]
```

`--socket` defaults to `/run/overlaybd-elio/supervisor.sock` and, when
present, must precede the command word. Commands:

- `hello` — the protocol handshake: the reply carries the control-protocol
  revision (`protocol`), the supervisor's version string (`version`), and
  the capability list (`features`); see [supervisor.md](./supervisor.md)
  for the additive-only evolution rule.
- `create <id> <config.json>` — ask the supervisor to spawn an obd-device
  child serving that image config. Optional `--global PATH` overrides the
  supervisor's default global config for this device; optional `--dev-id N`
  requests a specific ublk id. The reply blocks until the child reports
  `ready` (carrying the `/dev/ublkb<N>` path) or fails/times out.
  `--dev-id -1` (or omitting the flag) means auto-assign, exactly as the
  protocol defines it — the CLI forwards the range the daemon accepts,
  `[-1, INT32_MAX]`.
- `create-blank <id> --size BYTES [--mkfs TYPE]` — ADR-0014 modes 2/3:
  create a blank raw device of `BYTES` (positive, multiple of 512, at
  most `kMaxBlankSizeBytes` = 16 TiB — the same bound the supervisor and
  obd-device enforce, so an absurd size is a local usage error rather
  than a server round trip) with no image config — the wire form is `create` with the additive `blank`
  object (`{"size":...}` plus optional `"mkfs"`). `--mkfs ext4` (mode 3)
  asks the supervisor to run host `mkfs.<type>` on the new block device
  before replying — a runtime-only convenience whose output is never an
  image-build input (see [operations.md](./operations.md)); mode-2
  devices (no `--mkfs`) are formatted by the caller. The reply carries
  `mode`, `size`, and `mkfs` when requested.
- `destroy <id>` — stop the child (SIGTERM, then SIGKILL after the
  supervisor's stop timeout) and remove the device.
- `list` — list known devices and their states.
- `status <id>` — query one device.
- `commit <id> [--tag TAG]` — offline commit (ADR-0014): stop
  the device when live, then seal its LSMT-RW upper in place and reply
  with the sealed file's `path`, hex `sha256`, and byte `size`. `--tag`
  is recorded as the sealed layer's `user_tag`. Sparse and upper-less
  devices are errors; the full contract is in
  [supervisor.md](./supervisor.md) and the runbook in
  [operations.md](./operations.md).

The supervisor's JSON reply is pretty-printed to stdout.

### obd-mkimage

```
obd-mkimage --input <raw.img> --out-dir <dir> [--name base]
            [--zfile] [--bs N] [--zstd [level]] [--no-verify]
```

| Option | Default | Meaning |
|---|---|---|
| `--input PATH` | required | Raw disk image to convert. |
| `--out-dir DIR` | required | Output directory (must exist). |
| `--name STR` | `layer` | Basename of the produced files. |
| `--zfile` | off | Also ZFile-compress the LSMT file into `<name>.zfile`; the snippet then points at the compressed blob. |
| `--bs N` | format default | ZFile block size (only meaningful with `--zfile`). |
| `--zstd [level]` | off | Use Zstd (OverlayBD algo 2) instead of the default compression; an optional level may follow. |
| `--no-verify` | verify on | Skip the read-back verification pass after writing. |

Produces `<out-dir>/<name>.lsmt` (a sealed single-layer LSMT) and, with
`--zfile`, `<out-dir>/<name>.zfile`. On success it prints a JSON snippet on
stdout:

```json
{
  "lowers": [
    { "digest": "sha256:<hex>", "size": <bytes>, "file": "<blob path>" }
  ],
  "repoBlobUrl": ""
}
```

which can be dropped into a per-image `config.json` for tests (see
[config.md](./config.md)).

### obd-convert

```
obd-convert --input <rootfs.tar|-> --out-dir <dir> [--name base]
            [--backend builtin-ext2|libe2fs] [--size bytes] [--keep-raw]
```

| Option | Default | Meaning |
|---|---|---|
| `--input PATH|-` | required | Rootfs ustar archive with the standard two-zero-block end marker. `-` reads stdin. |
| `--out-dir DIR` | required | Output directory; it is created if missing. |
| `--name STR` | `layer` | Basename of the produced layer. The value must be a plain file stem using letters, digits, `.`, `_`, or `-`. |
| `--backend builtin-ext2\|libe2fs` | `libe2fs` when built with `OBD_ENABLE_LIBE2FS_BACKEND=ON`; otherwise `builtin-ext2` | Select the filesystem-image writer. The `libe2fs` backend uses the pinned e2fsprogs/libext2fs build and is the default in normal builds. Dependency-free builds keep the built-in backend available, and `--backend builtin-ext2` forces that path explicitly. |
| `--size BYTES` | auto | Raw filesystem size. When omitted, `libe2fs` estimates a 4 KiB-aligned size from the tar contents and metadata; the built-in backend picks a bounded 4 KiB-aligned size with room for the archive, applies a 4 MiB minimum image size, and rounds with slack. When provided, it must be a 4 KiB multiple and large enough for the contents. |
| `--keep-raw` | off | Keep the intermediate `<out-dir>/.<name>.ext2.tmp` filesystem image for inspection. It is first written in the private staging directory and then atomically renamed to this path. By default it is removed after the LSMT layer is written. |

On success, `obd-convert` writes `<out-dir>/<name>.lsmt` by atomically
renaming a completed temporary file into place, then prints a JSON snippet
compatible with a `lowers[]` entry plus converter metadata. In a libe2fs-enabled
build the default backend output looks like:

```json
{
  "lowers": [
    { "digest": "sha256:<hex>", "size": <bytes>, "file": "<blob path>" }
  ],
  "repoBlobUrl": "",
  "converter": {
    "backend": "libe2fs",
    "filesystem": "ext2",
    "raw_digest": "sha256:<hex>",
    "virtual_size": <bytes>
  }
}
```

The `libe2fs` backend supports the same regular-file, directory and symlink tar
entry classes as the built-in backend, but delegates filesystem construction to
the pinned e2fsprogs/libext2fs implementation. It raises the built-in backend's
small image, single-indirect-file, direct-directory-block and 16-bit uid/gid
limits, while preserving deterministic timestamps, stable ordering and the
no-device/no-mount/no-host-`mkfs` boundary. The current libe2fs path still keeps
converter-local guardrails: at most 65536 in-memory tar nodes and at most 1024
data blocks in any one directory, because it does not create htree-indexed ext2
directories yet. It still rejects malformed archives and tar features outside
the current converter contract, including PAX/GNU long names, hardlinks, device
nodes, FIFOs, sparse tar entries and xattrs.

The built-in backend remains available for dependency-free builds and explicit
`--backend builtin-ext2` runs. It supports regular files up to 4,243,456 bytes
(12 direct data blocks plus one single-indirect block), directories up to 12
data blocks, and short inline symlinks. It uses 4 KiB ext2 blocks, uid/gid
values up to 65535, at most 32768 inodes, and images up to 128 MiB, or the
explicit aligned `--size` budget when smaller. It rejects malformed archives and
unsupported tar entries before publishing an LSMT layer.

When `OBD_ENABLE_LIBE2FS_BACKEND=ON`, the install tree places the pinned
`libext2fs.so*` files under `lib/overlaybd-elio` and gives `obd-convert` an
`$ORIGIN/../lib/overlaybd-elio` runtime search path so the bundled libext2fs is
preferred relative to the binary. `libcom_err.so.2` is intentionally not bundled;
it is resolved as a normal system runtime library. Operators may intentionally
omit the bundled libext2fs and rely on a system libext2fs-compatible library;
that is a performance and deployment-control tradeoff, especially for images
with many files, rather than a known output-correctness incompatibility.

## Behavior & guarantees

### Exit behavior

- All five binaries: exit `0` on success, `1` on runtime failure (message on
  stderr, and for obd-device additionally a `failed` status report), `2` on
  usage errors (unknown argument, missing option value, missing required
  option). obdctl exits `0` exactly when the supervisor's reply carries
  `"ok": true`, `1` otherwise (including connection failures and malformed
  replies).
- obd-device exit `0` means a *clean stop*: the device served, received
  SIGTERM/SIGINT, tore the ublk device down, and reported `stopped`. Exit
  `1` covers every setup or runtime failure, including an image whose
  virtual size is zero or not a multiple of 512 bytes (rejected up front as
  "virtual size not sector aligned").

### Signal handling

Both daemons use the signalfd model. Before the Elio scheduler starts,
obd-supervisor blocks SIGTERM, SIGINT and SIGCHLD, and obd-device blocks
SIGTERM and SIGINT, **on every thread** (failure to block → exit `1`
before any work begins). Signals are then consumed via
`elio::signal::signal_fd` inside coroutines — there are no signal handlers
running on arbitrary threads.

- obd-device: after reporting `ready`, it waits on the signal fd; the first
  SIGTERM or SIGINT breaks the loop, stops and destroys the ublk device,
  reports `stopped`, and returns exit code 0. When the image has a writable
  upper, the shutdown **checkpoints** the upper's in-memory segment index
  into its file after the queues drain (ADR-0014) — this is what the
  supervisor's offline `commit` seal consumes; a checkpoint failure is
  logged and shutdown continues (commit will then report the missing
  checkpoint).
- obd-supervisor: SIGTERM/SIGINT triggers a graceful shutdown — children
  are terminated first (SIGTERM with the stop-timeout grace, then SIGKILL)
  before the daemon exits.

### Lifecycle status reporting

obd-device reports `starting` → `ready` (with the `/dev/ublkb<N>` path) →
`stopped`, or `failed` with an error string, as JSON-lines on the inherited
`--control-fd` (`src/supervisor/protocol.hpp::DeviceStatus`, built via
`src/supervisor/protocol.hpp::make_device_status`). Reports are best-effort
single short writes; the supervisor tolerates a lost report as EOF on the
channel and falls back to waitpid-derived state. Without `--control-fd`
(standalone runs) reporting is silently disabled.

### Supervisor control socket

The supervisor listens on a stream UDS (default
`/run/overlaybd-elio/supervisor.sock`), one JSON-lines command per
connection, one JSON-lines reply, max 64 KiB per message
(`src/supervisor/protocol.hpp::kMaxMessageBytes`). `create` spawns one
obd-device child per request — process isolation per device is the ADR-0004
guarantee. `destroy` sends SIGTERM and escalates to SIGKILL after
`--stop-timeout` seconds. A blank (`create-blank`) request spawns the same
kind of child with `--blank-size`/`--blank-dir` (ADR-0014); when the blank
carries an `mkfs` type, the supervisor additionally runs host
`mkfs.<type>` against the reported `/dev/ublkb<N>` before answering — and
refuses `commit` for such a device afterwards (never seals host-mkfs
output).

### Concurrency and stability notes

- obdctl, obd-mkimage and obd-convert are single-threaded, synchronous
  tools; they hold no state between invocations.
- The supervisor↔obdctl command protocol and the supervisor↔obd-device
  status protocol are wire contracts between binaries that may be upgraded
  independently; changes require an ADR (trigger T1, see
  [adr/README.md](./adr/README.md)).
- Command-line parsing is exact-token matching: `--socket=PATH` style
  combined forms are *not* accepted by any of the five binaries.

## Testing

- `supervisor: protocol commands parse and reject garbage` — pins the
  obdctl↔supervisor command grammar: valid `create`/`destroy`/`list`/
  `status` lines parse; malformed JSON, unknown commands and bad fields are
  rejected with an error.
- `supervisor: daemon answers hello and never drops bad input` — the
  `obdctl hello` path end to end: a real daemon answers the handshake with
  `protocol`/`version`/`features` and never drops malformed input.
- `supervisor: commit stops the device and seals its upper offline` — the
  `obdctl commit` path end to end against a real daemon (ADR-0014).
- `cli: obdctl create-blank sends a create command with the blank object` —
  the real obdctl binary executed against a test-owned UDS: the wire line
  is `cmd:"create"` with the `blank` object (`size`, optional `mkfs`) plus
  `global`/`dev_id`, image-mode create still sends `config`, an `ok:false`
  reply exits 1, and malformed CLI input exits 2 without connecting.
- `supervisor: blank create serves a writable zero base and commit seals its upper` — the `obdctl create-blank` mode-2 path against a real daemon (ADR-0014).
- `supervisor: mode-3 mkfs runs only when the blank spec requests it` — the `obdctl create-blank --mkfs` gate and error path against a real daemon (ADR-0014).
- `supervisor: default mkfs runner completes without the reaper stealing it` — the daemon's real fork/exec mkfs runner against a PATH shim: the helper child's status stays with its owner (the reaper reaps device pids only) and mode 3 succeeds.
- `supervisor: mkfs runner maps exit codes and bounds the timeout` — the real runner's mappings: success, nonzero exit, exec-not-found (127), an unsafe type refused before argv, and a bounded timeout that SIGKILLs the helper.
- `supervisor: child spawn execs and reports through the channel` — guards
  the fork/exec path the supervisor uses to start obd-device and the
  JSON-lines status channel back.
- `supervisor: exec failure surfaces as exit 127` — guards that a missing
  or non-executable device binary is reported as a spawn failure (exit
  127), not mistaken for a device-level error.
- `integration: ublk device serves sector reads from a blob` — end-to-end
  obd-device behavior: a real ublk device serves reads assembled from a
  local blob. Requires privileges and `ublk_drv`; self-skips otherwise (by
  design — see [testing.md](./testing.md)).
- `integration: registry pipeline serves a zfile-compressed image` —
  exercises the full image-assembly path obd-device runs, over a mock
  registry with a ZFile-compressed layer.
- obd-mkimage's writers are pinned by round-trip tests:
  `format: lsmt round-trip and multi-layer merge semantics` (the LSMT
  single-layer writer output reads back and merges correctly) and
  `format: zfile round-trip reads back the original content` (the ZFile
  writer output decompresses byte-identically).
- `cli: obd-convert builds a deterministic ext2 layer from tar` — executes the
  real converter's built-in backend twice, once from a tar file and once from
  stdin, requires byte-identical LSMT sha256 output, verifies the printed
  digest/metadata, and reads the produced layer back as an ext2 image to check
  file content, mode, uid/gid, explicit zero modes and symlink target. It also
  covers an explicit aligned `--size` value.
- `cli: obd-convert defaults to libe2fs when the backend is enabled` — executes
  the converter without `--backend`; `OBD_ENABLE_LIBE2FS_BACKEND=ON` builds must
  report `libe2fs`, while dependency-free builds report `builtin-ext2`. The
  output is run twice to verify deterministic layer bytes and read back through
  the ext2 test view, including the same metadata assertions as the built-in
  backend test.
- `cli: obd-convert libe2fs expands built-in file and directory limits` — in
  libe2fs-enabled builds, converts a regular file that requires double-indirect
  ext2 metadata, verifies finite explicit `--size` values that cannot hold
  metadata or the libe2fs minimum-group boundary fail before publishing output,
  proves the built-in backend rejects that file, verifies a directory that exceeds
  the built-in 12-data-block limit is present in the libe2fs output, and checks
  libe2fs-only coverage for the large-file ro-compat feature, slow symlink
  payloads and uid/gid values above 65535.
- `obd-convert-libe2fs-install-runtime-path` — installs the build tree, checks
  that the installed binary advertises `$ORIGIN/../lib/overlaybd-elio`, verifies
  that `libext2fs.so.2` resolves from that relative bundled directory, and
  verifies that `libcom_err.so.2` is not bundled there.
- `cli: obd-convert rejects unsupported tar entries before writing a layer` —
  proves unsupported tar entry types, non-ustar headers, empty streams,
  malformed end-of-archive markers, regular files beyond the built-in
  single-indirect backend limit, too-small explicit `--size` values, tar contents
  plus ext2 metadata beyond the image budget, directories beyond the built-in
  direct-block backend limit, and inode counts beyond the bitmap capacity fail with exit 1 and
  do not publish an LSMT output file.
- `cli: obd-convert atomically replaces existing output symlinks` — verifies
  converter outputs are completed in a private temporary workspace and published
  by rename instead of following or truncating an existing output symlink.

Run with:

```bash
cmake -S . -B build -DOBD_BUILD_TESTS=ON && cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

## Limitations & TODO

- Recovery respawns are bounded (`max_recovery_attempts`, default 3);
  beyond the bound the device stays down and re-creating it is the
  operator's job (`destroy` + `create`). See
  [operations.md](./operations.md) and ADR-0010.
- No combined `--opt=value` form and no short-option aliases (other than
  `-h`) on any binary.
- obdctl is one-shot: one command per invocation, no interactive or batch
  mode.
- obd-mkimage builds single-layer images only, on a synchronous cold path;
  it is a fixture generator, not a replacement for the upstream
  `overlaybd-*` image toolchain.
- Ordinary obd-convert conversion supports regular files, directories and symlinks from
  ustar input. The default `libe2fs` backend raises the built-in backend's small
  ext2 writer limits but still caps the current unindexed directory path at
  65536 in-memory tar nodes and 1024 data blocks per directory; it does not yet
  implement PAX/GNU long names, hardlinks, device nodes, FIFOs, sparse tar files,
  xattrs or wider ext4 feature selection. The explicit `builtin-ext2` backend is
  intentionally bounded: ext2-compatible output only, 4 KiB blocks, images up to
  128 MiB or an explicit aligned `--size` budget, uid/gid up to 65535, at most
  32768 inodes, regular files up to 4,243,456 bytes, directories up to 12 data
  blocks and short inline symlinks.
- obd-supervisor runs host `mkfs.<type>` for mode-3 blank creates only;
  the mkfs binaries are host prerequisites for that mode, never bundled
  (ADR-0014).
- obd-device supports exactly one image per process by design (ADR-0004);
  multi-device serving will not be added to it.

### TurboOCI conversion and import (ADR-0020)

`obd-convert --turboOCI --input original.tar.gz --out-dir output` retains the
original local tar/gzip blob as the payload source. The libe2fs backend produces
`output/layer/layer-0/ext4.fs.meta`; gzip inputs also produce `gzip.meta`
in that directory. The deterministic
`turboOCIv1.tar.gz` package contains the filesystem metadata, `.turbo.ociv1`
marker, and optional index. The stdout JSON includes runnable local `lowers`,
the package path, and an OCI descriptor with upstream target annotations.
The original blob must remain available for reads. Move it only together with
an appropriate configuration update.

Save the stdout JSON as `descriptor.json` when importing the package elsewhere:

```sh
obd-convert --turboOCI --input original.tar.gz --out-dir generated > descriptor.json
obd-convert --import-turboOCI generated/layer/layer-0/turboOCIv1.tar.gz \
  --descriptor descriptor.json --input original.tar.gz \
  --out-dir imported --name rootfs > imported-config.json
```

Import verifies package and target identities against the descriptor and
validates the metadata before publishing `imported/rootfs`, which must not
already exist. Both an OCI descriptor object and the converter's stdout wrapper
are accepted for `--descriptor`. Publishing blobs to a registry remains the
external CLI responsibility. EROFS and reverse materialization are outside this
feature's scope.

Repeat `--input` in bottom-up parent order to convert an OCI layer stack. The
converter applies whiteouts and opaque directories before the current layer's
entries, preserving surviving hardlinks and parent content. It publishes the
complete stack atomically under `<out-dir>/<name>` (default name `layer`), which
must not already exist. Each `layer-N` directory contains its metadata package
and descriptor; the top-level JSON contains ordered `lowers`, `packages`, and
`descriptors` arrays. `--keep-raw` retains the final filesystem at the path in
`converter.raw_file` for inspection.

The TurboOCI path requires libe2fs and local regular input files. It supports
USTAR, local/global PAX, GNU long names and links, hardlinks, symbolic links,
device nodes, FIFOs, binary xattrs, and GNU sparse PAX 0.1 and 1.0. GNU sparse
0.0 and old GNU `S` entries are rejected explicitly. Each archive is bounded to
65,536 entries, 16 MiB per extension, 64 MiB retained path/extension metadata,
and one million sparse spans. Gzip input must contain one complete member;
concatenated members and trailing bytes are rejected.

TurboOCI filesystem metadata uses 256-byte ext-family inodes with the
`EXTRA_ISIZE` feature (ext4-compatible timestamp extensions, without a journal
or extent trees). Input `mtime` is preserved at nanosecond precision, including
negative times and the extended epoch range; synthesized timestamps remain
fixed for deterministic output. Nonzero precision finer than a nanosecond and
timestamps outside the representable range are rejected. Explicit directory
entries replace their complete xattr set; creating an implicit parent does not
clear attributes inherited from lower layers.
