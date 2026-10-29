# Configuration Reference

overlaybd-elio reads two configuration files, both compatible with the
upstream overlaybd / overlaybd-snapshotter schemas:

- the **global config** (`overlaybd.json`) — daemon-wide settings:
  credentials, DART P2P proxy, download defaults, log level;
- the **per-image config** (`config.json`) — written by the
  overlaybd-snapshotter (or by hand), describing one image's blob
  location and layers, plus the optional writable upper (ADR-0008).

The parser entry points are `GlobalConfig` / `ImageConfig`
(`src/image/config.hpp::GlobalConfig`, `src/image/config.hpp::ImageConfig`).

**Compatibility rules (operator contract, changing them needs an ADR):**

- **Unknown fields are ignored.** New upstream fields never break
  parsing; older overlaybd-elio versions tolerate newer configs.
- **Known fields keep their overlaybd-snapshotter meaning.** Field
  names, types, units, and defaults below follow upstream overlaybd.
- **Malformed JSON is fatal** for the file being parsed
  (`obd::format_error`); a missing file is an IO error
  (`obd::error`). A missing *credential* file is explicitly **not**
  fatal — see below.
- A field present with the wrong JSON type (e.g. a string where a bool
  is expected) is a parse error, not a silent default.

## Global config: `overlaybd.json`

Top-level object; every section is optional.

### `credentialConfig`

Selects where registry credentials come from. **Only `mode = "file"` is
honored**; any other mode is ignored (no inline or secret-store
credential modes).

| Field | Type | Default | Meaning |
|---|---|---|---|
| `mode` | string | `"file"` | Only `"file"` is honored. |
| `path` | string | `"/opt/overlaybd/cred.json"` | Credential file location (see §Credential file). |

### `p2pConfig` (DART proxy, ADR-0005)

Optional P2P acceleration through an **external** DART proxy. When
enabled, blob HTTP requests are rewritten as prefix-passthrough GETs:
`http://<address>/<full upstream URL>`.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `enable` | bool | `false` | Master switch for DART acceleration. |
| `address` | string | `""` | `host:port[/prefix]`, e.g. `"localhost:19145/dart"`. An optional `scheme://` is stripped (DART speaks plain HTTP); the prefix defaults to `/dart` (`src/source/dart.cpp` address parsing). |

At image-open time the proxy is probed with a short (1 s) reachability
budget: if it is unreachable — or the address is malformed — the device
logs a warning and falls back to direct registry reads. An
enabled-but-unreachable proxy therefore delays startup by at most the
probe budget, it never blocks the image.

### `download`

Global defaults for the LayerStore background fill. The per-image
`download` section overrides these **per field** (see below). Defaults
come from `src/image/config.hpp::DownloadConfig`:

| Field | Type | Default | Meaning |
|---|---|---|---|
| `enable` | bool | `false` | Start the background fill after device open. |
| `delay` | uint32 | `300` | Fill start delay in seconds after device open. |
| `delayExtra` | uint32 | `30` | Plus a uniform random extra of 0..`delayExtra` seconds. |
| `maxMBps` | uint32 | `100` | Fill throughput throttle, MiB/s. |
| `tryCnt` | uint32, range `1..4294967295` | `5` | Completion-verify attempts before giving up (a failed sha256 verification discards the staging pair and restarts); zero, negative, and out-of-range values are structural configuration errors (ADR-0018). |
| `blockSize` | uint32 | `262144` (256 KiB) | Fill range-read coalescing cap in bytes (contiguous missing extents are fetched in reads of up to this size, never more than 1 MiB, and split back into 64 KiB extents for accounting). |

With `enable` set, every remote layer's `LayerStore` starts a background
fill coroutine at open: a scavenger-class walk that warms every extent
nobody has read yet into the layer's staging pair (sparse staging file
`<dir>/.download.<nonce>` + sidecar `<dir>/.bitmap.<nonce>`), completing
through the same sha256-verify + atomic rename to
`<dir>/overlaybd.commit` as read-through warming. Fill traffic is
throttled by `maxMBps`, delayed by `delay`+`delayExtra`, runs at
concurrency 1 per layer, and back-pressures itself — readers are never
queued behind it. Locality grows with reads regardless of
`download.enable`; the knob only controls the proactive whole-layer warm.
If whole-file verification fails while retry attempts remain, the fill
walk follows the fresh staging pair automatically; `tryCnt` bounds those
automatic fill-driven verification attempts just as it bounds read-through
warming attempts.

### `prefetch` (ADR-0012/0013)

Bring-up warm-up: the structural head/tail prefetch (ADR-0012's
cold-start floor) and trace replay (ADR-0013). **Honored fields:
`enable`, `head_kb`, `tail_kb`** — previously the whole section was
parsed-tolerated (ignored); with the ADR-0012 admission funnel the
master switch became meaningful, and the structural window sizes landed
with the warm-up itself. The funnel's AIMD window is deliberately
**not** operator-configured (ADR-0012 rejects static budgets), so no
window or AIMD fields are exposed.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `enable` | bool | `true` | Master switch for BOTH warm-up kinds: when false, no warm-up traffic is issued at bring-up — an acceleration layer's trace blob is neither loaded nor replayed (the layer is still set aside from the merge — recognition is structural), and the structural head/tail windows are not populated. Warm-up traffic rides the funnel's Prefetch scavenger class either way. |
| `head_kb` | uint | `1024` | Structural warm-up head window, in KiB: the first `head_kb` KiB of every data lower's stored blob (the tar-stripped payload byte space — ZFile/LSMT headers and first data blocks; the tar header itself sits in the same first underlying extents) are populated at bring-up. `0` disables the head window. Accepted range 0..4294967295 (uint32) — out-of-range values are rejected with `EINVAL` at parse time (a negative would otherwise wrap to ~4 TiB and silently warm whole layers). |
| `tail_kb` | uint | `1024` | Structural warm-up tail window, in KiB: the last `tail_kb` KiB of every data lower's stored blob payload — the region carrying the ZFile jump table + trailer and the LSMT index — are populated at bring-up. `0` disables the tail window. Accepted range and rejection as for `head_kb`. |

A blob smaller than `head_kb + tail_kb` is warmed whole (the clamped
windows merge — no byte is populated twice). Warm-up is opportunistic:
failures are logged and skipped, never a bring-up error; see
`docs/image.md` → "Structural warm-up".

### `logConfig`

| Field | Type | Default | Meaning |
|---|---|---|---|
| `logLevel` | int | `1` | 0 = debug, 1 = info, 2 = warn, 3 = error. |

### `ublkConfig`

| Field | Type | Default | Meaning |
|---|---|---|---|
| `enableRecovery` | bool | `true` | ADR-0010: create devices with `UBLK_F_USER_RECOVERY` so a crashed device process can be replaced without failing the block device. Disable only for debugging; kernels without the feature fall back automatically. |
| `queues` | int, 0..256 | `0` | Production queue count; zero selects automatic sizing or matches an explicit `workers` value (ADR-0025). |
| `workers` | int, 0..256 | `0` | Elio runtime worker count; zero matches explicit queues or participates in automatic sizing. Explicit workers must be at least the queue count. |

When both counts are zero, a newly created device uses equal queue and worker
counts: at least one pair, at most eight, and no more than the detected CPU
budget. That budget combines process CPU affinity and visible cgroup v1/v2
CPU quotas; fractional quotas round up. If quota files are unavailable, affinity
is used; affinity-probe failure falls back to hardware concurrency, then one.
Restrictions hidden outside the mount namespace cannot be inferred.

If exactly one count is positive, the other matches it. Set both fields to
provision additional workers, for example `{"queues": 4, "workers": 6}`.
Manual counts may exceed the eight-pair automatic ceiling. Negative, fractional,
boolean, null, string and out-of-range count values are configuration errors,
as is an explicit worker count smaller than the queue count. Validation occurs
before scheduler construction and new-device creation.

Invalid topology fields are usage errors (exit 2). Operational global-config
or recovery-probe failures keep the existing startup control-endpoint cleanup
path and exit 1. Image assembly diagnostics retain their original precedence
over a failed recovery probe.

These global settings apply to image and blank devices, including supervised
and standalone startup. The file is read once per process startup. A recovery
uses the existing kernel queue count, depth and maximum buffer size; automatic
workers match that queue count even if the detected CPU budget has changed.
An explicit queue mismatch, insufficient workers or unsupported geometry is
rejected before recovery admission. The automatic eight-pair ceiling applies
to new creation, not to a previously created layout. Geometry is revalidated
before the recovery handshake. Counts do not change while serving requests.

Each queue's default request buffers occupy 16 MiB (128 tags × 128 KiB), so
eight queues allocate 128 MiB of those buffers per device, plus ring, worker,
handler and source memory. Startup logs show requested counts, resolved queues,
actual runtime workers and serving queues; see [operations.md](./operations.md).

Recovery flags do not authorize cleanup of an unadopted old device. Under
ADR-0022, failed startup with outstanding ownership retains its process and
resources until supported release. Disabling `enableRecovery` does not disable
startup ownership accounting or make timeout/EOF a retirement barrier. No
configuration option selects an automatic destructive fallback. The
`--startup-generation` grant is supervisor-generated protocol metadata, not an
image or global configuration field.

### Recognized but not honored

`cacheConfig` and `ioEngine` are parsed-tolerated (ignored as unknown
sections) in the current version; see *Limitations & TODO* in
`docs/architecture.md`. (`prefetch` was in this list until the ADR-0012
admission funnel landed; its `enable`, `head_kb`, and `tail_kb` fields
are now honored — see above.)

## Per-image config: `config.json`

> **Blank (raw) devices take no config.** The ADR-0014 blank creation
> modes (`create-blank` / `create` with a `blank` object) have no
> `config.json` at all: the device is assembled from the requested size
> and the supervisor's `--blank-dir` workspace (`overlaybd.zero` zero
> base + `overlaybd.rw` upper — see [image.md](./image.md) and
> [supervisor.md](./supervisor.md)). Everything below applies to image
> (`config`) mode only.

One file per device, passed to `obdctl create <id> <config.json>` and
through to the `obd-device` child.

**Device size (D3).** A device's `dev_size` defaults to the image's
declared virtual size (the assembled merged size of `lowers[]`; the
`config.json` does not carry a size field). `obdctl create` accepts a
CLI-level `--virtual-size <bytes>` headroom override (grow-only: at
least the image's declared size, validated with the single rule
`image::device_capacity_bytes`). For a WRITABLE image (`upper` set) the
override sizes the writable top — and hence the merged DATA PLANE — to
the override during assembly (`open_image`'s override parameter), so
writes into the headroom land in the upper and a plain commit seals the
larger declared size; for a read-only image it is dev-size-only (reads
past the image's end are zero-filled). No config field is added, so
existing images and snapshotter output are unaffected. `obdctl commit
--virtual-size` re-baselines a sealed layer's declared size explicitly
(docs/supervisor.md, "Offline commit").

### `repoBlobUrl`

String, default `""`. The registry blob base URL for this repo, e.g.
`"https://registry-1.docker.io/v2/library/redis/blobs"`. Each remote
lower is fetched as `<repoBlobUrl>/<digest>`. **Required whenever at
least one lower has no local blob** — opening an image with a remote
lower and an empty `repoBlobUrl` fails with `EINVAL`.

### `lowers` (required, non-empty)

Array of layer objects, **bottom-up**: `lowers[0]` is the base layer.
An image with no lowers is rejected.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `digest` | string | `""` | Content digest, `"sha256:<hex>"`. Used in the fetch URL and as the `LayerStore` completion integrity-check value (the `"sha256:"` prefix is stripped; other algorithms disable verification). |
| `size` | uint64 | `0` | Blob size in bytes (informational for the reader). |
| `dir` | string | `""` | Per-layer directory — the local probe location and the `LayerStore` persistence directory (staging pair + `overlaybd.commit`; created if missing). **Empty for a remote layer disables persistence**: the layer is served remote-only, straight from the registry (a warning is logged; the snapshotter always sets `dir`, so this is the compatibility path). |
| `file` | string | `""` | Explicit local blob file; empty means the layer may be remote. |

**Local probe order** — a lower is served locally when one of these
exists as a regular file, in this order (`src/image/image_file.cpp::probe_local_blob`):

1. `file` (explicit blob path);
2. `<dir>/overlaybd.commit`;
3. `<dir>/.commit`;
4. `<dir>/overlaybd.sealed`.

Otherwise the lower is fetched remotely through `repoBlobUrl`.

### `accelerationLayer` (optional; trace prefetch, ADR-0013 accepted)

Boolean, default `false`. The snapshotter's signal that the **uppermost
lower is the acceleration (trace) layer**, not a data layer — the same
field upstream's backstore config (`config.v1.json`) carries
([trace-format.md](./trace-format.md) §6). When true, image assembly sets
the last lower aside from the merge and replays its trace blob as
`populate` warm-up on the data lowers (see `docs/image.md` → "The trace
layer"). Setting it requires at least one data lower beneath the trace
layer (`EINVAL` otherwise); a missing, malformed, or timed-out trace blob
only disables prefetch, never device bring-up. `recordTracePath` (upstream's
config-file recording trigger) is parsed-tolerated but ignored; live
recording is controlled by the supervisor `trace_start` / `trace_stop`
protocol instead.

### `upper` (optional; writable mode, ADR-0008)

Object. When present with a non-empty `dir`, the device becomes
**writable**: the sealed lowers are merged under a copy-on-write upper
layer (`MergedWritable`). An absent `upper`, an empty object `{}`, or an
empty `dir` keeps the device read-only.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `dir` | string | `""` | Directory holding the upper layer file; created if missing. Non-empty engages writable mode (`src/image/config.hpp::UpperConfig`). |
| `type` | string | `"lsmt"` | Upper layer format: `"lsmt"` → `<dir>/overlaybd.rw` plus `<dir>/overlaybd.rw.index` (unsealed in-place-edit LSMT pair); `"sparse"` → `<dir>/overlaybd.sparse` with an inode format marker and extent ownership (ADR-0037; 4 KiB-aligned capacity); `"hybrid"` → `<dir>/overlaybd.hybrid/{data,index}` (persisted mapping log and metadata-only seal, ADR-0026). **Any other value is rejected** with `EINVAL`. |

Durability note: the `lsmt` upper recovers completed operations from its
upstream-compatible data/index pair after process exit (ADR-0029). Explicit
FLUSH synchronizes storage; interrupted writes may be partial. Both files must
be retained. Missing or invalid metadata fails without recreating the upper.
A valid legacy shutdown checkpoint is migrated before writable use; old
memory-only uppers without a checkpoint cannot recover. Sparse commit exports
a separate sealed LSMT delta after stop/reap and ownership recovery,
retaining the reusable source (ADR-0027).

Hybrid is opt-in. Image assembly atomically creates an absent pair directory or
reopens both existing files; an incomplete pair fails without recreating either
file. Recovery preserves a larger persisted virtual size, and a larger requested
size grows the pair durably. An explicit headroom override smaller than that
persisted size is rejected. FLUSH and crash semantics are specified in
[format.md](./format.md) and ADR-0026; in-place overwrites do not promise power-loss
atomicity. Existing-image Hybrid uppers use the normal shutdown checkpoint and
stopped-device commit path. Managed writable Hybrid pairs require user-xattr support for local log integrity.
Blank devices select Hybrid explicitly with `blank.upper_type = "hybrid"`;
the default remains LSMT. See ADR-0026 for the contract and validation limits.

### `download` (per-image overrides)

Same fields as the global `download` section. Merged **over the global
defaults per field**: only fields present in the image's section
override; absent fields inherit the global value
(`src/image/config.cpp::apply_download_json`).

### `resultFile`

String, default `""`. **Informational only** in the current version:
parsed for compatibility, never written or acted upon.

## Credential file

Path from `credentialConfig.path` (default `/opt/overlaybd/cred.json`).
Docker config-json style:

```json
{
  "auths": {
    "registry-1.docker.io": {
      "auth": "dXNlcjpwYXNz"
    },
    "https://registry.example.com/v2": {
      "username": "robot",
      "password": "s3cret"
    }
  }
}
```

Semantics (`src/source/credentials.cpp::CredentialStore`):

- Each `auths` entry carries either `auth` (base64 of `user:pass`,
  Docker-style) or explicit `username` / `password` fields.
- Lookup is **longest-prefix matching**: entries are kept sorted
  longest-key-first and the first key that prefixes the request URL
  wins. Matching is tried against both the full URL and the URL with
  its `scheme://` prefix stripped, so keys may be written with or
  without the scheme. Empty keys never match.
- **A missing or unreadable credential file is not fatal**: the open
  logs a warning and pulls anonymously. A *malformed* credential file
  is fatal (`obd::format_error`). An empty file or one without an
  `auths` object yields an empty store (anonymous pull).

## Complete examples

### `overlaybd.json`

```json
{
  "credentialConfig": {
    "mode": "file",
    "path": "/opt/overlaybd/cred.json"
  },
  "p2pConfig": {
    "enable": true,
    "address": "localhost:19145/dart"
  },
  "download": {
    "enable": true,
    "delay": 60,
    "delayExtra": 30,
    "maxMBps": 100,
    "tryCnt": 5,
    "blockSize": 262144
  },
  "logConfig": {
    "logLevel": 1
  }
}
```

### Read-only `config.json`

```json
{
  "repoBlobUrl": "https://registry.example.com/v2/demo/app/blobs",
  "lowers": [
    {
      "digest": "sha256:1c6f2e48...a1",
      "size": 7340032,
      "dir": "/var/lib/overlaybd/io.containerd.snapshotter.v1.overlaybd/snapshots/12/block",
      "file": ""
    },
    {
      "digest": "sha256:99bd04c2...7e",
      "size": 2097152,
      "dir": "/var/lib/overlaybd/io.containerd.snapshotter.v1.overlaybd/snapshots/15/block",
      "file": ""
    }
  ],
  "download": {
    "enable": true,
    "maxMBps": 50
  },
  "resultFile": "/tmp/overlaybd.log"
}
```

### Writable `config.json` (ADR-0008)

```json
{
  "repoBlobUrl": "https://registry.example.com/v2/demo/app/blobs",
  "lowers": [
    {
      "digest": "sha256:1c6f2e48...a1",
      "size": 7340032,
      "dir": "/var/lib/overlaybd/snapshots/12/block",
      "file": "/var/lib/overlaybd/snapshots/12/block/overlaybd.commit"
    }
  ],
  "upper": {
    "dir": "/var/lib/overlaybd/snapshots/16/rw",
    "type": "lsmt"
  }
}
```

This assembles one local lower plus a writable upper at
`/var/lib/overlaybd/snapshots/16/rw/overlaybd.rw`
(`"type": "sparse"` would use `overlaybd.sparse` instead).

### TurboOCI lower targets (ADR-0020)

A TurboOCI lower pairs its filesystem metadata `file` with the original OCI
layer through `targetFile` (local path) or `targetDigest` (registry SHA-256 blob
identity, `sha256:` followed by exactly 64 lowercase hexadecimal digits).
When both are supplied, the local target is used. The target is the
complete original archive: its tar header is not stripped. A remote target uses
`repoBlobUrl` and the same registry authentication and read-admission funnel as
ordinary lowers. With `dir`, its raw original bytes persist separately under
`<dir>/targets/<target-sha256-hex>`; a completed target cache can reopen offline.
Reopening sweeps stale staging pairs in that target directory. As with ordinary
committed layers, completion verifies SHA-256 before publication; reopening
trusts the committed file without scanning the entire blob again.
The target cache is below gzip decoding and cannot collide with the metadata
cache. Without `dir`, or when that directory cannot support persistence, reads
use the remote source through the admission funnel.

`gzipIndex` is a local path to the upstream `ddgzidx` v1 restart index for a gzip
encoded target. It requires a target; orphan indexes and malformed target
digests are configuration errors. Without `gzipIndex`, target offsets address
uncompressed tar bytes directly. The metadata lower may independently use the
ordinary ZFile envelope. Metadata packages must be imported before their
extracted filesystem metadata and gzip index paths are used here.

Hybrid USER_RECOVERY requires both existing artifacts and uses their persisted
virtual size, ignoring an obsolete create-time headroom override after online
grow. Missing recovery artifacts fail closed; they never trigger fresh creation.
This differs from normal first creation, which may publish a new pair directory.
