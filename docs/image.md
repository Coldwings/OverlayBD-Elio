# Image Module (`src/image`)

Configuration parsing (`config.json` / `overlaybd.json`) and image assembly:
turning an overlaybd-snapshotter image config into one merged block source,
optionally with a writable upper layer.

## Overview

The image module is the bridge between operator-facing configuration and the
read stack. It has two halves:

- **Config parsing** (`config.hpp` / `config.cpp`) — the two JSON files an
  operator provides:
  - `GlobalConfig` — the daemon-wide `overlaybd.json`: `credentialConfig`
    (mode=file), `p2pConfig` (the DART proxy, ADR-0005), global `download`
    defaults, `logConfig`.
  - `ImageConfig` — the per-image `config.json` written by the
    overlaybd-snapshotter: `repoBlobUrl`, the bottom-up `lowers[]` list,
    per-image `download` overrides, `resultFile`, and optionally `upper` —
    a writable layer (ADR-0008).
- **Assembly** (`image_file.hpp` / `image_file.cpp`) — `open_image` walks
  the lowers, builds the per-layer source stack out of `src/source` pieces,
  wraps each with the `src/format` readers, and merges everything into a
  single root source: a `MergedLsmt` for the default read-only image, or a
  `MergedWritable` when an `upper` is configured.

The module contains no IO logic of its own beyond assembly; every byte is
served by `src/source` and decoded by `src/format` (see `docs/source.md` and
`docs/format.md`).

Compatibility is an operator contract: unknown config fields are ignored,
and known fields keep their overlaybd-snapshotter meaning (see
`docs/config.md`; wire-contract rule in `AGENTS.md`).

## Concepts

### Two configuration layers

`GlobalConfig` is parsed once per device process; `ImageConfig` is parsed
per image **on top of the global download defaults**: the image's `download`
object overrides the global one field by field (only present fields
override). All other global settings (credentials, DART, log level) apply
to every image in the process.

### Lowers, bottom-up

`lowers[]` is ordered bottom-up: `lowers[0]` is the base layer and the last
entry is the topmost read-only layer. Blob identity is the OCI `digest`
(`"sha256:<hex>"`); the hex payload doubles as the integrity-check value for
background downloads.

### The per-lower assembly chain

For each lower, in order:

1. **Local probe** — if `lower.file` names a regular file, use it; else probe
   the per-layer directory for the commit markers, in order:
   `<dir>/overlaybd.commit`, `<dir>/.commit`, `<dir>/overlaybd.sealed`.
   A hit yields a `LocalFileSource`.
2. **Remote chain** — otherwise the layer is remote, addressed as
   `repoBlobUrl + "/" + digest`:
   `RegistrySource` (on the image-wide shared `RegistryClient`, with the
   DART accelerate prefix when `p2pConfig` is enabled **and** the proxy is
   reachable) → `LayerStore` (ADR-0011): one remote source per layer,
   read through by the store, which persists every served extent into
   `lower.dir` (created if missing) as a sparse staging file plus sidecar
   bitmap, and renames it to `<dir>/overlaybd.commit` once the layer is
   complete and sha256-verified. A remote layer with an **empty**
   `lower.dir` cannot persist — it keeps the legacy in-memory
   `ChunkCache` in front of the `RegistrySource` (with a warning;
   restart-cold, retired with part 3). The `download` section is
   still parsed (operator contract) but currently has no effect: locality
   already grows read-through; background whole-blob fill returns as
   LayerStore fill in the ADR-0011 follow-up (part 3).
3. **TarOffsetSource** — auto-detects and removes the tar wrapper.
4. **ZFile detection** — when `format::is_zfile()` recognizes the ZFile
   magic, the view becomes a `ZFileSource` (with caller-side digest
   verification); otherwise the raw (uncompressed) view is used.
5. **LsmtLayer** — the format reader that exposes the layer's segment
   index.

Finally all layers are merged: `MergedLsmt` for read-only images (the last
lower wins on overlaps), or — with a configured `upper` — `MergedWritable`,
whose topmost layer is the writable upper and whose reads fall through to
the merged lowers.

### The trace layer (ADR-0013, proposed)

When the image config carries `accelerationLayer: true`, the **uppermost
lower is the acceleration (trace) layer**, not a data layer
([trace-format.md](./trace-format.md) §6). Assembly sets it aside — it is
excluded from the LSMT merge, so device reads never see trace bytes — and
replays its trace blob as cache warm-up.

**Recognition chain finding.** Upstream's identification chain is: layer
annotation `containerd.io/snapshot/overlaybd/acceleration-layer=yes` →
containerd snapshot label → the overlaybd snapshotter writes the backstore
config (`config.v1.json`) with `accelerationLayer: true` and appends the
acceleration layer as the last lower → the backstore pops the last lower
and looks for `<dir>/trace`. Our `ImageConfig` **is** the backstore config
equivalent, so the signal surfaces as the top-level `accelerationLayer`
boolean (added with this feature; see `docs/config.md`). Recognition
therefore = the config flag + the blob's magic check: the trace layer is
the last lower; its blob is loaded (locally `<dir>/trace` — the upstream
lookup name — or `lower.file`; remotely the tar-wrapped layer blob read
through `TarOffsetSource`) and validated by the trace codec
(`src/format/trace.hpp`). A magic mismatch means the layer carries a
dynamic-prefetch file list instead — unsupported by design (ADR-0013) —
and is logged and ignored.

**Replay.** Each READ record `{layer_index, offset, count}` becomes
`populate(offset, count)` on the corresponding data lower's
**stored-blob-level** source (the `TarOffsetSource` view — the same byte
space upstream's `PrefetchFile` wraps, below decompression), executed in
recorded order and sequentially awaited. The admission funnel that would
deprioritize this traffic is B-phase work (not merged); replay calls
`populate` directly. Skip rules follow upstream replay parity
(trace-format.md §5/§8): non-READ ops, unknown layer indexes, zero
counts, counts above the 1 MiB conforming-writer cap, and negative
offsets are silently skipped. Replay is **opportunistic**: a missing,
malformed, or stale trace and any individual populate failure are logged
and never fail device bring-up.

**Replay bounds** (`src/image/trace_replay.hpp::TraceReplayOptions`) keep
a hostile or stale trace from amplifying registry traffic or delaying
bring-up without limit:

| Bound | Default | Rationale |
|---|---|---|
| `max_records` | 65536 | Real recorded traces carry thousands of records (one per pread during container start); 64k is far above legitimate sizes yet bounds the loop. |
| `max_bytes` | 1 GiB | Warm-up beyond ~1 GiB delays the cold start more than it saves; with the 1 MiB per-record cap this also bounds extent-rounding amplification. |
| `max_wall_time` | 30 s | Replay is awaited during bring-up; this caps the worst-case bring-up delay. |
| `max_record_count` | 1 MiB | The upstream replay buffer cap (trace-format.md §8/§10); larger records come only from non-conforming writers and are skipped, not clamped. |

The trace blob itself is read with a 64 MiB cap (a conforming trace is
~1.5 MiB at the record bound).

### The writable mode (ADR-0008)

A non-empty `upper` object in the image config engages the writable device
mode:

- `upper.dir` — directory holding the writable layer file (created if
  missing);
- `upper.type` — `"lsmt"` (default) → `<dir>/overlaybd.rw`, an
  in-place-edit LSMT layer (`LsmtRwLayer`); `"sparse"` →
  `<dir>/overlaybd.sparse`, a fiemap sparse file (`SparseRwLayer`);
- the upper is sized to cover the whole image (the maximum of the lowers'
  virtual sizes); writes land in the upper, reads fall through (copy-on-write
  — the lowers are never modified);
- the root then implements `WritableBlobSource`, and `OpenedImage` reports
  `writable = true` plus the `upper_path`;
- an **unknown** `upper.type` is rejected at parse time with
  `obd::error(EINVAL)`; an absent or empty `upper` object keeps the
  read-only behavior exactly.

The rationale and scope live in ADR-0008; writable layers and TurboOCI
beyond this scope remain out (ADR-0007).

## Public API

All types live in namespace `obd::image`. Parsing functions are synchronous
(plain file IO + JSON) and scheduler-free; `open_image` is an Elio coroutine.

### `config.hpp` — GlobalConfig

```cpp
struct GlobalConfig {
    std::string credential_file;
    bool p2p_enable = false;
    std::string p2p_address;
    source::DownloadConfig download;
    int log_level = 1;

    static GlobalConfig from_file(const std::string& path);
    static GlobalConfig from_json_text(const std::string& text);
};
```

`src/image/config.hpp::GlobalConfig` — the daemon-wide `overlaybd.json`.

- `src/image/config.hpp::credential_file` — from `credentialConfig`: **only
  `mode=file` is honored**; `path` defaults to `/opt/overlaybd/cred.json`.
  Inline/secret modes leave this empty (the caller warns; see
  `docs/config.md`).
- `src/image/config.hpp::p2p_enable` / `src/image/config.hpp::p2p_address` —
  from `p2pConfig` (`enable`, `address`); the DART proxy (ADR-0005).
- `src/image/config.hpp::download` — the global download defaults
  (`enable`, `delay`, `delayExtra`, `maxMBps`, `tryCnt`, `blockSize` →
  `src/source/downloader.hpp::DownloadConfig`); per-image `download`
  sections override these field by field. Parsed for compatibility but
  currently inert (ADR-0011 part 2 — see Limitations & TODO).
- `src/image/config.hpp::log_level` — `logConfig.logLevel`:
  0=debug, 1=info (default), 2=warn, 3=error.
- `from_file(path)` — reads and parses the file. Throws `obd::error` on IO
  failure, `src/common/errors.hpp::format_error` on malformed JSON.
- `src/image/config.hpp::from_json_text` — same, from an in-memory string
  (tests, the supervisor's config channel).
- `cacheConfig`, `ioEngine`, and `prefetch` sections are intentionally not
  honored in v0.1; unknown fields are ignored per the operator contract.

### `config.hpp` — LowerConfig, UpperConfig, ImageConfig

```cpp
struct LowerConfig {
    std::string digest;   // "sha256:<hex>"
    uint64_t size = 0;
    std::string dir;      // per-layer directory (download cache)
    std::string file;     // local blob file when present ("" = remote)
};

struct UpperConfig {
    std::string dir;
    std::string type = "lsmt";   // "lsmt" | "sparse"
};

struct ImageConfig {
    std::string repo_blob_url;
    std::vector<LowerConfig> lowers;   // bottom-up
    std::string result_file;
    source::DownloadConfig download;   // merged over the global defaults
    bool acceleration_layer = false;   // uppermost lower is the trace layer
    UpperConfig upper;
    bool writable() const noexcept;

    static std::string digest_sha256_hex(const std::string& digest);
    static ImageConfig from_file(const std::string& path,
                                 const source::DownloadConfig& defaults);
    static ImageConfig from_json_text(const std::string& text,
                                      const source::DownloadConfig& defaults);
};
```

`src/image/config.hpp::LowerConfig` — one layer. `src/image/config.hpp::digest`
is the OCI digest; `size` is the blob size as recorded by the snapshotter
(informational — the authoritative size comes from the local file or the
registry probe); `dir` is the per-layer directory used for the local probe
and the `LayerStore` persistence state (staging pair and
`overlaybd.commit`; created if missing; empty disables persistence for a
remote layer, which then keeps the legacy in-memory `ChunkCache`); `file`
names a local blob file (empty = the layer is remote).

`src/image/config.hpp::UpperConfig` — the writable upper (ADR-0008).
`src/image/config.hpp::dir` holds the layer file; `src/image/config.hpp::type`
selects `overlaybd.rw` (`"lsmt"`, default) or `overlaybd.sparse`
(`"sparse"`).

`src/image/config.hpp::ImageConfig::acceleration_layer` — from the
top-level `accelerationLayer` boolean (ADR-0013, proposed): marks the
uppermost lower as the acceleration (trace) layer; see Concepts → "The
trace layer".

`src/image/config.hpp::ImageConfig` — the per-image `config.json`.

- `src/image/config.hpp::repo_blob_url` — `repoBlobUrl`, the registry blob
  base; remote layers are fetched from `repo_blob_url + "/" + digest`.
- `lowers` — the bottom-up layer list (`lowers[0]` = base layer).
- `src/image/config.hpp::result_file` — `resultFile`; informational in v0.1
  (see `docs/config.md`).
- `download` — starts as a copy of the global `defaults`, then the image's
  `download` object overrides it field by field.
- `src/image/config.hpp::upper` / `src/image/config.hpp::writable` — `upper`
  is populated only when the config carries a non-empty `upper` object;
  `writable()` is true iff `upper.dir` is non-empty.
- `src/image/config.hpp::digest_sha256_hex` — the digest's hex payload with
  the `"sha256:"` prefix stripped (the `LayerStore` completion-verify
  value); returns empty for any other algorithm.
- `src/image/config.hpp::from_file` / `from_json_text` — parse the config.
  Throw `obd::error` on IO failure, `src/common/errors.hpp::format_error` on
  malformed JSON, and `obd::error(EINVAL)` on an unknown `upper.type`
  (ADR-0008 — anything but `lsmt`/`sparse`). Both take the global download
  `defaults` explicitly. Unknown fields are ignored; known fields keep their
  overlaybd-snapshotter meaning.

### `image_file.hpp` — OpenedImage, open_image

```cpp
struct OpenedImage {
    source::BlobSourcePtr root;   // MergedLsmt or MergedWritable
    uint64_t virtual_size = 0;
    size_t layer_count = 0;       // data layers (trace layer excluded)
    bool writable = false;
    std::string upper_path;
    TraceReplayStats trace;       // ADR-0013 replay outcome
};

elio::coro::task<OpenedImage> open_image(const ImageConfig& cfg,
                                         const GlobalConfig& global);
```

`src/image/image_file.hpp::OpenedImage` — the assembled device view.

- `src/image/image_file.hpp::root` — the merged block source: a
  `MergedLsmt` for read-only images, a `MergedWritable` (which implements
  `src/source/blob_source.hpp::WritableBlobSource`) when writable. The ublk
  data plane reads through this pointer and dispatches writes only when
  `writable` is set.
- `src/image/image_file.hpp::virtual_size` — the device size in bytes (the
  merged view's size).
- `src/image/image_file.hpp::layer_count` — number of DATA lowers (the
  trace layer is excluded), or data lowers + 1 in writable mode (the
  upper counts as a layer).
- `writable` / `src/image/image_file.hpp::upper_path` — whether the root is
  writable, and the writable layer's file path when it is
  (`<upper.dir>/overlaybd.rw` or `<upper.dir>/overlaybd.sparse`).
- `trace` — the `src/image/trace_replay.hpp::TraceReplayStats` outcome of
  trace replay (ADR-0013): whether a valid trace was present, records
  replayed/skipped, bytes warmed, and whether a replay bound stopped the
  pass early. All zero when no `accelerationLayer` was configured.

`open_image(cfg, global)` — assembles the merged view. Cold path; runs on
the calling Elio coroutine during device setup.

Behavior, in order:

1. Rejects an empty `lowers` list with `obd::error(EINVAL)` — an image
   without layers is a configuration error, not an empty device.
2. Loads the credential file when configured; an unreadable/missing file is
   **not fatal** — it logs a warning and pulls anonymously.
3. Resolves DART: when `p2pConfig` is enabled and the address parses, the
   proxy is probed (`dart_proxy_reachable`); only a reachable proxy sets the
   client's accelerate prefix. Malformed addresses and unreachable proxies
   log a warning and fall back to direct registry reads (ADR-0005). One
   `RegistryClient` is shared by all layers of the image.
4. When `accelerationLayer` is set: requires at least one data lower
   beneath the trace layer (`obd::error(EINVAL)` otherwise), sets the
   uppermost lower aside, and loads its trace blob best-effort — every
   load failure only disables prefetch (ADR-0013).
5. Builds each lower per the chain in Concepts. A lower with **no local
   file and an empty `repoBlobUrl`** fails with `obd::error(EINVAL)` —
   there is nowhere to read it from.
6. Replays the trace blob (when loaded) via
   `src/image/trace_replay.hpp::replay_trace`: `populate()` on the data
   lowers' stored-blob-level sources, in recorded order, sequentially
   awaited, bounded by `TraceReplayOptions`. Never fails assembly.
7. Read-only: merges with `MergedLsmt` and returns. Writable
   (`cfg.writable()`): creates `upper.dir` if needed, opens/creates the
   upper (`LsmtRwLayer::create` for `lsmt`, `SparseRwLayer::open` for
   `sparse`) sized to the maximum lower virtual size, merges with
   `MergedWritable`, and returns with `writable = true`.

Error behavior: **all-or-nothing** — any failure (config, credentials parse
aside, network probe, corrupt layer, unsupported format, an unwritable
layer dir) throws `obd::error` /
`src/common/errors.hpp::format_error`; a device that cannot assemble must
not come up half-broken. `open_image` never returns a partially assembled
image. It requires a running Elio scheduler (the DART probe, the registry
size probes, and `LayerStore::open`'s blocking setup via
`elio::spawn_blocking`).

### `trace_replay.hpp` — TraceReplayOptions, TraceReplayStats, replay_trace

```cpp
struct TraceReplayOptions {
    size_t max_records = 65536;
    uint64_t max_bytes = uint64_t{1} << 30;        // 1 GiB
    std::chrono::milliseconds max_wall_time{30000};
    uint64_t max_record_count = 1048576;           // 1 MiB
};

struct TraceReplayStats {
    bool trace_present = false;
    size_t records_total = 0;
    size_t records_replayed = 0;
    size_t records_skipped = 0;
    uint64_t bytes_warmed = 0;
    bool budget_exhausted = false;
};

elio::coro::task<TraceReplayStats> replay_trace(
    std::span<const uint8_t> blob,
    const std::vector<source::BlobSource*>& warm_targets,
    const TraceReplayOptions& opts = {});
```

`src/image/trace_replay.hpp::replay_trace` — parses `blob` with the trace
codec and replays it against `warm_targets` (one stored-blob-level source
per data lower; `layer_index` addresses the vector directly, a nullptr
entry counts as unknown layer). Records are processed in recorded order,
each `populate()` sequentially awaited. **Never throws**: a blob the
codec rejects yields `{trace_present = false}`; non-READ ops, unknown
layer indexes, zero/oversized counts, and negative offsets are skipped
silently (upstream parity); a failed populate is logged and skipped.
Processing stops early on any `TraceReplayOptions` bound
(`budget_exhausted` set). The bounds and their rationale are tabulated in
Concepts → "The trace layer".

## Invariants & Guarantees

- **Deterministic assembly** — the same configs always produce the same
  stack shape and the same virtual size; layer order is exactly the config
  order (bottom-up), and the topmost layer wins on overlapping segments.
- **Read-only by default** — without a non-empty `upper`, the assembled root
  is strictly read-only; nothing in the assembly path can modify lower
  blobs. With an upper, writes are copy-on-write into the upper file only —
  lowers stay byte-identical (guarded by tests, below).
- **Local-first** — a layer with a usable local file (`lower.file` or a
  commit marker in `lower.dir`) never touches the network for that layer.
- **Read-through persistence (ADR-0011)** — every byte a remote layer
  serves is persisted into `lower.dir` by the `LayerStore`, so locality
  grows monotonically and survives restarts; once complete and
  sha256-verified, the staging file is renamed to
  `<dir>/overlaybd.commit`, which the local probe binds directly on the
  next open.
- **DART is strictly optional** — enabling `p2pConfig` can never make an
  image that would otherwise open fail to open (ADR-0005).
- **Fail-loud assembly** — `open_image` either returns a fully assembled
  image or throws; there is no degraded/half-open state.
- **Config compatibility** — unknown fields are ignored; known fields keep
  their overlaybd-snapshotter meaning (operator contract). A config produced
  by the overlaybd-snapshotter parses identically here.
- **Upper type validation is total** — every non-empty `upper` either
  selects a known layer type or is rejected with `EINVAL` at parse time; an
  unknown type can never reach assembly.

## Concurrency & Call Permissions

- **Config types are plain values** — `GlobalConfig`/`ImageConfig` and their
  members are data-only; parsing is synchronous, scheduler-free, and safe
  from any thread. No instance-level mutable state after parsing.
- **`open_image` is a cold-path Elio coroutine** — call it once per device
  during setup, on a thread with a running Elio scheduler (required by
  `LayerStore::open`, the DART probe, and the registry size probes).
  It is not re-entrant per config and not intended for the IO hot path.
- **The returned root follows the `BlobSource` contract** — concurrent
  `pread`s are safe (see `docs/source.md`); `pwrite`/`flush` on a writable
  root are called only from the ublk data plane after checking
  `OpenedImage.writable`.
- **Side effects** — `open_image` may: read the credential file, probe the
  DART proxy, create `upper.dir` and the upper file (writable mode), and —
  for every remote lower with a non-empty `dir` — create the layer
  directory if missing and open a `LayerStore` in it (staging pair
  `<dir>/.download.<nonce>` + `<dir>/.bitmap.<nonce>`, a dedicated writer
  `std::thread`, and an atomic rename to `<dir>/overlaybd.commit` on
  completion). The caller must keep the returned `OpenedImage` (and thus
  the sources and stores) alive for the device's lifetime, and must not
  destroy it while reads are in flight: a `LayerStore` touches members on
  resume of a suspended `pread`/`populate` (its lifetime contract, see
  `docs/source.md`).
- **No global state** — all per-image state (registry client, caches,
  downloaders) hangs off the returned `OpenedImage` ownership tree.

## Stability Contract

Breaking changes (require an ADR per `docs/adr/README.md`; config-schema
changes are T1):

- **`config.json` semantics** consumed from the overlaybd-snapshotter:
  `repoBlobUrl`, `lowers[]` (`digest`/`size`/`dir`/`file`), `resultFile`,
  `download` field names and the per-field override rule, and the
  ignore-unknown-fields rule. Operators' existing configs must keep working
  unchanged.
- **`overlaybd.json` semantics**: `credentialConfig` (mode=file), `p2pConfig`
  (`enable`/`address`, ADR-0005), `download` defaults, `logConfig.logLevel`.
- **The local probe contract** — the probe order (`lower.file`, then
  `overlaybd.commit`, `.commit`, `overlaybd.sealed` in the layer directory)
  is shared with the snapshotter's on-disk layout and with the install
  path every persistence mechanism uses (the retired Downloader, and the
  `LayerStore` whose completion rename lands exactly there); changing it
  strands previously downloaded blobs.
- **The remote addressing rule** — `repoBlobUrl + "/" + digest`.
- **The writable mode (ADR-0008)** — the `upper` object shape, the
  `lsmt`/`sparse` type set, the upper file names (`overlaybd.rw`,
  `overlaybd.sparse`), `EINVAL` on unknown types, and read-only behavior
  when `upper` is absent or empty.
- **Assembly failure semantics** — `open_image` throwing (never degrading)
  is relied on by the device process's startup protocol.

Compatible changes: newly honored config fields (additive, previously
ignored), new lower/upper layer types added alongside the existing ones,
default tuning (cache sizes, timeouts), and additional `OpenedImage`
metadata fields.

## Testing

Unit tests live in `tests/unit/test_image.cpp` (config parsing and local
assembly), `tests/unit/test_trace_replay.cpp` (trace replay and local
trace-layer assembly, ADR-0013), and `tests/unit/test_writable.cpp` (the
writable upper through `open_image`); integration coverage lives in
`tests/integration/test_integration.cpp` (remote assembly against a mock
registry). Run with `ctest --test-dir build --output-on-failure` (see
`docs/testing.md`).

- `image: global config parses overlaybd.json fields` — `credentialConfig`,
  `p2pConfig`, `download`, and `logConfig` parse into their fields; unset
  download fields keep their defaults; unknown sections (`cacheConfig`) are
  ignored.
- `image: per-image download overrides merge over global defaults` — an
  image `download` section overrides only the fields it sets and inherits
  the rest; `repoBlobUrl`/`lowers` parse; `digest_sha256_hex` strips the
  `sha256:` prefix and rejects other algorithms.
- `image: upper config parses; unknown type rejected` — a non-empty `upper`
  engages `writable()` with `lsmt` as the default type; absent/empty `upper`
  stays read-only; an unknown `upper.type` throws (ADR-0008).
- `image: assembly from local layer files reads merged content` — two local
  LSMT lowers assemble into a `MergedLsmt` whose reads return the top
  layer's content byte-exactly, with the correct layer count and virtual
  size.
- `image: assembly picks the ZFile view for compressed layers` — a
  ZFile-compressed lower is auto-detected, decompressed, and reads back the
  original content byte-exactly.
- `image: writable upper assembles and serves writes` — with an `lsmt`
  upper, `open_image` returns a writable root; a write lands in the upper
  and is visible to subsequent reads while unwritten ranges still read
  through to the lower; `flush` succeeds (ADR-0008 end-to-end at the image
  layer). The underlying writable-layer mechanics are pinned separately by
  `format: merged writable falls through and copy-on-writes` (see
  `docs/format.md`).
- `integration: registry pipeline serves a zfile-compressed image` — a
  remote image config (no local files, no layer dir) assembles through the
  mock registry and serves the full image byte-exactly, with the expected
  layer count and virtual size; the missing `dir` exercises the no-dir
  exception path (legacy in-memory `ChunkCache`).
- `integration: image assembly serves remote reads through the layer store` —
  a remote lower with a configured `dir` is served through the
  `RegistrySource → LayerStore` chain byte-exactly, and the read-through
  path leaves persistence state (a staging pair or `overlaybd.commit`) in
  the layer dir (ADR-0011).
- `integration: layer store restart serves warmed extents without remote reads` —
  after a partially-warmed first open (a prefix read, staging pair drained
  to disk), a second `open_image` serves the same reads entirely from the
  resumed pair: the mock registry's remote-read counter does not move.
- `integration: completed layer store commit binds read-only without remote reads` —
  a single-extent layer driven to completion renames to
  `overlaybd.commit`; a reopen binds the commit marker via the local probe
  and serves byte-exact reads with zero remote data reads.
- `integration: enabled-but-unreachable DART falls back to direct reads` —
  with `p2pConfig` enabled against a dead address, `open_image` still opens
  and serves the full image directly from the registry (ADR-0005).
- `image: trace replay populates traced extents in recorded order` —
  records interleaving two lowers issue `populate` on the right target in
  the trace's exact order, with stats accounting (ADR-0013).
- `image: trace replay skips unknown ops, layers and bad records` — 'W'
  and arbitrary op bytes, unknown/null layer indexes, zero and > 1 MiB
  counts, and negative offsets are skipped silently; a failing populate
  and a malformed blob degrade to "no prefetch", never an error.
- `image: trace replay enforces record, byte and time budgets` — the
  `max_records` / `max_bytes` / `max_wall_time` bounds each stop replay
  early with `budget_exhausted` set.
- `image: local trace layer is set aside and replayed at open` — an
  `accelerationLayer: true` config with a local `<dir>/trace` blob opens
  with the trace layer excluded from the merge (layer count, virtual
  size, byte-exact content) and the trace fully replayed.
- `image: garbage trace layer never fails assembly` — a garbage trace
  blob still yields a working device (opportunistic replay).
- `integration: trace layer replays warm-up through the layer store` —
  end to end against the multi-blob mock: a tar-wrapped trace layer is
  recognized, set aside, and its records warm the data layer through
  `TarOffsetSource::populate` → `LayerStore::populate` — the tar-header
  translation is pinned by attributing fetched extents on the mock (an
  extent only the replay can reach), and the device serves the data
  layer byte-exactly (ADR-0013 acceptance).

Fixture data is generated in-test (`obd-mkimage`-equivalent writers from
`src/format`, deterministic patterned bytes); the mock registry serves a
single Range-capable blob. No external golden files.

## Limitations & TODO

- **`download.enable` is parsed but currently inert** — the `download`
  sections keep parsing (operator contract), but the SwitchSource/Downloader
  background download was retired from assembly in ADR-0011 part 2:
  locality already grows read-through via the `LayerStore`. Background
  whole-blob fill returns in part 3 as LayerStore fill (see
  `docs/config.md`).
- **In-memory caching only on the no-dir exception path** — for
  dir-configured remote layers the kernel page cache over the
  `LayerStore` staging/commit file is the L1 (ADR-0011 rejected a
  user-space LRU on top); a remote layer without `lower.dir` keeps the
  legacy in-memory `ChunkCache` (restart-cold) until part 3.
- **Honored config surface is a subset** — `cacheConfig`, `ioEngine`,
  `prefetch`, non-file `credentialConfig` modes, and `resultFile` handling
  are parsed-as-ignored / informational in v0.1 (see `docs/config.md` for
  the full compatibility matrix). The trace layer IS recognized and
  replayed (ADR-0013, proposed; see Concepts → "The trace layer"), with
  these gaps: trace **recording** is not implemented; the
  dynamic-prefetch file-list fallback is rejected by design; the tar
  member name (`trace`) is not checked — recognition is the config flag
  plus the blob magic; remote lowers without `dir` are not warmed
  (populate is a no-op on the legacy in-memory `ChunkCache`); and replay
  runs at normal priority until the B-phase admission funnel exists.
- **`lower.size` is not cross-checked** against the probed/local blob size;
  the authoritative size comes from the source at open time.
- **Writable uppers are per-device and not sealed automatically** — the
  ADR-0008 mode persists writes in the upper file across reopen, but
  committing/sealing an upper into a new lower is an explicit, offline
  operation (the supervisor's `commit` command, ADR-0014); TurboOCI and
  registry write-back remain out of scope (ADR-0007).
- **Sparse uppers depend on filesystem fiemap support** for extent recovery
  after reopen (see `docs/format.md`); exotic filesystems without
  `SEEK_HOLE`/fiemap semantics are unsupported for `upper.type = "sparse"`.
- **One DART decision per open** — reachability is probed once at assembly;
  a proxy that appears later is not adopted until reopen, and a proxy that
  dies later degrades to per-request errors rather than re-fallback.
- **Credentials are loaded once** — rotating the credential file requires a
  device reopen.
- **Assembly is sequential over lowers** — layers are probed and opened one
  after another; very high layer counts pay N sequential registry size
  probes at open (parallel open is a candidate optimization).
- **`OpenedImage` ownership is monolithic** — there is no API to add/remove
  a layer or re-resolve a single lower without reassembling the image.
