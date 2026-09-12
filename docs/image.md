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
(`"sha256:<hex>"`); the hex payload doubles as the integrity-check value
for the `LayerStore` completion verification.

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
   complete and sha256-verified. The `download` section drives the
   store's background fill (`enable`/`delay`/`delayExtra`/`maxMBps`/
   `blockSize`) and the completion-verify retry bound (`tryCnt`). Two
   degrade paths keep a remote layer bootable without persistence
   (ADR-0016): an **empty** `lower.dir` is served remote-only (plain
   `RegistrySource`, with a warning — the snapshotter always sets `dir`,
   so this is the compatibility path), and a `lower.dir` the store
   cannot open (unwritable, full, blocked by a non-directory) degrades
   to the same remote-only chain with a warning instead of failing
   assembly. All remote reads of the whole image pass one per-device
   read admission funnel (ADR-0012): the `LayerStore`s take it through
   `Config::funnel` (misses = OnDemand, populate = Prefetch, background
   fill = Fill), and the remote-only chains are wrapped in an
   `AdmissionSource` (pread = OnDemand) — see `docs/source.md`.
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

### Structural warm-up (ADR-0012's cold-start floor)

Before any trace replay, assembly warms a **bounded window at the head
and the tail of every data lower** — no trace required. The windows are
computed from the layer blob's byte layout alone (head = `[0, head_kb)`,
tail = `[size-tail_kb, size)`; sizes from the `prefetch` section,
`docs/config.md`) — deliberately **no format parsing** (the
prioritized-files mode stays rejected, ADR-0013).

**Window semantics — the tail of the tar VIEW, not of the blob.** The
windows are computed in the stored-blob byte space of the
`TarOffsetSource` view — the same space trace records address. The
structures the warm-up is for (ADR-0012: "index regions, tar headers,
filesystem metadata neighborhoods") sit there:

- the **tar header** is at the underlying blob's head, just *below* the
  view (base offset 512/1536) — inside the same first extents the head
  window warms, and already fetched by the `TarOffsetSource::open` probe
  during assembly;
- the **ZFile header / LSMT header** are at the payload (= view) head —
  the head window also covers the first data blocks, the filesystem
  metadata neighborhood of uncompressed layers;
- the **indexes** — the ZFile jump table + trailer and the LSMT index —
  live at the tail of the *payload* (ZFile layout: `... | index |
  Trailer (512B)`; LSMT keeps its index at the file tail). The bytes
  past the payload end inside the blob are tar zero-padding, which holds
  no data — so the correct tail window is the view's tail, which
  `TarOffsetSource::populate` translates through the tar base offset
  onto exactly the extents carrying the indexes.

A blob smaller than head+tail is warmed whole: the clamped windows merge
into `[0, size)` so no byte is populated twice; a 0 window size disables
that side; an empty blob gets no windows.

**Bring-up position and class.** Warm-up runs awaited inline during
bring-up, **after the layer chains are built and before the trace blob
load and replay** (the floor first; the trace path refines it — and a
slow trace layer's fetch time sits outside both warm-up budgets, so the
floor must not wait on it), bounded by a 30 s wall-time budget
(the same pattern and default as replay). The budget means **30 s of
actual warm-up work**: windows are populated in 64 KiB slices (one
LayerStore extent — the granularity the LayerStore path already
suspends at between remote fetches, so slicing adds no remote traffic),
the budget is re-checked between slices, and a window that cannot
finish within the budget is **abandoned mid-window** (`windows_skipped`)
rather than awaited to its end. A slice whose funnel admission stays
closed past the per-extent admit bound (2 s,
`LayerStore::Config::populate_admit_timeout`, issue #35) skips its
window outright the same way — blocked or slow windows are skipped,
never awaited, overshoot bounded by one admit bound plus one extent
fetch per in-flight extent (a view-space slice spans two extents at an
unaligned tar base, so the worst case is two of each), and every
skipped extent is simply served on demand later.
(The same admit bound also covers trace replay's populates, which share
the LayerStore populate path.) Combined with the trace blob load and replay
budgets below, structural warm-up plus trace load plus trace replay add at
most **~90 s** to the worst-case device bring-up;
detaching both off the
bring-up path — now safe, since the funnel yields to on-demand reads —
is the documented follow-up. Every populate rides
the device's admission funnel as the **Prefetch scavenger class**
(populate's wiring, ADR-0012), so it yields to on-demand reads
automatically, and dedup against the open-time format probes, replay,
and background fill is automatic via the LayerStore in-flight map and
present flags — an already-warm extent is joined or skipped, never
re-fetched. Warm-up is **opportunistic**: a failing populate is logged
and skipped, never a bring-up error, and a bypassed/degraded LayerStore
turns populate into a no-op. `prefetch.enable` is the master gate for
both warm-up kinds.

### The trace layer (ADR-0013)

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
recorded order. The trace blob load and the replay are **awaited inline
during device bring-up, after the structural warm-up** (ADR-0012's floor
runs first — a slow or unhealthy trace layer must not delay it). The
blob load gets its own 30 s wall-clock budget: if opening or reading the
trace layer is still pending at the deadline, `open_image` drops the late
result, skips replay, and returns a fully functional device whose later
reads are served on demand. The underlying fetch may finish in the
background and its result is discarded; this avoids relying on hard
cancellation in registry/funnel paths that deliberately do not have it.
Replay is bounded by the separate 30 s wall-time budget below, and every
populate it issues passes the device's read admission funnel (ADR-0012) as the
**Prefetch scavenger class**, outranking background fill. Detaching
replay off the bring-up path — now safe, since the funnel yields to
on-demand reads — is a documented follow-up. The global `prefetch.enable` switch
(`docs/config.md`) gates the trace load/replay. Skip rules follow
upstream replay parity
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
| `max_bytes` | 1 GiB | Caps requested bytes handed to `populate()`, charged before each issued call even when the warm-up fails; a record that would exceed the remaining allowance is not clamped or submitted. This bounds requested warm-up, not exact network bytes (extent rounding, cache hits, partial progress, and retries are source-layer details). |
| `max_wall_time` | 30 s | Replay is awaited during bring-up; this caps the worst-case bring-up delay. |
| `max_record_count` | 1 MiB | The upstream replay buffer cap (trace-format.md §8/§10); larger records come only from non-conforming writers and are skipped, not clamped. |

The trace blob itself is read with a 64 MiB cap (a conforming trace is
~1.5 MiB at the record bound).

**Recording.** The record path of ADR-0013 lives in
`src/image/trace_record.hpp` and is driven by the supervisor's
`trace_start`/`trace_stop` commands (docs/supervisor.md); the runbook is
docs/operations.md. Design:

- **Tap placement.** Assembly wraps each remote lower's `RegistrySource`
  in a `TraceRecordSource` — BEFORE the `LayerStore` (and before the
  no-dir `AdmissionSource` wrapper). The `LayerStore` and
  `AdmissionSource` pass the ADR-0012 read class down to the tap, so a
  recording appends only fully satisfied guest OnDemand reads. Local
  cache hits never reach the tap, and Prefetch/Fill traffic from
  structural warm-up, trace replay, or background fill is filtered out.
  The tap is the narrowest common point of both remote chain shapes
  (dir-cached and no-dir); the `layer_index` it stamps is the
  open_image loop index threaded in at construction (data lowers only —
  local lowers occupy index slots but carry no remote source, hence no
  tap).
- **Payload offsets.** Records carry PAYLOAD offsets (the tar wrapper is
  translated out: the tap subtracts the `TarOffsetSource` base once
  assembly probes it, and an extent fetch spanning the 512-byte tar
  header clamps to its payload overlap) — the same byte space replay
  consumes, so a recorded blob feeds the replay path verbatim.
- **Only fully-satisfied OnDemand reads record** (a short or failed
  pread, or a Prefetch/Fill read, appends nothing): a partial read would
  record bytes the device never received, while synthetic scavenger
  traffic is not guest demand.
- **Hot path.** Recording disabled costs one atomic load per remote
  read; enabled, an append is a mutex + bounded-deque push (no IO, no
  allocation growth, no syscall) — nanoseconds against the millisecond
  scale of the remote fetch it follows. The buffer is bounded
  (65536 records ≈ 1.5 MiB serialized, matching the replay
  `max_records` bound): overflow DROPS the incoming record chunk and
  counts it (`dropped`, surfaced in the stop reply and status field).
  A trace with drops is still valid and replayable — it simply covers
  fewer extents.
- **Adjacent coalescing.** An append that continues the tail record
  (same layer, `offset == tail.offset + tail.count`) merges into it —
  sequential extent fetches, the common case, collapse to one record —
  while respecting the 1 MiB conforming count cap (an oversized read is
  pre-split into ≤ 1 MiB records at append time, so the queued stream
  always satisfies the writer contract). Coalescing never reorders:
  the serialized stream preserves record order exactly.
- **Finalize.** `stop()` (explicit stop, duration expiry, or device
  shutdown) drains the queue through the codec's
  `format::trace::TraceWriter` — 24×N framing, raw-chaining CRC-32C,
  header checksum rewritten on `finalize()` — then writes + fsyncs the
  blob and reports `{path, sha256, size, records, dropped}`. The
  duration timer is a joinable recorder task; an external,
  non-reentrant `stop()` cancels a sleeping timer and waits until the
  timer coroutine has destroyed its frame, including expiry callbacks
  that run after an expiry-owned finalize. Device shutdown therefore
  calls `stop()` for any recorder, even when `recording()` is already
  false. `stop()` is idempotent: a stop racing the expiry waits for the
  in-flight finalize and returns its stats. A stop task created on the
  expiry callback's execution thread captures and returns that expiry
  result without trying to join the callback's own timer. Calls created
  by other threads while the callback is still running remain external
  calls: they wait for the timer drain instead of using the callback's
  reentrant fast path.

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

ADR-0020 preserves the writable-layer contracts established by ADR-0008
and adds native TurboOCI lower layers.

### The blank (raw) device mode (ADR-0014)

A blank device has **no image at all** — no config, no lowers, no
registry. `obd::image::open_blank_device` (below) assembles the same
stack shape as the writable mode, but its "lower" is a **sealed empty
LSMT layer** (docs/format.md, `create_empty_lsmt_layer`): virtual size =
the requested size, no segments, no data. The writable LSMT-RW upper sits
on top, also sized to the requested size, so:

- the device reads as a **zeroed block device from birth** — never-written
  ranges return zeroes through the ordinary merge path, not through any
  new source type (the synthetic-zero-source alternative was rejected in
  ADR-0014);
- **writes land in the upper from day one**; `commit` (ADR-0014) seals the
  upper exactly like an image-born writable device;
- the empty zero base is byte-deterministic (content-derived uuid, a pure
  function of the size), so identical blank devices start from identical
  base bytes.

The workspace layout (`overlaybd.zero` + `overlaybd.rw` under a per-device
directory) is chosen by the caller of `open_blank_device`
(`BlankDeviceSpec::dir`); the supervisor passes
`<blank_dir>/<id>/` (docs/supervisor.md).

## Public API

All types live in namespace `obd::image`. Parsing functions are synchronous
(plain file IO + JSON) and scheduler-free; `open_image` is an Elio coroutine.

### `config.hpp` — GlobalConfig

```cpp
struct GlobalConfig {
    std::string credential_file;
    bool p2p_enable = false;
    std::string p2p_address;
    DownloadConfig download;
    bool prefetch_enable = true;
    uint32_t prefetch_head_kb = 1024;
    uint32_t prefetch_tail_kb = 1024;
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
  `src/image/config.hpp::DownloadConfig`); per-image `download`
  sections override these field by field. The knobs drive the
  `LayerStore` background fill and completion-verify bound (ADR-0011;
  see `docs/config.md`).
- `src/image/config.hpp::log_level` — `logConfig.logLevel`:
  0=debug, 1=info (default), 2=warn, 3=error.
- `src/image/config.hpp::prefetch_enable` — `prefetch.enable` (default
  true): the master switch for BOTH bring-up warm-up kinds — the
  structural head/tail prefetch (ADR-0012's cold-start floor) and trace
  replay (ADR-0013).
- `src/image/config.hpp::prefetch_head_kb` /
  `src/image/config.hpp::prefetch_tail_kb` — `prefetch.head_kb` /
  `prefetch.tail_kb` (default 1024 each; 0 disables that side): the
  structural warm-up window sizes (see Concepts → "Structural warm-up").
  The funnel's AIMD window is not operator-configured, so the section
  exposes nothing else — see `docs/config.md`.
- `from_file(path)` — reads and parses the file. Throws `obd::error` on IO
  failure, `src/common/errors.hpp::format_error` on malformed JSON.
- `src/image/config.hpp::from_json_text` — same, from an in-memory string
  (tests, the supervisor's config channel).
- `cacheConfig` and `ioEngine` sections are intentionally not honored in
  v0.1; unknown fields are ignored per the operator contract.

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
    DownloadConfig download;   // merged over the global defaults
    bool acceleration_layer = false;   // uppermost lower is the trace layer
    UpperConfig upper;
    bool writable() const noexcept;

    static std::string digest_sha256_hex(const std::string& digest);
    static ImageConfig from_file(const std::string& path,
                                 const DownloadConfig& defaults);
    static ImageConfig from_json_text(const std::string& text,
                                      const DownloadConfig& defaults);
};
```

`src/image/config.hpp::LowerConfig` — one layer. `src/image/config.hpp::digest`
is the OCI digest; `size` is the blob size as recorded by the snapshotter
(informational — the authoritative size comes from the local file or the
registry probe); `dir` is the per-layer directory used for the local probe
and the `LayerStore` persistence state (staging pair and
`overlaybd.commit`; created if missing; empty disables persistence for a
remote layer, which is then served remote-only); `file`
names a local blob file (empty = the layer is remote).

`src/image/config.hpp::UpperConfig` — the writable upper (ADR-0008).
`src/image/config.hpp::dir` holds the layer file; `src/image/config.hpp::type`
selects `overlaybd.rw` (`"lsmt"`, default) or `overlaybd.sparse`
(`"sparse"`).

`src/image/config.hpp::ImageConfig::acceleration_layer` — from the
top-level `accelerationLayer` boolean (ADR-0013): marks the
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

### `image_file.hpp` — OpenedImage, open_image, blank device

```cpp
struct OpenedImage {
    source::BlobSourcePtr root;   // MergedLsmt or MergedWritable
    uint64_t virtual_size = 0;
    size_t layer_count = 0;       // data layers (trace layer excluded)
    bool writable = false;
    std::string upper_path;
    TraceReplayStats trace;       // ADR-0013 replay outcome
    StructuralWarmupStats warmup; // ADR-0012 structural warm-up outcome
    std::vector<source::LayerStore*> layer_stores;  // non-owning
    source::AdmissionFunnelPtr funnel;  // the device's ADR-0012 funnel
    TraceRecorderPtr recorder;    // ADR-0013 record path (one per image)
};

elio::coro::task<OpenedImage> open_image(const ImageConfig& cfg,
                                         const GlobalConfig& global);

struct BlankDeviceSpec {   // ADR-0014 blank (raw) device
    uint64_t size = 0;     // bytes; positive, sector aligned
    std::string dir;       // per-device workspace (created if missing)
};
elio::coro::task<OpenedImage> open_blank_device(const BlankDeviceSpec& spec);

elio::coro::task<void> park_image_fills(const OpenedImage& opened);
```

`open_blank_device` in `src/image/image_file.hpp` — assembles the blank (raw)
device view: creates `<dir>/overlaybd.zero` (a sealed empty LSMT zero base
via `create_empty_lsmt_layer` in `src/format/lsmt_rw.hpp`), opens it as an
`LsmtLayer`, creates `<dir>/overlaybd.rw` (an `LsmtRwLayer` sized to
`size`), and merges them into a `MergedWritable` — `writable = true`,
`virtual_size = size`, `upper_path` set, idle recorder for shape parity.
Throws `obd::error` on an invalid spec (size not a positive multiple of
512) or IO failure.

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
- `warmup` — the
  `src/image/structural_warmup.hpp::StructuralWarmupStats` outcome of the
  structural head/tail warm-up (ADR-0012): layers/windows warmed, bytes
  requested, windows failed, and whether the wall-time budget stopped the
  pass early. All zero when `prefetch.enable` is false.
- `src/image/image_file.hpp::layer_stores` — non-owning handles to every
  `LayerStore` in the chain (owned by `root`), for lifecycle operations.
- `funnel` — the device's read admission funnel (ADR-0012), created at
  open and shared by every lower's `LayerStore` and by the remote-only
  source wrappers (no-dir lowers, ADR-0016 degrade, the trace blob
  fetch): all remote reads of the device compete at this single gate.
  The handle is for observability and tests; the chains keep it alive.
- `src/image/image_file.hpp::recorder` — the image's `TraceRecorder`
  (ADR-0013 record path): the supervisor-driven trace commands start and
  stop it (docs/supervisor.md); obd-device finalizes it during shutdown.
  The taps in the chain share it; idle, it costs one atomic load per
  remote read.
- `src/image/image_file.hpp::park_image_fills` — stops every background
  fill in the chain and joins each fill task before returning. The timeout
  is a warning threshold, not permission to destroy a store still owned by
  a fill frame. Must be called before `root` is destroyed on any path other
  than process exit: destroying a store with a fill in flight is a
  use-after-free (the fill coroutine touches members on resume).
  `open_image` uses the same parking path before unwinding a partially
  assembled chain, and the device server uses it during normal shutdown and
  post-open boot-failure cleanup.

`open_image(cfg, global)` — assembles the merged view. Cold path; runs on
the calling Elio coroutine during device setup.

Behavior, in order:

1. Rejects an empty `lowers` list and invalid `download.tryCnt` with
   `obd::error(EINVAL)` — these are configuration errors, not an empty
   device or a persistence-environment failure (ADR-0018).
2. Loads the credential file when configured; an unreadable/missing file is
   **not fatal** — it logs a warning and pulls anonymously.
3. Resolves DART: when `p2pConfig` is enabled and the address parses, the
   proxy is probed (`dart_proxy_reachable`); only a reachable proxy sets the
   client's accelerate prefix. Malformed addresses and unreachable proxies
   log a warning and fall back to direct registry reads (ADR-0005). One
   `RegistryClient` is shared by all layers of the image.
4. When `accelerationLayer` is set: requires at least one data lower
   beneath the trace layer (`obd::error(EINVAL)` otherwise) and sets the
   uppermost lower aside. Recognition is structural and always applies;
   the trace blob itself is **not** loaded yet (see step 7).
5. Builds each lower per the chain in Concepts. A lower with **no local
   file and an empty `repoBlobUrl`** fails with `obd::error(EINVAL)` —
   there is nowhere to read it from.
6. Runs the structural warm-up via
   `src/image/structural_warmup.hpp::warmup_structural` (when
   `prefetch.enable`): head/tail windows on the data lowers'
   stored-blob-level sources, sequentially awaited under a wall-time
   budget, Prefetch scavenger class. Never fails assembly.
7. With the floor warmed, loads the acceleration layer's trace blob
   best-effort under its 30 s wall-clock budget (when `accelerationLayer`
   is set and `prefetch.enable`) — every load failure or timeout only
   disables prefetch (ADR-0013) — and replays it via
   `src/image/trace_replay.hpp::replay_trace`: `populate()` on the data
   lowers' stored-blob-level sources, in recorded order, sequentially
   awaited, bounded by `TraceReplayOptions`. Never fails assembly.
8. Read-only: merges with `MergedLsmt` and returns. Writable
   (`cfg.writable()`): creates `upper.dir` if needed, opens/creates the
   upper (`LsmtRwLayer::create` for `lsmt`, `SparseRwLayer::open` for
   `sparse`) sized to the maximum lower virtual size, merges with
   `MergedWritable`, and returns with `writable = true`.

Error behavior: **all-or-nothing** — any structural failure (config,
credentials parse aside, network probe, corrupt layer, unsupported
format) throws `obd::error` /
`src/common/errors.hpp::format_error`; a device that cannot assemble must
not come up half-broken. `open_image` never returns a partially assembled
image. The one sanctioned degrade: a remote layer whose `LayerStore`
cannot open (unwritable/full/unusable `lower.dir`) falls back to
remote-only reads with a warning — persistence is best-effort and never
gates boot (ADR-0016). It requires a running Elio scheduler (the DART
probe, the registry size probes, and `LayerStore::open`'s blocking setup
via `elio::spawn_blocking`).

### `structural_warmup.hpp` — StructuralWarmupOptions, StructuralWarmupStats, structural_windows, warmup_structural

```cpp
struct WarmWindow { uint64_t offset; uint64_t len; };

std::vector<WarmWindow> structural_windows(
    uint64_t blob_size, uint64_t head_bytes, uint64_t tail_bytes) noexcept;

struct StructuralWarmupOptions {
    uint64_t head_bytes = uint64_t{1} << 20;       // 1 MiB; 0 disables
    uint64_t tail_bytes = uint64_t{1} << 20;       // 1 MiB; 0 disables
    std::chrono::milliseconds max_wall_time{30000};
};

struct StructuralWarmupStats {
    size_t layers_total = 0;
    size_t layers_warmed = 0;
    size_t windows_populated = 0;
    size_t windows_failed = 0;
    uint64_t bytes_warmed = 0;
    bool budget_exhausted = false;
};

elio::coro::task<StructuralWarmupStats> warmup_structural(
    const std::vector<source::BlobSource*>& warm_targets,
    const StructuralWarmupOptions& opts = {});
```

`src/image/structural_warmup.hpp::structural_windows` — the pure window
computation: head `[0, head_bytes)` and tail `[size-tail_bytes, size)`,
each clamped to the blob (0 disables a side), merged into `[0, size)`
when the clamped windows would cover the whole blob (no byte populated
twice), empty for an empty blob.

`warmup_structural_grouped` accepts `StructuralWarmupTarget` entries associating each stored blob with
its logical `layer_index`. TurboOCI metadata and original target both receive
windows, but `layers_total` and `layers_warmed` count their lower once; a
successful slice from either blob marks that lower warmed. Window and byte
counters still count actual work across both sources.

`src/image/structural_warmup.hpp::warmup_structural` — populates the
windows of every target (a nullptr entry is skipped): head before tail per layer, each `populate()`
sequentially awaited. **Never throws**: a failed (or throwing) populate
is logged, counted in `windows_failed`, and warm-up moves on; processing
stops early when the wall-time budget is spent (`budget_exhausted`
set). The window semantics (why the tail is the tar *view's* tail) and
the bring-up position are in Concepts → "Structural warm-up".

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
    uint64_t bytes_requested = 0;
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
silently (upstream parity); a failed populate is logged and skipped after
charging `bytes_requested`. `max_bytes` is record-atomic: replay stops
before submitting a record that would exceed the remaining requested-byte
allowance rather than clamping it. Processing stops early on any
`TraceReplayOptions` bound (`budget_exhausted` set). The bounds and their
rationale are tabulated in Concepts → "The trace layer".

### `trace_record.hpp` — TraceRecorder, TraceRecordSource

```cpp
class TraceRecorder {  // one per opened image (OpenedImage::recorder)
    static constexpr size_t kMaxPendingRecords = 65536;
    static constexpr uint32_t kMinDurationSec = 1;
    static constexpr uint32_t kMaxDurationSec = 3600;
    struct FinalizeResult {
        bool ok = false;
        std::string error, path, sha256, reason;
        uint64_t size = 0, records = 0, dropped = 0;
    };
    bool recording() const noexcept;  // hot-path gate (one atomic load)
    // bounded append, drop-counted, noexcept:
    void record(uint32_t layer_index, uint64_t offset,
                uint64_t count) noexcept;
    elio::coro::task<bool> start(
        std::string path, uint32_t duration_sec,
        std::function<void(const FinalizeResult&)> on_expire,
        std::string& error);
    elio::coro::task<FinalizeResult> stop(std::string reason);
    std::optional<FinalizeResult> last_result() const;
};

class TraceRecordSource : public source::BlobSource,
                          public source::ReadClassAwareSource {
    // pread: OnDemand pass-through for direct callers.
    // pread_with_class: records only fully-satisfied OnDemand reads
    // with the base subtracted; Prefetch/Fill pass through unrecorded.
    // set_base() threads the tar payload offset.
};
```

`src/image/trace_record.hpp::TraceRecorder` — the ADR-0013 record path's
writer side: a bounded in-memory queue (drop-counted overflow, adjacent
coalescing within the 1 MiB count cap, > 1 MiB reads pre-split) drained
at `stop()` through the codec's conforming writer; the duration timer is
joinable and device-side (expiry finalizes without any client call, while
external shutdown/explicit stop is the timer completion barrier). Re-entrant
`start()` from the expiry callback is rejected; schedule any follow-up
window after the callback returns.
`start()` validates the duration bound and the absolute output path and
opens the output file fail-fast. **Never throws from the read path** —
`record()` is `noexcept`; an append failure drops and counts. The design
(tap placement, payload offsets, hot-path cost, lifetime rule) is in
Concepts → "The trace layer → Recording".

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
  image or throws; there is no half-open state. Loss of *persistence* is
  not an assembly failure: an unusable layer dir degrades that layer to
  remote-only reads with a warning (ADR-0016). The degrade covers
  environment failures only — a malformed lower digest is a structural
  config error, validated before any registry I/O, and throws
  `obd::error(EINVAL)`.
- **Config compatibility** — unknown fields are ignored; known fields keep
  their overlaybd-snapshotter meaning (operator contract). A config produced
  by the overlaybd-snapshotter parses identically here.
- **Upper type validation is total** — every non-empty `upper` either
  selects a known layer type or is rejected with `EINVAL` at parse time; an
  unknown type can never reach assembly.
- **Blank devices are zero- and writable-from-birth (ADR-0014)** —
  `open_blank_device` returns a `MergedWritable` (never a read-only root):
  its sealed empty LSMT zero base makes every never-written range read
  zero through the ordinary merge path, and writes land in the LSMT-RW
  upper from day one; the workspace layout is
  `<dir>/overlaybd.zero` + `<dir>/overlaybd.rw`.

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
  destroy it while reads are in flight or a background fill is unparked:
  a `LayerStore` touches members on
  resume of a suspended `pread`/`populate`/fill step (its lifetime
  contract, see `docs/source.md`). `park_image_fills` is the sanctioned
  pre-destroy step; `open_image` calls it before unwinding a partial
  chain, and the device server calls it during shutdown and post-open
  boot-failure cleanup.
- **No global state** — all per-image state (registry client, layer
  stores) hangs off the returned `OpenedImage` ownership tree.

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
  path every persistence mechanism uses (the
  `LayerStore`, whose completion rename lands exactly there); changing it
  strands previously downloaded blobs.
- **The remote addressing rule** — `repoBlobUrl + "/" + digest`.
- **The writable mode (ADR-0008)** — the `upper` object shape, the
  `lsmt`/`sparse` type set, the upper file names (`overlaybd.rw`,
  `overlaybd.sparse`), `EINVAL` on unknown types, and read-only behavior
  when `upper` is absent or empty.
- **Assembly failure semantics** — `open_image` throwing on structural
  failure (never half-assembling) is relied on by the device process's
  startup protocol. The bounded exception is ADR-0016: layer persistence
  is best-effort, so an unusable `lower.dir` degrades the layer to
  remote-only reads with a warning instead of throwing.

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
- `image: invalid download tryCnt is rejected at config boundaries` —
  zero, negative, and out-of-range JSON values fail before conversion, and
  programmatic zero fails at assembly entry before persistence fallback
  can swallow it.
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
- `image: blank device assembles a zeroed writable upper` — ADR-0014
  mode-2 assembly without any config: `open_blank_device` returns a
  writable root of the requested size whose on-disk zero base
  (`overlaybd.zero`) re-opens as a sealed empty LSMT layer; a fresh blank
  reads zeroes across its whole range, and a patch write lands in the
  upper (`overlaybd.rw`) and reads back while the untouched regions stay
  zero.
- `integration: registry pipeline serves a zfile-compressed image` — a
  remote image config (no local files, no layer dir) assembles through the
  mock registry and serves the full image byte-exactly, with the expected
  layer count and virtual size; the missing `dir` exercises the no-dir
  remote-only path (ADR-0016).
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
  and serves byte-exact reads with zero remote data reads while sweeping stale
  `.download.*`/`.bitmap.*` files beside the committed layer.
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
- Trace recording (ADR-0013; `tests/unit/test_trace_record.cpp`,
  daemon-level coverage in `tests/integration/test_trace_record.cpp`,
  listed in docs/supervisor.md):
  `image: trace recording round-trips through the codec reader` — a
  recorded blob parses with the C2 reader (checksum rewrite included)
  and its stats match the file;
  `image: trace recording coalesces adjacent records and preserves order`
  — same-layer continuations merge within the 1 MiB cap, disjoint reads
  stay ordered;
  `image: trace recording splits reads beyond the conforming count cap`
  — an oversized read lands as consecutive ≤ 1 MiB records;
  `image: trace recording drops and counts records when the buffer fills`
  — overflow sheds whole chunks, `dropped` is reported, and the blob
  stays valid;
  `image: trace recording skips partial and failed reads` — short reads
  and read errors record nothing;
  `image: trace recording is pass-through and error-clean when idle` —
  an idle tap reads and reports errors exactly like the wrapped source;
  `image: trace recording stop is idempotent and reports expiry stats` —
  the device-side timer finalizes with no client call and a late stop
  returns the same stats;
  `image: trace recording stop drains an awakened duration timer` —
  explicit shutdown stop waits for a timer that already woke before it
  touched recorder state;
  `image: trace recording expiry callback is skipped when explicit stop wins` —
  a timer that already captured the expiry callback but loses finalization
  to an explicit shutdown stop returns that shutdown result without
  emitting an expiry callback;
  `image: trace recording shutdown joins expiry finalization` —
  shutdown stop joins an expiry-owned finalize while `recording()` is
  already false;
  `image: trace recording late stop waits for expiry callback completion` —
  a cached late stop waits for the timer's callback tail to finish;
  `image: trace recording timer losing stop race skips expiry callback` —
  if an external stop wins after the timer copied its callback but before
  it owns finalization, no expiry callback is emitted for the external
  stop result;
  `image: trace recording expiry callback stop never joins itself` —
  a stop task created re-entrantly from the expiry callback returns the
  captured expiry result without joining its own timer or stopping a
  restarted recording, and a callback-created start is rejected;
  `image: trace recording external stop during expiry callback drains timer` —
  a stop task created by another thread while the expiry callback is
  running still waits for the timer frame to finish before returning;
  `image: trace recording external start during expiry callback drains timer` —
  a start task created by another thread while the expiry callback is
  running waits for the stale timer drain instead of being rejected as
  callback-reentrant;
  `image: trace recording stale stop never drains a restarted timer` —
  a late stop waiting on an old timer drain stays bound to that old
  timer after a new recording starts;
  `image: concurrent trace stops join the winning finalize` — an
  expiry stop and explicit stop are held after both observe Recording;
  the loser joins and returns the winner's result;
  `image: trace recording captures only remote fetches through the layer store`
  — local hits record nothing, misses record exactly the fetched
  extents;
  `image: trace recording filters fill and prefetch layer-store reads`
  — with background fill active and a Prefetch populate issued during
  the recording window, only the guest OnDemand extent is recorded;
  `image: trace recording translates offsets out of the tar wrapper` —
  records address payload space, with header-spanning fetches clamped
  to their payload overlap;
  `image: trace recording finalizes an empty window to a valid header-only blob`
  — a stop before any record still runs the checksum rewrite;
  `image: trace recording restarts cleanly after a stop` — a fresh
  window resets the queue and counters;
  `image: trace recording rejects start while a finalize is in flight`
  — the state-machine guard against queue wipe/stat corruption, pinned
  with the test-only finalize hook;
  `image: trace recording rejected start never truncates existing files`
  — the gate runs before the output open, so a rejected start cannot
  destroy a previous valid blob or the active window;
  `image: trace recording start race truncates the output exactly once`
  — only the state winner truncates (under the lock); the loser of a
  same-path start race touches no filesystem;
  `image: trace recording drops out-of-range offsets instead of corrupting`
  — an out-of-int64 range is drop-counted, never a negative blob
  offset.
- `image: local trace layer is set aside and replayed at open` — an
  `accelerationLayer: true` config with a local `<dir>/trace` blob opens
  with the trace layer excluded from the merge (layer count, virtual
  size, byte-exact content) and the trace fully replayed.
- `image: prefetch enable false skips trace replay but keeps recognition` —
  `prefetch.enable = false` skips the trace load/replay (stats zero)
  while the acceleration layer is still set aside from the merge
  (ADR-0012).
- `image: garbage trace layer never fails assembly` — a garbage trace
  blob still yields a working device (opportunistic replay).
- `image: writable image with a trace layer assembles and replays` — a
  writable (`upper`) image with `accelerationLayer` still sets the trace
  layer aside, replays it, and serves copy-on-write reads/writes.
- `image: structural warm-up windows clamp and merge on small blobs` —
  the pure window computation: disjoint head/tail windows at the exact
  edges, clamping per side, a single merged full window for any blob
  smaller than head+tail (no double-population), 0 disabling a side, and
  no windows for an empty blob.
- `image: structural warm-up populates head and tail windows opportunistically` —
  the driver issues head-before-tail per layer in layer order (each
  window in 64 KiB slices), merges a small blob into one window, skips
  nullptr targets, counts failing and throwing populates without
  propagating them, issues nothing when both window sizes are 0, and
  stops early on the wall-time budget (abandoning the in-flight window).
- `image: structural warm-up budget interrupts a slow window` — the
  wall budget is real mid-window: a source sleeping 250 ms per extent
  is interrupted after one or two 64 KiB slices under a 300 ms budget
  (`windows_skipped`, `budget_exhausted`), never awaited to the
  window's end.
- `image: structural warm-up skips windows the funnel will not admit` —
  issue #35: a `-EAGAIN` populate (funnel gate closed past the admit
  timeout) skips the window (`windows_skipped`) and warm-up moves on
  without waiting.
- `image: prefetch config parses structural window knobs` — the
  `prefetch` section's honored subset parses with the documented
  defaults (enabled, 1024/1024), a 0 window size is kept, partial
  sections default field by field, and out-of-range window sizes
  (negative, or above the uint32 range) are rejected with `EINVAL`.
- `image: prefetch enable false skips structural warm-up` — with
  `prefetch.enable = false` no structural warm-up runs (stats zero)
  while the device still assembles and reads byte-exactly; with it
  enabled the local lower's merged window is populated (ADR-0012).
- `integration: structural warm-up fetches head and tail extents at bring-up` —
  against the multi-blob mock with 256 KiB windows: with warm-up
  enabled, `open_image` alone (no device read) fetches a head extent and
  a tail extent that only the warm-up can reach, while a middle extent
  stays cold; extent 4 — reachable only through the +512 tar-base
  translation of the head window — pins the windows to the tar-VIEW
  byte space; with `prefetch.enable = false` the same extents stay cold
  and the device still reads byte-exactly (ADR-0012 cold-start floor).
- `integration: structural warm-up runs before the trace blob load` —
  the mock's ordered cross-blob request log pins ADR-0012's "floor
  first": both warm-up windows of the data blob (a warm-up-only head
  extent and the tail window's first extent) are served before the
  trace blob's first data GET, and replay of a traced middle extent
  still completes (a slow trace layer must not delay the floor).
- `integration: trace blob load budget skips slow trace and keeps reads` —
  with a local data layer and a latency-injected remote trace layer, a
  fast trace load still replays, while a slow trace load times out inside
  the trace-load budget, leaves trace stats empty, and the device still
  serves byte-exact reads.
- `integration: trace layer replays warm-up through the layer store` —
  end to end against the multi-blob mock: a tar-wrapped trace layer is
  recognized, set aside, and its records warm the data layer through
  `TarOffsetSource::populate` → `LayerStore::populate` — the tar-header
  translation is pinned by attributing fetched extents on the mock (an
  extent only the replay can reach), and the device serves the data
  layer byte-exactly (ADR-0013 acceptance).
- `integration: trace replay warms the lower addressed by layer index` —
  two remote dir-configured data layers: a `layer_index` 1 record warms
  an otherwise-untouched extent of layer 1's blob only, pinning the
  warm-target ordering end to end.
- `integration: background fill completes a layer through image assembly` —
  with `download.enable` set in the image config, a first open reads a
  prefix while the `LayerStore` background fill warms every remaining
  extent to `overlaybd.commit`; a second open binds the commit with zero
  additional remote reads.
- `integration: open_image parks fills when later lower fails assembly` —
  a first remote lower opens with background fill enabled, then a later
  malformed lower fails assembly; `open_image` stops and parks the partial
  chain before unwinding it.
- `integration: admission funnel bounds on-demand latency under scavenger load` —
  through `open_image` against a serialized, latency-injected mock: a
  populate storm plus the background fill cannot push any of twelve
  on-demand image reads past a bounded 2 s each (ADR-0012).
- `integration: admission funnel collapses scavenger traffic under on-demand contention` —
  two lowers sharing the one per-device funnel threaded by `open_image`:
  the shadowed bottom layer's fill stalls while eight concurrent
  on-demand readers stream the top layer, and resumes after (ADR-0012).
- `image: malformed remote lower digest fails assembly` — a remote lower
  with a malformed `sha256:` digest fails `open_image` with
  `obd::error(EINVAL)` before any registry I/O — structural config errors
  fail loud; the ADR-0016 degrade never swallows them.
- `integration: unwritable layer dir degrades to remote-only reads` — a
  `lower.dir` that can never be created (a regular file blocks its parent
  path) does not fail `open_image`: the image boots, serves byte-exact
  remote-only reads, and writes no persistence state (ADR-0016).

Fixture data is generated in-test (`obd-mkimage`-equivalent writers from
`src/format`, deterministic patterned bytes); the mock registry serves a
single Range-capable blob. No external golden files.

## Limitations & TODO

- **The no-dir path has no local caching at all** — for dir-configured
  remote layers the kernel page cache over the
  `LayerStore` staging/commit file is the L1 (ADR-0011 rejected a
  user-space LRU on top); a remote layer without `lower.dir` is served
  remote-only (the snapshotter always sets `dir`; the empty case is the
  compatibility path).
- **Honored config surface is a subset** — `cacheConfig`, `ioEngine`,
  non-file `credentialConfig` modes, and `resultFile` handling
  are parsed-as-ignored / informational in v0.1; of `prefetch`,
  `enable`, `head_kb`, and `tail_kb` are honored (see `docs/config.md`
  for the full compatibility matrix). The trace layer IS recognized and
  replayed (ADR-0013; see Concepts → "The trace layer"), with
  these gaps: the dynamic-prefetch file-list fallback is rejected by
  design; the tar member name (`trace`) is not checked — recognition is
  the config flag plus the blob magic; remote lowers without `dir` are
  not warmed (populate is a no-op on the bare `RegistrySource`); both
  replay and the structural warm-up are still awaited inline during
  bring-up. Supervisor-driven trace recording records only fully
  satisfied OnDemand remote `pread`s at the `TraceRecordSource` tap.
  When `LayerStore` is active that tap sits below it; no-`dir` and
  ADR-0016 degraded remote-only chains are tapped before the direct
  `AdmissionSource` wrapper.
- **`lower.size` is not cross-checked** against the probed/local blob size;
  the authoritative size comes from the source at open time.
- **Writable uppers are per-device and not sealed automatically** — a
  sparse upper can recover written extents from the file/fiemap across
  reopen, while an unsealed LSMT-RW upper is not recovered by a later
  image open even after graceful shutdown writes its checkpoint; that
  checkpoint is consumed only by the supervisor's offline `commit`, and a
  fresh open truncates the unsealed file. Committing/sealing an upper into
  a new lower is an explicit, offline operation (ADR-0014);
  registry write-back remains out of scope (ADR-0007).
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
