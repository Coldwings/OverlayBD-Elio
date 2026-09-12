# Source Module (`src/source`)

Pluggable blob sources behind one async byte-source interface: local files,
OCI registry HTTP range reads, a DART P2P proxy front-end, a sparse-file
layer store with background fill, and the read admission funnel that meters
all remote traffic by class.

## Overview

The source module answers one question: *given a blob (a layer of an
OverlayBD image), where do its bytes come from?* Everything above it — the
ZFile/LSMT readers in `src/format` and the image assembly in `src/image` —
reads through a single interface, `src/source/blob_source.hpp::BlobSource`,
and never cares whether the bytes come from a local file, a registry, or a
P2P proxy.

Within a device process (`obd-device`, see `docs/supervisor.md`) each layer
blob gets its own source stack, assembled bottom-up from these pieces:

- **LocalFileSource** — a read-only local file over the Elio IO backend.
- **RegistryClient / RegistrySource** — an OCI registry HTTP client
  implementing the overlaybd registryfs v2 request contract: GET-with-Range
  only, bearer-token auth, redirect caching.
- **DART proxy support** (`dart.hpp`) — address parsing and a bounded
  reachability probe for the external DART P2P proxy; the actual request-URL
  rewriting lives in `RegistryClient` (ADR-0005).
- **TarOffsetSource** — auto-detection and removal of the tar wrapper that
  overlaybd puts around layer blobs.
- **LayerStore** — sparse-file layer persistence with a sidecar extent map
  and per-extent CRC32 (ADR-0011): every remote byte served is persisted
  into a local staging file, so remote dependence shrinks monotonically and
  survives restarts. This is the component image assembly wires behind
  `RegistrySource` for every remote layer with a per-layer directory; its
  optional background fill (the `download` config contract) warms the whole
  layer without readers.
- **AdmissionFunnel / AdmissionSource** (`admission.hpp`) — the ADR-0012
  read admission funnel: one instance per device that every remote range
  request passes before reaching the source client, plus the decorator
  that routes sources without their own funnel wiring (the remote-only
  paths) through it.
- **CredentialStore** — registry credentials from the overlaybd-compatible
  credential file, with longest-prefix matching.
- **base64** (`base64.hpp`) — a minimal RFC 4648 codec used for Basic-auth
  headers and credential-file decoding.

The module also defines the write-side extension point,
`src/source/blob_source.hpp::WritableBlobSource`, used by the writable image
mode (ADR-0008; the concrete writable layers live in `src/format`).

## Concepts

### The BlobSource contract

Every source is a positional, immutable byte range:

- **Immutability** — a blob's content and size never change for the lifetime
  of the source. This is what makes persistence (LayerStore) and wrapping
  (TarOffsetSource) sound without invalidation logic.
- **Positional reads** — `pread(buf, count, offset)` never carries a cursor;
  concurrent reads on one source are independent.
- **Optional warming** — `populate(offset, len)` asks a source to make a
  range local without delivering data (ADR-0011: prefetch and background
  fill must not materialize buffers nobody consumes). The default
  implementation is a no-op; only sources with local persistence
  (LayerStore) override it.
- **Sources compose by wrapping** — each decorator holds a
  `src/source/blob_source.hpp::BlobSourcePtr` to its inner source and
  translates offsets or adds behavior. The canonical read stack for a remote
  layer (as wired by image assembly, ADR-0011) is:

  ```
  RegistrySource → LayerStore → TarOffsetSource
  ```

  (A remote layer without a per-layer `dir` is the one exception: it is a
  plain `RegistrySource` — remote-only, no local persistence; see
  `docs/image.md`. The format module then wraps the chain with ZFile/LSMT
  readers; see `docs/image.md` for the full per-lower chain.)

### Hot path vs cold path error reporting

Following `docs/design-assumptions.md` and `src/common/errors.hpp`:

- **Hot IO paths** (`pread`, `pwrite`, `flush`) return a negative `-errno`
  value on failure and **never throw**. Callers in the ublk data plane depend
  on this.
- **Cold paths** (open/parse/probe, e.g. `LocalFileSource::open`,
  `RegistrySource::open`) report failures by throwing `obd::error` /
  `src/common/errors.hpp::format_error`. A source that cannot be constructed
  fails image assembly loudly rather than serving errors per read.

### The registry request contract (registryfs v2 semantics)

`RegistryClient` mirrors overlaybd's `registryfs_v2.cpp` behavior
(the registry contract, mirrored from overlaybd):

- **GET with Range only** — never HEAD. Blob size comes from a `Range:
  bytes=0-0` probe's `Content-Range` total.
- **Auth discovery** — an unauthenticated probe; on 401/403 the
  `WWW-Authenticate` Bearer challenge (`realm`, `service`, `scope`) is parsed
  and a token is fetched from the realm with Basic auth built from the
  credential store. Token cache lifetime honors the OAuth2 `expires_in`
  field at 80% of the declared lifetime, falling back to a fixed 30 s when
  the field is absent or unparsable (ADR-0015).
- **Self vs Redirect mode** — after auth succeeds, a second probe discovers
  whether the registry serves the blob itself (Self mode: the
  `Authorization` header rides on every data request) or redirects to a CDN
  (Redirect mode: the `Location` URL is cached for 300 s and fetched
  *without* auth — CDNs reject unexpected headers). Self-mode Bearer
  URL-info entries expire no later than the token proactive refresh
  deadline (ADR-0017).
- **Retries** — data requests retry up to 3 times with a short backoff on
  transport failures, stale-token 401/403s (dropping the cached URL info and
  re-resolving), and 429s.
- **Single-flight re-auth** — the 401-triggered token exchange is coalesced
  per `realm|service|scope` key: the first requester performs the exchange
  and concurrent 401s await the same flight. Each successful exchange bumps
  a token generation; a request that took a 401 retries only with a strictly
  newer generation, so a just-rejected token is never reused even while it
  is still time-valid (ADR-0015). The per-request retry budget is unchanged.
- **Status mapping** — 416 → `-ERANGE`, 429 → `-EBUSY`, 401/403 after retry
  → `-EPERM`, 404 → `-ENOENT`, everything else → `-EIO`.

### DART prefix passthrough (ADR-0005)

DART is an external, per-node, read-only P2P cache daemon speaking the
overlaybd prefix-passthrough convention:

```
GET http://<dart-host>:<port>/<prefix>/<full upstream URL, scheme included>
```

For example, blob `https://reg.example.com/v2/lib/nginx/blobs/sha256:abc`
behind proxy address `localhost:19145/dart` is fetched as
`http://localhost:19145/dart/https://reg.example.com/v2/lib/nginx/blobs/sha256:abc`.
The double slashes of the embedded upstream scheme are preserved verbatim
(DART parses the raw RequestURI). DART answers Range GETs with 200/206/416
and explicit `Content-Length`.

DART is an **optional accelerator**: at image-open time the proxy is probed
with a bounded (1 s), cancellable TCP connect; if it is disabled in config,
malformed, or unreachable, reads fall back to the registry directly and the
device still comes up (overlaybd `check_accelerate_url()` semantics).

### Tar wrapper auto-detection

OverlayBD registry blobs are a single file inside a tar container, in one of
two shapes (overlaybd `tar/tar_file.cpp`):

- **ustar** — a plain 512-byte ustar header; payload size comes from the
  header's octal size field; `base_offset = 512`.
- **"new tar"** (locally committed layers) — a pax extended header block,
  one pax record block, then a real header carrying the empty-marker magic
  `xxtar`/version `xx`; `base_offset = 1536` and the payload size is
  *underlying size − base offset*.

`TarOffsetSource::open` probes the first block(s) and either returns a
wrapping source or hands the original source back unchanged.

### Background fill (the download contract)

When the image config's `download.enable` is set, the `LayerStore` starts a
background fill coroutine at open: a scavenger-class bulk walk that warms
every extent nobody has read yet, so a long-lived device's locality does not
depend on access patterns (overlaybd's background-download contract, ported
onto the ADR-0011 machinery):

- start delay `delay_sec` plus a uniform random `0..delay_extra_sec` jitter
  (`std::random_device`; not part of the determinism surface);
- the ADR-0011 bulk path: contiguous missing extents are coalesced into
  larger range reads (capped near 1 MiB; `blockSize` tunes the cap) and the
  result is split back into extents for persistence accounting;
- a per-second throughput budget (`maxMBps` MiB/s) throttles fill traffic —
  a bandwidth cap, separate from the admission funnel: the budget bounds
  fill's byte rate, the funnel governs WHEN fill's requests may enter the
  source at all (both apply; fill keeps its throttle);
- fill is the ADR-0012 `Fill` scavenger class: it runs at concurrency 1,
  enters the remote only through the device's admission funnel behind both
  OnDemand and Prefetch traffic, and back-pressures itself when the
  write-behind queue is full — readers are never queued behind fill
  traffic.
- transient remote errors back off (2 s doubling, capped at 64 s) and the
  walk resumes — persisted extents survive in the sidecar, so progress is
  never lost; the backoff sleep is sliced so `stop_fill()` can still park
  the task promptly;
- completion rides the same sha256-verify + atomic rename as read-through
  warming; an all-present bitmap is only a pending verification state, so
  fill waits for the writer's verification outcome and resumes automatically
  when a mismatch restarts fresh, bounded by `try_count` (the `tryCnt`
  knob);
- `Bypass` disables fill (the same rule as on-demand persistence), and a
  `populate`/prefetch path shares the extent map with both.

### Sparse layer persistence (ADR-0011)

`LayerStore` persists every remotely-served byte into a sparse local staging
file, so a layer's dependence on the remote shrinks monotonically and
survives restarts. Image assembly wires it behind `RegistrySource` for
every remote layer with a non-empty `dir`:

- one staging file `<dir>/.download.<nonce>` (sparse-truncated to the blob
  size) plus one sidecar `<dir>/.bitmap.<nonce>`; the shared nonce in the
  file names pairs them, so a stale sidecar can never attach to a fresh
  staging file — unpaired or header-invalid files are deleted at open and
  the layer starts fresh;
- a uniform extent (64 KiB default) is the remote-fetch, persistence
  accounting, and sidecar-record unit at once;
- the sidecar is a fixed 80-byte header (magic `OBDSIDE1`, version, extent
  size, blob size, extent count, layer sha256, pairing nonce, header CRC32)
  followed by one 8-byte `{crc32, flags}` record per extent, each written
  with a single atomic `pwrite` **after** its data write completed (a crash
  may lose a bit whose data survived — safe — never the reverse);
- reads of a "present" extent verify its CRC32 first; a mismatch demotes the
  extent to a hole and re-fetches, making torn writes a deterministic cache
  miss instead of silent corruption;
- persistence is write-behind on a bounded, droppable queue drained by a
  dedicated plain `std::thread` (readers are never back-pressured, and no
  Elio worker ever runs a blocking syscall); `ENOSPC`/`EIO` moves the store
  into a bypass state — writes stop, fill switches off, reads continue
  purely remote;
- when every extent is present, the staging file is sha256-verified against
  the image-config digest and atomically renamed to
  `<dir>/overlaybd.commit` — the committed-layer contract the local probe
  binds; a mismatch restarts fresh (bounded by `try_count`);
- when the commit file binds at open, any stale `.download.*`/`.bitmap.*`
  pair left beside it (a previous run that died between record writes and
  the rename) is swept — it could never win the probe.

### Read admission funnel (ADR-0012)

Three traffic classes compete for one QoS-less resource — the throughput of
the same remote source (a registry, object storage behind it, or DART
peers). Since no source can be asked to serve one class first, priority is
enforced entirely client-side: **every remote range request of a device
passes one shared `AdmissionFunnel` before reaching the source client**.
Image assembly creates the funnel at open and threads it through every
lower's `LayerStore` (`LayerStore::Config::funnel`) and through an
`AdmissionSource` wrapper on the remote-only paths (a lower without a
`dir`, the ADR-0016 degrade path, the trace blob fetch), so the funnel is
per-device: all of a device's lowers compete at a single gate. Registry
and DART clients stay class-agnostic — the funnel governs admission,
never source selection (the ADR-0005 fallback is untouched).

The class taxonomy and admission rules:

- **OnDemand** — a ublk miss blocking a guest (`LayerStore::pread` of a
  missing extent, remote-only `pread`s, assembly probes). Admitted
  **immediately and unconditionally**, even if that momentarily exceeds
  the concurrency window; delaying a guest-visible miss to protect a
  window is never correct.
- **Prefetch** — warm-up `populate` traffic (`LayerStore::populate`):
  the structural head/tail warm-up at bring-up (ADR-0012's cold-start
  floor) and trace replay (ADR-0013). A scavenger: admitted only when
  **no on-demand request is in flight** AND total in-flight requests are
  **below the AIMD window**.
- **Fill** — the background layer fill (`LayerStore::run_fill`). Same
  scavenger gate, but the funnel keeps two FIFO queues and drains
  **Prefetch before Fill**: traced data is needed soon, fill has the
  whole runtime.

Scavenger requests are **size-capped near 1 MiB**
(`AdmissionFunnel::cap_request`): the funnel caps and the caller splits.
`LayerStore::populate` fetches extent-granular (64 KiB, naturally under
the cap); fill's coalesced range reads are clamped to the same cap (the
ADR-0011 bulk path already assembles contiguous missing extents into
larger reads). The cap bounds the head-of-line delay an arriving
on-demand read can suffer behind an already-issued scavenger request.
Extent-granular dedup is NOT the funnel's job: it lives one layer down in
the LayerStore's in-flight map. Only issued remote work is published
there: a request for an extent already being fetched joins that fetch
(whatever class started it) and never reaches the funnel, while a
Prefetch still queued for scavenger admission is not a join target for a
later OnDemand miss.

The structural warm-up itself (ADR-0012's cold-start floor) is driven
from image assembly (`src/image/structural_warmup.hpp`, documented in
`docs/image.md`): at device bring-up, a bounded window at the head and
the tail of every data lower's stored-blob view — the regions carrying
the tar header neighborhood, the ZFile/LSMT headers, and the format
indexes (ZFile jump table + trailer, LSMT index, all at the payload
tail) — is populated before trace replay. It needs no trace and no
format parsing (windows come from the blob's byte layout alone), rides
this funnel as the Prefetch class like every populate, and is
opportunistic: failures are logged and skipped, never a bring-up error,
and on a bypassed/degraded LayerStore populate is a no-op. Its 30 s
wall budget is checked between 64 KiB populate slices (one extent), and
each slice's funnel admission is bounded (`populate_admit_timeout` —
`acquire_scavenger_bounded`), so a slow source abandons a window
mid-way and a storm-closed gate skips it outright — skipped, never
awaited — instead of delaying bring-up (issue #35; see `docs/image.md`).

The window is **not operator-configured**; it is AIMD-managed from
observed on-demand latency, a LEDBAT-style scavenger that consumes only
spare capacity and yields on the first congestion signal. The exact
signals (defaults in `AdmissionFunnel::Config`):

- **sample** — the acquire-to-release wall-clock latency of each OnDemand
  request (the funnel permit brackets exactly one remote fetch);
- **baseline** — an EMA (α = 1/8) of past samples, anchored by the first
  sample and floored at 1 ms (local-speed samples carry no congestion
  signal);
- **flat** (sample ≤ baseline × 3/2) — additive increase: window += 1,
  capped at `window_max` (default 32);
- **rise** (sample > baseline × 3/2) — multiplicative decrease: window
  halved, floored at `window_min` (default 1);
- the window starts at `window_init` (default 2).

Consequences: on-demand latency is invariant under prefetch load by
construction (an on-demand request never queues at the funnel, and waits
behind at most `window` size-capped scavenger requests at the source);
scavenger throughput expands into idle capacity as the window grows and
collapses under contention as the gate closes and AIMD shrinks the
window. In-flight scavenger requests are not cancelled on a competing
miss in v1 — the size cap bounds their damage (cancellation remains a
documented refinement, ADR-0012).

### Credentials

The credential file follows overlaybd's `credentialConfig` mode=file shape —
`{ "auths": { "<key>": { "username" / "password" or "auth" } } }` — and
lookup is a **longest-prefix match** against the full blob URL (and against
the URL with its `scheme://` stripped, since auth keys commonly omit the
scheme). See `docs/config.md`.

## Public API

All types live in namespace `obd::source`. All IO functions are Elio
coroutines (`elio::coro::task<T>`) and must be `co_await`ed on a running Elio
scheduler.

### `blob_source.hpp` — the interface

```cpp
class BlobSource {
public:
    virtual ~BlobSource() = default;
    virtual elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                            uint64_t offset) = 0;
    virtual elio::coro::task<ssize_t> populate(uint64_t offset, size_t len);
    virtual uint64_t size() const noexcept = 0;
    virtual std::string_view label() const noexcept = 0;
};
using BlobSourcePtr = std::unique_ptr<BlobSource>;
```

`src/source/blob_source.hpp::BlobSource` — the single async byte-source
interface every layer of the read stack is built on.

- `pread` — reads up to `count` bytes at `offset` into caller-owned `buf`.
  Returns `count` on success; at end of blob returns the remaining byte
  count (0 at/after EOF); on failure returns a negative `-errno`.
  Implementations loop internally over short backend reads, so callers never
  see a mid-blob short read. Must not throw.
- `populate` — warms local persistence for `[offset, offset+len)` without
  delivering data (ADR-0011). Returns 0 or a negative `-errno`; the default
  implementation is a no-op (`co_return 0`), so existing sources remain
  valid without implementing it. Implementations: `LayerStore::populate`
  fetches and persists the covering extents; `TarOffsetSource::populate`
  translates by the tar base offset and forwards. Consumer: trace replay
  (ADR-0013; `src/image/trace_replay.hpp::replay_trace`) drives
  it on the data lowers' stored-blob-level sources.
- `size` — total blob size in bytes; constant for the source's lifetime.
- `label` — human-readable identity for logs (path, URL, digest); the
  returned view is stable for the source's lifetime (it points into the
  source).

```cpp
class WritableBlobSource : public BlobSource {
public:
    virtual elio::coro::task<ssize_t> pwrite(const void* buf, size_t count,
                                             uint64_t offset) = 0;
    virtual elio::coro::task<int> flush() = 0;
};
```

`src/source/blob_source.hpp::WritableBlobSource` — write-side extension for
device roots with a writable upper layer (ADR-0008). `pwrite` writes `count`
bytes at `offset` (both 512-byte multiples, like reads) and returns `count`
or a negative `-errno`; `flush` is the durability point behind ublk FLUSH
and returns 0 or a negative `-errno`. Grow-capable writable roots may report
a larger `size()` after a successful resize, matching the widened block
device. The ublk bridge answers `-EROFS` when the image root does not
implement this interface.

### `local_file.hpp` — LocalFileSource

```cpp
class LocalFileSource final : public BlobSource {
public:
    static elio::coro::task<std::unique_ptr<LocalFileSource>> open(
        std::string path);
    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;
    uint64_t size() const noexcept override;
    std::string_view label() const noexcept override;
    int fd() const noexcept;
};
```

`src/source/local_file.hpp::LocalFileSource` — a `BlobSource` over a local
read-only file, read through the Elio IO backend (io_uring when available).

- `open(path)` — opens `path` with `O_RDONLY | O_CLOEXEC` and `fstat`s the
  size. The open/stat are synchronous syscalls on the calling coroutine
  (cold path); subsequent reads are fully async. Throws `obd::error` with
  the failing errno on open/stat failure, or `obd::error(EINVAL)` when
  `path` is not a regular file.
- `pread` — clamps `count` to the size probed at open; returns 0 for
  `offset >= size()`. Loops over the backend until `count` bytes are read or
  a backend EOF occurs (possible only if the file shrank after open).
  Returns the backend's negative errno verbatim on IO failure.
- `fd()` — the underlying descriptor (diagnostics/interop).
- The destructor closes the fd via `elio::io::close_fd_for_destructor`,
  which orders the close against in-flight io_uring operations — destroying
  a source with reads in flight is safe.

Complexity: `pread` is O(count) with at most one extra backend call per
short backend read; no allocation on the read path.

### `registry.hpp` — RegistryClientConfig, RegistryClient, RegistrySource

```cpp
struct RegistryClientConfig {
    std::string user_agent = "overlaybd-elio/0.1";
    std::chrono::seconds connect_timeout{10};
    std::chrono::seconds read_timeout{30};
    std::string accelerate_base;              // DART base, empty = direct
    size_t max_response_size = 64 * 1024 * 1024;
};
```

`src/source/registry.hpp::RegistryClientConfig` — knobs for one client.
`accelerate_base` is the DART prefix base (`"http://host:port/prefix"`); when
non-empty every data request is rewritten to `accelerate_base + "/" +
<actual blob URL>` (ADR-0005). `max_response_size` bounds a single buffered
response body and therefore the largest single Range the client will serve.

```cpp
class RegistryClient {
public:
    RegistryClient(CredentialStorePtr creds, RegistryClientConfig cfg);
    elio::coro::task<ssize_t> get_data(const std::string& url, void* buf,
                                       uint64_t offset, size_t count);
    elio::coro::task<int64_t> get_length(const std::string& url);
};
using RegistryClientPtr = std::shared_ptr<RegistryClient>;
```

`src/source/registry.hpp::RegistryClient` — shared HTTP client plus auth and
redirect caches. All `RegistrySource`s of an image share one instance (the
same layering as overlaybd's per-image registryfs). A null `creds` means
anonymous pull.

- `get_data(url, buf, offset, count)` — Range GET of
  `[offset, offset+count)` into `buf`. Returns exactly `count` on success
  (callers clamp to the blob size first, so a server-side short body is an
  error, `-EIO`). Negative `-errno` on failure: `-EPERM` (auth rejected
  after re-resolution), `-ERANGE` (416), `-EBUSY` (429 after retries),
  `-ETIMEDOUT` (retry budget exhausted on transport errors), `-EIO`
  otherwise. Never throws.
- `get_length(url)` — blob size from a `Range: bytes=0-0` probe's
  `Content-Range` total; falls back to the body length when the server
  ignores Range and answers 200. Negative `-errno` on failure. Never throws.

Internal behavior worth depending on (see Concepts): tokens cached for 80%
of the OAuth2 `expires_in` lifetime (30 s fallback for absent/unparsable/
negative values, capped at 7 days) keyed by
`realm|service|scope`, with single-flight exchanges and a per-key generation
counter (ADR-0015); per-URL resolution (final URL + auth header) cached
300 s, except Self-mode Bearer entries expire no later than the token's
proactive refresh deadline; 401/403 on a data request drops the cached URL
info and re-resolves with a strictly newer token generation; a 206 whose
`Content-Range` does not start at `offset` invalidates the cache and
retries. Both caches and the
in-flight exchange map are guarded by an `elio::sync::mutex`; network IO
never happens under the lock except during the resolve probe sequence,
which is naturally serialized per URL.

```cpp
class RegistrySource final : public BlobSource {
public:
    static elio::coro::task<std::unique_ptr<RegistrySource>> open(
        RegistryClientPtr client, std::string url);
    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;
    uint64_t size() const noexcept override;
    std::string_view label() const noexcept override;
};
```

`src/source/registry.hpp::RegistrySource` — a `BlobSource` bound to one blob
URL.

- `open(client, url)` — probes the blob size (one `get_length` round trip)
  and binds the source. Throws `obd::error(EINVAL)` on a null client and
  `obd::error` with the probe's errno when the size cannot be determined.
- `pread` — clamps to the probed size (0 at/after EOF) and forwards to
  `RegistryClient::get_data`; the return value is `get_data`'s.

Blob URLs are addressed as `repoBlobUrl + "/" + digest` by the caller (image
assembly); the source treats the URL as opaque.

### `tar_offset.hpp` — TarOffsetSource

```cpp
class TarOffsetSource final : public BlobSource {
public:
    static elio::coro::task<BlobSourcePtr> open(BlobSourcePtr src);
    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;
    elio::coro::task<ssize_t> populate(uint64_t offset, size_t len) override;
    uint64_t size() const noexcept override;
    std::string_view label() const noexcept override;
    uint64_t base_offset() const noexcept;
};
```

`src/source/tar_offset.hpp::TarOffsetSource` — removes the tar wrapper
around an overlaybd layer blob.

- `open(src)` — takes ownership of `src` **either way**: returns a
  `TarOffsetSource` wrapping `src` when the ustar magic + version + header
  checksum identify a tar wrapper, or returns `src` itself unchanged when
  the blob is smaller than 512 bytes, unreadable, or fails the tar checks
  (plain-file passthrough — read failures at probe time degrade to
  passthrough, not an error). Throws `obd::error(EINVAL)` on a null source,
  `src/common/errors.hpp::format_error` on a truncated pax prefix, and
  `obd::error(EIO)` on a short read of the pax real header.
  Detection replicates overlaybd `is_tar_file()`: typeflag `x`/`g` selects
  `base_offset = 1536` (pax-wrapped "new tar", size = underlying − 1536 when
  the real header carries the `xxtar`/`xx` empty marker, else the real
  header's octal size) vs `base_offset = 512` (plain ustar, size from the
  first header's octal size field).
- `pread` — clamps to the detected payload size and forwards at
  `offset + base_offset`. A short inner read is propagated as-is (the inner
  source's contract makes that an EOF case).
- `populate` — the same translation for warm-up: clamps to the payload
  size and forwards `populate(offset + base_offset, len)` to the wrapped
  source, so trace replay (ADR-0013) addressing the payload byte space
  reaches the `LayerStore` below. When the blob is not tar-wrapped, `open`
  returns the inner source itself and its own `populate` applies.
- `base_offset()` — the detected payload offset (512 or 1536); diagnostics
  and tests.

Cost: `open` costs one (usually) or two (pax) 512-byte reads.

### `credentials.hpp` — Credential, CredentialStore

```cpp
struct Credential {
    std::string username;
    std::string password;
};

class CredentialStore {
public:
    static CredentialStore from_file(const std::string& path);
    CredentialStore() = default;
    std::optional<Credential> find(std::string_view url) const;
    bool empty() const noexcept;
    size_t size() const noexcept;
    void add(std::string key, Credential cred);
};
using CredentialStorePtr = std::shared_ptr<const CredentialStore>;
```

`src/source/credentials.hpp::CredentialStore` — registry credentials parsed
from the overlaybd-compatible credential file. Synchronous and
scheduler-free (plain file IO + JSON parse at load time; lookups are pure
string scans).

- `src/source/credentials.hpp::from_file` — reads and parses the file.
  Throws `obd::error` on IO failure and
  `src/common/errors.hpp::format_error` on malformed JSON. An empty file
  yields an empty store; a missing or non-object `auths` member also yields
  an empty store. Each entry accepts either an `auth` field (Docker-style
  base64 `user:pass`) or separate `username`/`password` fields.
- `find(url)` — longest-prefix match: returns the credential whose `auths`
  key is a prefix of `url`, preferring the longest key. Keys without a
  scheme also match by the URL's host part (the store scans both the full
  URL and the URL with `scheme://` stripped). Returns `std::nullopt` when
  nothing matches. O(n) in the number of entries, longest key first.
- `add(key, cred)` — inserts an entry directly (tests); re-sorts longest
  key first.
- The store is immutable after construction from the registry client's point
  of view: `CredentialStorePtr` is a `shared_ptr<const …>` shared by the
  client and (for Basic-challenge lookups) token fetches.

### `dart.hpp` — DART proxy support

```cpp
struct DartProxyAddress {
    std::string host;
    uint16_t port = 0;
    std::string prefix;   // e.g. "/dart"
    std::string base;     // "http://host:port/prefix"
};

std::optional<DartProxyAddress> parse_dart_address(std::string_view address);
std::string dart_prefixed_url(const DartProxyAddress& address,
                              const std::string& actual_url);
elio::coro::task<bool> dart_proxy_reachable(const DartProxyAddress& address);
```

`src/source/dart.hpp::DartProxyAddress` — the parsed form of an overlaybd
`p2pConfig.address` (`"<host>:<port>/<prefix>"` or with a scheme; the default
prefix is `/dart`).

- `parse_dart_address(address)` — strips an optional scheme (DART speaks
  plain HTTP regardless and `base` is always normalized to `http://`),
  splits authority and prefix, and validates host/port. Returns
  `std::nullopt` on missing host, missing/bad/zero/out-of-range port. Pure
  function; never throws.
- `src/source/dart.hpp::dart_prefixed_url` — builds the passthrough request
  URL `address.base + "/" + actual_url`, preserving the embedded upstream
  scheme verbatim. Pure function. (Note: `RegistryClient` performs this
  rewriting inline for its data requests; this helper exists for tests,
  tooling, and documentation of the convention.)
- `dart_proxy_reachable(address)` — bounded reachability probe: resolves the
  host and attempts a cancellable TCP connect under a 1000 ms deadline
  (`elio::with_timeout` + cancel token), so a silently-dropping network
  cannot stall image open behind TCP retransmits. Returns `false` on any
  failure (resolution, connect, timeout, exception). Mirrors overlaybd's
  `check_accelerate_url()`: on `false` the caller falls back to direct
  registry reads instead of failing the device.

### `admission.hpp` — ReadClass, AdmissionFunnel, AdmissionSource

```cpp
enum class ReadClass : int { OnDemand = 0, Prefetch = 1, Fill = 2 };

class ReadClassAwareSource {
public:
    virtual elio::coro::task<ssize_t> pread_with_class(
        ReadClass cls, void* buf, size_t count, uint64_t offset) = 0;
};

elio::coro::task<ssize_t> pread_with_class(BlobSource& source,
                                           ReadClass cls, void* buf,
                                           size_t count, uint64_t offset);

class AdmissionFunnel final {
public:
    struct Config {
        uint32_t window_init = 2;
        uint32_t window_min = 1;
        uint32_t window_max = 32;
        uint32_t ai_step = 1;
        uint32_t md_percent = 50;
        uint32_t rise_percent = 150;
        std::chrono::nanoseconds baseline_floor{1000 * 1000};
        size_t scavenger_size_cap = 1024 * 1024;
    };
    AdmissionFunnel();
    explicit AdmissionFunnel(Config cfg);
    class Permit;  // move-only RAII admission slot
    elio::coro::task<Permit> acquire(ReadClass cls);
    elio::coro::task<std::optional<Permit>> acquire_scavenger_bounded(
        ReadClass cls, std::chrono::milliseconds timeout);
    size_t cap_request(ReadClass cls, size_t bytes) const;
    void note_on_demand_latency(std::chrono::nanoseconds sample);
    uint32_t window() const;
    uint64_t inflight_on_demand() const;
    uint64_t inflight_total() const;
    uint64_t on_demand_admissions() const;
    uint64_t scavenger_admissions() const;
    uint64_t scavenger_waits() const;
    void set_gap_hook_for_test(std::function<void()> hook);  // test-only
};
using AdmissionFunnelPtr = std::shared_ptr<AdmissionFunnel>;

class AdmissionSource final : public BlobSource {
public:
    AdmissionSource(BlobSourcePtr inner, AdmissionFunnelPtr funnel);
    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;
    elio::coro::task<ssize_t> populate(uint64_t offset,
                                       size_t len) override;
    uint64_t size() const noexcept override;
    std::string_view label() const noexcept override;
};
```

`src/source/admission.hpp::AdmissionFunnel` — the per-device read
admission funnel (ADR-0012; see Concepts §"Read admission funnel").

- `acquire(cls)` — admits one request. OnDemand never suspends; a
  scavenger (Prefetch/Fill) suspends until no on-demand request is in
  flight and the AIMD window has room, queueing FIFO with Prefetch
  drained before Fill. The returned `Permit` holds the reserved slot;
  the caller's remote fetch must happen while holding it — the permit's
  lifetime is the fetch, and for OnDemand its wall-clock latency is the
  AIMD sample fed at release. Releasing may wake queued scavengers; it
  never blocks. Plain `acquire()` waiters must not be cancelled: a
  waiter cancelled after its slot was reserved would leak the slot.
- `acquire_scavenger_bounded(cls, timeout)` — the scavenger-only bounded
  variant (issue #35): if the gate has not opened within `timeout`, the
  waiter dequeues itself and the result is `std::nullopt` — the caller
  skips the request instead of waiting indefinitely (warm-up's "blocked
  windows are skipped, never awaited" contract). Cancel-safe where
  `acquire()` is not: a waiter whose slot was already reserved when the
  timeout fires adopts the slot rather than leaking it; a still-queued
  waiter is removed under the funnel mutex. OnDemand is refused
  outright (`std::nullopt`, logged — a real runtime check, not an
  assert): unconditional admission is the OnDemand contract, and a
  bounded on-demand call is always a caller bug.
- `cap_request(cls, bytes)` — clamps scavenger requests to
  `scavenger_size_cap` (~1 MiB; the caller splits); OnDemand is uncapped.
- `note_on_demand_latency(sample)` — feeds a synthetic AIMD sample
  directly (tests); production samples flow through `Permit`.
- The counters are relaxed atomic snapshots, safe to poll from any
  thread; `scavenger_waits()` counts queue events — the observable "the
  funnel is throttling" signal.
- `ReadClassAwareSource` / `pread_with_class(source, cls, ...)` is the
  opt-in metadata path for decorators that need the ADR-0012 class of a
  remote read. Sources that do not implement the optional hook receive a
  normal `BlobSource::pread`. Image trace recording uses this to record
  only OnDemand traffic while leaving the base `BlobSource` API stable.
- The `Config` window parameters are internal (the ADR-0012 window is
  not operator-configured); image assembly uses the defaults.

`src/source/admission.hpp::AdmissionSource` — a decorator routing a
source without its own funnel wiring through a shared funnel: `pread` is
admitted OnDemand, `populate` is split at the scavenger size cap and
admitted Prefetch per chunk. `size()`/`label()` pass through. Image
assembly wraps every remote-only read path with it.

### `layer_store.hpp` — LayerStore

```cpp
class LayerStore final : public BlobSource {
public:
    struct Config {
        uint32_t extent_size = 64 * 1024;
        uint64_t queue_max_bytes = 4ULL * 1024 * 1024;
        uint32_t try_count = 5;
        struct Fill {
            bool enable = false;
            uint32_t delay_sec = 300;
            uint32_t delay_extra_sec = 30;
            uint32_t max_mbps = 100;
            uint32_t block_size = 256 * 1024;
        } fill;
        AdmissionFunnelPtr funnel;  // null = no admission governance
        std::chrono::milliseconds populate_admit_timeout{0};  // 0 = wait
    };
    enum class State : int { Filling = 0, Complete = 1, Bypass = 2 };
    enum class FillStatus : int { kDisabled, kWaiting, kFilling, kDone,
                                  kStopped };
    static elio::coro::task<std::unique_ptr<LayerStore>> open(
        BlobSourcePtr remote, std::string dir, std::string expected_sha256_hex);
    static elio::coro::task<std::unique_ptr<LayerStore>> open(
        BlobSourcePtr remote, std::string dir, std::string expected_sha256_hex,
        Config cfg);
    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;
    elio::coro::task<ssize_t> populate(uint64_t offset, size_t len) override;
    uint64_t size() const noexcept override;
    std::string_view label() const noexcept override;
    State state() const noexcept;
    uint64_t extents_present() const noexcept;
    uint64_t extents_total() const noexcept;
    uint64_t dropped_writes() const noexcept;
    uint64_t crc_failures() const noexcept;
    uint64_t remote_fetches() const noexcept;
    uint64_t coalesced_joins() const noexcept;
    FillStatus fill_status() const noexcept;
    void stop_fill() noexcept;
    elio::coro::task<void> park_fill(std::chrono::milliseconds timeout);
    void set_test_write_hook(std::function<int(uint64_t)> hook);  // test-only
};
```

`src/source/layer_store.hpp::LayerStore` — a `BlobSource` that persists
remote bytes into a sparse local staging file with a sidecar extent map
(ADR-0011; see Concepts §"Sparse layer persistence").

- `src/source/layer_store.hpp::LayerStore::open` — takes ownership of
  `remote` (the read path for missing extents) and opens or creates the
  persistence state in `dir`. `expected_sha256_hex` is the hex digest from
  the image config (an optional `sha256:` prefix is stripped; empty means no
  completion verification, logged). If `<dir>/overlaybd.commit` exists, the
  store binds to it read-only (state `Complete`, no staging, no writer
  thread) and sweeps any stale `.download.*`/`.bitmap.*` pair files left
  beside it. Otherwise it scans `dir` for a `.download.X`/`.bitmap.X` pair
  whose sidecar header (magic, version, extent size, blob size, extent
  count, layer sha256, nonce, header CRC32) matches this layer; a valid
  pair's records are loaded and filling resumes, anything unpaired or
  invalid is deleted and the layer starts fresh with a new random nonce.
  Filesystem probing and staging-pair creation run through
  `elio::spawn_blocking`, so setup disk I/O does not occupy an Elio worker.
  A zero-length layer starts its writer in the completion check and is
  committed immediately. When `Config::fill.enable` is set (and the store
  is `Filling`), the background fill coroutine starts here (see Concepts
  §"Background fill").
  Throws `obd::error` on unrecoverable setup problems (null remote, bad
  config, missing/unwritable `dir`, unloadable commit file, staging-pair
  creation failure); image assembly degrades such failures to remote-only
  reads rather than failing the image (ADR-0016).
- `pread` — splits the request into extents. In `Complete` state: plain
  positional reads from the commit file. In `Filling` state, a present
  extent is read locally and CRC32-verified (the tail extent over its actual
  length); a mismatch clears the bit (memory and sidecar), counts
  `crc_failures`, and re-fetches. A missing extent (or any extent in
  `Bypass`) is fetched whole from the remote with in-flight coalescing —
  concurrent readers of the same missing extent join one fetch
  (`coalesced_joins`) — and the fetched bytes are enqueued for write-behind
  (not in `Bypass`). The starter signals fetch completion and retires the
  in-flight entry in one critical section (signal first), so a same-extent
  caller either joins or arrives after completion — it never starts a
  duplicate fetch of bytes just fetched. The starter's funnel permit
  covers exactly the remote fetch (released before the completion
  bookkeeping, the run_fill contract). With `Config::funnel` set, the
  fetch is admitted as the ADR-0012 OnDemand class. Returns the clamped
  count, or a negative `-errno` / `-EIO` on remote error/short fill. Never
  throws.
- `src/source/layer_store.hpp::LayerStore::populate` — warms every missing
  extent in `[offset, offset+len)` through the same coalesced fetch and
  write-behind path without delivering data; with `Config::funnel` set the
  fetches are admitted as the ADR-0012 Prefetch scavenger class. When
  `Config::populate_admit_timeout` is non-zero (assembled devices set it,
  issue #35), an extent whose fetch cannot be admitted within that bound
  is SKIPPED: populate fails with `-EAGAIN` (warm-up and replay count the
  window/record skipped and move on) instead of waiting at a storm-closed
  gate indefinitely. This local skip is not published as same-extent
  in-flight work, so OnDemand never inherits a queued Prefetch wait or its
  `-EAGAIN`; already-issued fetches still deduplicate across classes.
  Returns 0 or a negative `-errno`. No-op (0) in `Complete` and
  `Bypass`.
- Background fill (`Config::fill`) — one scavenger coroutine walking the
  extent map and fetching contiguous missing runs as coalesced range reads
  (cap `max(fill.block_size, extent_size)`, ≤ 1 MiB; with a funnel, the
  funnel's scavenger size cap is authoritative) admitted Fill-class at the
  funnel, split back into
  extents for the write-behind queue. Fill waits for queue room instead of
  dropping its own work (a reader's write stays droppable — the walk would
  just re-fetch it). Start delay `delay_sec + random(0, delay_extra_sec)`,
  a per-second `max_mbps` MiB/s budget, transient-error backoff, and
  completion through the same sha256 + rename path as read-through
  warming. `stop_fill()` asks the walk to exit; start delay, transient
  error backoff, and throttle waits are sliced so stop is observed
  promptly. `fill_status()` reports `kDisabled`/`kWaiting`/`kFilling`/
  `kDone`/`kStopped`, but it is observability only. Fill shares the
  lifetime contract below: call `park_fill()` before destroying the store
  so the fill coroutine has released its frame.
- Write-behind: fetched extents queue (bounded by `queue_max_bytes`) for a
  dedicated writer thread that `pwrite`s data, then the 8-byte sidecar
  record, then sets the in-memory bit. A full queue drops the entry and
  counts `dropped_writes` (never back-pressures readers). `ENOSPC`/`EIO`
  from any write enters `Bypass` (logged once): writes stop, reads continue
  remotely. Teardown stops the thread with a bounded drain-by-drop — pending
  writes are only cache, so teardown never hangs.
- Completion: when the last missing extent lands, the writer thread re-reads
  the staging file in bounded chunks, sha256-verifies it against
  `expected_sha256_hex` (skipped with a log when empty), and on match
  atomically renames it to `<dir>/overlaybd.commit` (state `Complete`;
  subsequent reads use the commit file without CRC). On mismatch it deletes
  the pair, drops queued cache writes from the failed attempt, and restarts
  fresh with a new nonce, bounded by
  `Config::try_count`; exhaustion leaves the store serving remotely in
  `Bypass` (logged as an error).
- Observability — `state()` (`src/source/layer_store.hpp::LayerStore::State`),
  `extents_present()`/`extents_total()`, `dropped_writes()`,
  `crc_failures()`, `remote_fetches()`, `coalesced_joins()`,
  `fill_status()` are relaxed atomic snapshots, safe to poll from any
  thread.
- `src/source/layer_store.hpp::LayerStore::set_test_write_hook` /
  `set_test_completion_hook` — test-only writer-thread hooks used to
  inject write failures or schedule completion-verification races. Not part
  of the module API.

Complexity: `pread` costs one local read + CRC32 per present extent, one
coalesced remote fetch per missing extent, plus one memcpy per extent; the
sidecar costs 8 bytes per extent on disk and in memory.

### `base64.hpp` — base64_encode, base64_decode

```cpp
inline std::string base64_encode(std::string_view in);
inline std::string base64_decode(std::string_view in);
```

`src/source/base64.hpp::base64_encode` — RFC 4648 encoding, no line
wrapping, with `=` padding. `src/source/base64.hpp::base64_decode` —
permissive decoding: stops at the first `=` or invalid character and returns
the bytes decoded so far (matching the needs of auth files, not strict
validation). Both are pure, header-only, and scheduler-free.

## Invariants & Guarantees

Callers may rely on:

- **Full reads or EOF only** — a successful `pread` returns exactly `count`
  except at end of blob, where it returns the remaining bytes (0 at/after
  EOF). No caller ever sees a mid-blob short read; implementations loop over
  short backend reads internally.
- **No exceptions from hot paths** — `pread`/`pwrite`/`flush` report failure
  as a negative `-errno` and never throw; construction/open functions throw
  `obd::error` / `obd::format_error` instead.
- **Size and label stability** — `size()` is constant and `label()`'s
  returned view stays valid for the source's lifetime (blobs are immutable).
- **Positional purity** — `pread` mutates no shared read state visible to
  other readers; concurrent `pread`s on one source are safe (see the
  per-type notes below).
- **Deterministic layering** — decorators change offsets and caching, never
  content: reading the same range through any composition of the sources
  above yields byte-identical results to reading the underlying blob at the
  translated offset.
- **Commit integrity** — `overlaybd.commit` only ever appears via an atomic
  `rename()` after a successful sha256 verification (when a digest is
  configured); a `.download.<nonce>` staging file is never served to
  readers (every byte of it is per-extent CRC-verified before it is).
- **LayerStore crash rule** — a sidecar bit is set only after the extent's
  data write completed, and every local read verifies the extent's CRC32:
  a crash or torn write degrades to a re-fetch, never to bad data.
- **Registry byte-exactness** — `RegistryClient::get_data` returns `count`
  or an error, never a short count; a server short body maps to `-EIO`.
- **Admission funnel priority (ADR-0012)** — an OnDemand request never
  suspends at the funnel; a scavenger (Prefetch/Fill) request enters the
  source only when no on-demand request is in flight and total in-flight
  requests are below the AIMD window, and is size-capped near 1 MiB.
  Every remote read of an assembled device passes the funnel — the
  LayerStore fetch paths (miss/populate/fill) and the `AdmissionSource`
  wrappers are the only remote-fetch call sites below the format layers.

## Concurrency & Call Permissions

- **Scheduler context** — every `open`, `pread`, `pwrite`, `flush`,
  `populate`, `get_data`, `get_length`, `dart_proxy_reachable`, and the
  LayerStore fill coroutine must run on a thread with a running Elio
  scheduler.
  `CredentialStore` and the base64 helpers are synchronous and
  scheduler-free. LayerStore's blocking disk writes (and its completion
  read-back) run on its own dedicated plain `std::thread`, never on an Elio
  worker — the same precedent as the ublk queue threads.
- **Concurrent reads** — concurrent `pread`s on one source are safe for
  every type in this module. `LocalFileSource` and `RegistrySource` are
  stateless per read; `RegistryClient`'s caches are mutex-guarded and its
  token exchanges are single-flight per key (concurrent re-auths share one
  exchange, ADR-0015); `LayerStore`
  shares its extent map with the writer thread through per-extent atomics
  and coalesces concurrent fetches of one extent to a single remote read;
  the fill coroutine never holds the fetch-coalescing lock over a wait, so
  readers stay latency-bounded while fill is active.
- **Instance state** — mutable state per instance: `RegistryClient` (token
  and URL-info caches), `LayerStore` (extent map, write-behind queue,
  state/counter/fill atomics), `AdmissionFunnel` (window, in-flight
  counters, scavenger queues — `std::mutex`-guarded, safe from any Elio
  worker thread; the funnel is shared by all of a device's sources).
  No
  hidden global mutable state anywhere in the module.
- **Buffer ownership** — `buf` arguments are borrowed for the duration of
  the `co_await`; sources never retain pointers into caller buffers.
  Fetched extent data is held as `shared_ptr<const …>` while it awaits
  write-behind.
- **Object lifetimes** — sources are owned via `BlobSourcePtr`
  (`unique_ptr`); wrapping takes ownership. A `LocalFileSource` may be
  destroyed with reads in flight (the destructor orders the fd close against
  the io_uring backend). A `LayerStore` must not be destroyed while
  `pread`/`populate` coroutines or a background fill are in flight on it
  (a suspended fetch, joiner, or fill step touches members on resume) —
  park an active fill first: `park_fill()` stops the walk and waits for
  the joinable fill task to release its coroutine frame. A terminal
  `fill_status()` is useful for observation, but it is not a destruction
  barrier by itself.
- **Writable sources** — `WritableBlobSource::pwrite`/`flush` are called only
  from the ublk data plane on the image root, after the bridge has confirmed
  the root implements the interface; writers and readers may race on
  distinct ranges (the concrete writable layers in `src/format` define the
  overlap rules — see `docs/format.md`).

## Stability Contract

Breaking changes (require an ADR per the trigger list in
`docs/adr/README.md`):

- **The registry wire behavior** is an operator contract: GET-with-Range
  only, the bearer-token flow, Self/Redirect redirect handling, and the
  status→errno mapping mirror overlaybd `registryfs_v2.cpp`. Cache policy
  on top of that wire behavior is ours (ADR-0015, ADR-0017): token
  lifetime is 80% of the server's `expires_in` (30 s fallback) with
  single-flight, generation-counted re-auth; Redirect, anonymous, and
  Basic URL-info entries keep the fixed 300 s lifetime, while Self-mode
  Bearer URL-info entries expire no later than the token proactive
  refresh deadline. Registries and CDNs that work with overlaybd must
  keep working here.
- **The DART integration shape** (ADR-0005): DART stays an external process
  reached by prefix passthrough (`base + "/" + full upstream URL`, embedded
  scheme preserved); in-process P2P is rejected. Enabled-but-unreachable
  DART must keep falling back to direct registry reads — a device must never
  fail to open because the accelerator is down.
- **On-disk persistence artifacts**: the staging-pair names
  `.download.<nonce>` / `.bitmap.<nonce>`, the sidecar layout, and the
  target name `overlaybd.commit` with its sha256-verify + atomic rename
  rule. Other tools (and earlier runs of this project) rely on these names
  and states.
- **The credential file format** (`auths` map, `auth` or
  `username`/`password` entries) and longest-prefix matching semantics
  mirror overlaybd; operator credential files must keep working unchanged.
- **The tar-wrapper detection** replicates overlaybd `tar/tar_file.cpp`
  (ustar magic + checksum, pax prefix, `xxtar` marker); blobs produced by
  upstream overlaybd tools must keep unwrapping identically.
- **The hot-path error convention** (negative `-errno`, no throwing from
  `pread`) is consumed by the ublk data plane; weakening it breaks the
  device.

Compatible (non-breaking) changes: new `BlobSource` implementations, new
decorators inserted by image assembly, cache-size/config default tuning, and
additive status/observability APIs.

## Testing

Unit tests live in `tests/unit/test_source.cpp`,
`tests/unit/test_layer_store.cpp`,
`tests/unit/test_admission.cpp`, and
`tests/unit/test_registry.cpp` (Catch2; the registry tests run against an
in-process mock registry speaking the auth/redirect/DART contract), plus
`common: base64 round-trips and decodes cred.json form` in
`tests/unit/test_common.cpp`. Integration tests live in
`tests/integration/test_integration.cpp` against a mock Range-capable blob
server. Run everything with `ctest --test-dir build --output-on-failure`
(see `docs/testing.md`).

- `source: tar adapter detects ustar wrapper and skips the header` — a
  ustar-wrapped blob is detected, `base_offset` is 512, the payload size
  comes from the octal size field, and reads return the payload bytes
  exactly.
- `source: tar adapter passes plain files through unwrapped` — a blob
  without ustar magic is handed back as the identical source object
  (passthrough identity), with size preserved.
- `source: credential store longest-prefix matching` — the longest matching
  `auths` key wins over a shorter host key, `auth`-form entries decode to
  `username`/`password`, and unknown hosts match nothing.
- `source: DART address parsing and prefixed URL` — address forms parse to
  host/port/prefix/base (including the `/dart` default and scheme
  stripping), malformed addresses (no port, non-numeric port, port > 65535)
  are rejected, and the prefixed URL preserves the embedded upstream URL
  verbatim.
- `source: registry range reads and size probe` — `RegistrySource::open`
  learns the size from the Range 0-0 probe and `pread` returns exact bytes
  for an interior range.
- `source: registry bearer auth flow via token endpoint` — a 401 Bearer
  challenge drives a token fetch with Basic credentials, and subsequent
  reads carry the bearer token (the mock only serves `Bearer sekrit`).
- `source: registry redirect mode drops auth on the CDN URL` — a 302 to a
  CDN URL is cached and followed anonymously; the blob is served through the
  redirect.
- `source: DART prefix passthrough preserves the embedded URL` — with
  `accelerate_base` set, the mock observes the request path
  `/dart/<full upstream URL>` with the double slashes of the embedded scheme
  intact, and the data still round-trips.
- `source: layer store cold read persists and reopen serves locally` — a
  cold read fetches from the remote and persists; after destroy + reopen
  the warmed extents are served locally with zero remote reads.
- `source: layer store detects corrupted staging via crc and refetches` —
  a staging file corrupted on disk between runs fails the per-extent CRC32,
  is demoted to a hole, re-fetched (correct data), and counted in
  `crc_failures`.
- `source: layer store deletes stale sidecar and restarts fresh` — a
  sidecar renamed to a wrong nonce name, and one with clobbered magic, are
  both deleted (with their staging files) and the layer restarts fresh.
- `source: layer store drops writes when the queue is full` — a stalled
  writer plus a one-extent queue bounds the write-behind queue: extra
  writes drop (`dropped_writes`), reads stay correct, and the queued
  entries persist once the writer resumes.
- `source: layer store enters bypass on write failure` — an injected
  `ENOSPC` or `EIO` moves the store to `Bypass`: reads keep working
  remotely, `populate` is a no-op, and nothing further persists even after
  the injected failure stops.
- `source: layer store stays filling after a non-fatal write error` — an
  injected non-ENOSPC/EIO failure drops only that entry: the store stays
  `Filling`, other extents persist, the failed extent re-fetches.
- `source: layer store completes to overlaybd.commit and reopens read-only` —
  filling every extent sha256-verifies and renames the staging file to
  `overlaybd.commit` (sidecar gone, state `Complete`); a reopen binds the
  commit with zero remote reads.
- `source: layer store restarts on sha mismatch within try count` — a wrong
  expected digest fails completion verification, restarts fresh with a new
  nonce exactly `try_count` times, then stays remote-serving in `Bypass`.
- `source: layer store populate warms extents without serving data` —
  `populate` returns 0, delivers no data, and the warmed extents are local
  after a reopen.
- `source: layer store coalesces concurrent fetches of one extent` — N
  concurrent preads of one missing extent cause exactly 1 remote fetch and
  N-1 `coalesced_joins`.
- `source: layer store handles a tail extent at eof` — a blob whose size is
  not a multiple of the extent size reads, persists, and CRC-verifies its
  short tail extent correctly (EOF clamping included).
- `source: layer store completes a fully-filled pair on reopen` — a pair
  left fully filled by a dead previous run (records written, rename never
  happened) is verified and renamed to `overlaybd.commit` immediately at
  reopen.
- `source: layer store accepts digest forms and rejects malformed` — the
  `sha256:` prefix and uppercase hex are accepted (normalized before
  comparison); malformed digests fail `open` with `EINVAL`.
- `source: layer store completes without verification when digest is empty` —
  an empty expected digest zero-fills the sidecar header (resume still
  matches) and completes to `overlaybd.commit` without verification.
- `source: layer store completes an empty layer` — a zero-length remote
  creates an empty `overlaybd.commit` instead of remaining in `Filling`.
- `source: layer store fill completes a partially warmed store` — with
  fill enabled, one read-through extent plus the background walk warm the
  whole layer: the store completes to `overlaybd.commit` (`kDone`) and a
  reopen serves everything locally.
- `source: layer store fill resumes from the sidecar across a restart` — a
  fill stopped mid-walk leaves persisted extents in the sidecar; a reopen
  resumes them (not refetched) and the restarted fill completes the layer.
- `source: layer store fill honors the throughput throttle` — a 1 MiB/s
  budget makes a 3 MiB fill take measurable seconds instead of
  milliseconds.
- `source: layer store fill stop interrupts error backoff` — a transient
  remote read failure enters the exponential fill backoff; `stop_fill()`
  interrupts that sleep promptly and `park_fill()` releases the task.
- `source: layer store fill stays off in bypass` — an injected `ENOSPC`
  flips the store to `Bypass` mid-fill: fill stops (`kStopped`), reads
  continue remotely, nothing persists, no commit appears.
- `source: layer store fill is disabled without download.enable` — with
  fill not configured there is no background traffic at all: `kDisabled`,
  zero extents present, zero remote reads.
- `source: layer store sweeps stale pairs when the commit binds` — a dir
  holding `overlaybd.commit` plus leftover `.download.*`/`.bitmap.*` files
  binds the commit and removes the pair files.
- `source: layer store fill does not starve readers` — with fill active
  and back-pressured (tiny queue, slow disk), eight concurrent-reader
  preads of cold extents each complete within a bounded 1 s budget
  byte-exactly.
- `source: registry concurrent 401s share one token refresh` — eight
  concurrent reads that all take a 401 on the server-side-expired token
  trigger exactly one coalesced token exchange (the mock counts token
  endpoint hits), and every read succeeds (ADR-0015).
- `source: registry failed token refresh reaches all concurrent waiters` —
  with the token endpoint rejecting every exchange, eight concurrent 401s
  share one failed flight: all receive `-EPERM` (no hang, no wrong
  success), and the first read after the endpoint recovers starts a fresh
  flight and succeeds (ADR-0015).
- `source: registry 401 retry budget is bounded when re-auth keeps failing` —
  a registry that rejects every fresh token on data GETs drives one token
  exchange per retry attempt (generation rule) and then `-EPERM`: bounded
  retries, no livelock (ADR-0015).
- `source: registry re-auths after expires_in lifetime elapses` — with
  `expires_in=1` (800 ms cache lifetime) a resolution within the lifetime
  reuses the token and a resolution after it performs a new exchange.
- `source: registry self-mode URL cache follows bearer expiry` — an
  already-open Self-mode source on the same URL refreshes before data GETs
  once the bearer token passes its proactive expiry.
- `source: registry keeps the cached token within expires_in lifetime` —
  with `expires_in=100` (80 s cache lifetime) repeated resolutions and
  reads never hit the token endpoint again.
- `source: registry survives hostile token endpoint fields` — a float
  `expires_in` of `1e100` is ignored rather than converted (no UB), a
  non-string `token` field surfaces as `-EINVAL` through the `-errno`
  discipline instead of an escaping exception, and string `expires_in`
  values are strict (`"1junk"` takes the fallback, `"1"` is honored)
  (ADR-0015).
- `source: registry token cache lifetime derives from expires_in` — the
  lifetime mapping itself: 80% of the declared value, 0 for
  `expires_in=0`, the 30 s fallback for absent/negative values, and the
  7-day cap for absurd ones.
- `source: admission funnel admits on-demand unconditionally under a full window` —
  with the window fully occupied by scavengers, on-demand acquires still
  complete immediately and the in-flight count exceeds the window
  (ADR-0012).
- `source: admission funnel blocks scavengers while on-demand is in flight` —
  queued Prefetch/Fill requests stay parked while an on-demand request is
  in flight (even with window room) and enter once it completes.
- `source: admission funnel bounded scavenger acquire times out and dequeues` —
  issue #35: a bounded scavenger acquire whose gate stays closed returns
  `std::nullopt` at its timeout, dequeues without leaking a slot, is
  still admitted when the gate opens before the deadline, and refuses
  OnDemand outright.
- `source: admission funnel grows additively on flat latency and halves on rise` —
  flat samples at the EMA baseline raise the window by one each; a sample
  above 150% of the baseline halves it (AIMD, ADR-0012).
- `source: admission funnel respects window floor and ceiling` — sustained
  flat samples stop at `window_max`; samples that keep outrunning the EMA
  baseline drive the window to `window_min`, not below.
- `source: admission funnel admits prefetch before fill` — with room for
  exactly one scavenger, a queued Prefetch is admitted ahead of a queued
  Fill (two-level scavenger queue, ADR-0012).
- `source: admission funnel caps scavenger request size` — scavenger
  requests clamp to the ~1 MiB cap (custom caps honored); OnDemand is
  uncapped.
- `source: admission source splits populate at the scavenger size cap` —
  `AdmissionSource::populate` forwards successive ≤ 1 MiB chunks at
  successive offsets (Prefetch class), and `pread` is counted OnDemand.
- `source: layer store populate waits at the admission funnel while reads pass` —
  with the window held full, a store `pread` (OnDemand) completes anyway
  while a `populate` (Prefetch) queues, proceeding when a slot frees.
- `source: layer store populate skips the extent when the funnel gate stays closed` —
  issue #35: with `populate_admit_timeout` set and the gate held closed,
  `populate` fails the extent with `-EAGAIN` within the bound and
  succeeds once the gate opens.
- `source: on-demand bypasses queued prefetch with populate timeout` —
  issue #22: a same-extent OnDemand read completes with bytes while a
  Prefetch is still queued behind an unrelated gate holder, and it does
  not inherit the Prefetch-only `-EAGAIN` timeout.
- `source: on-demand bypasses queued prefetch with unbounded populate wait` —
  issue #22: with the default unbounded populate wait, a same-extent
  OnDemand read still completes before the unrelated gate holder is
  released.
- `source: admission funnel re-checks the gate when queueing a scavenger` —
  lost-wakeup regression: the scavenger slow path pushes its waiter and
  re-runs admission in one critical section, so a release landing in the
  check-then-queue gap (injected via the test hook) cannot strand a
  queued waiter.
- `source: layer store fill frees the funnel window before throttling` —
  fill's permit covers the remote fetch alone: a queued Prefetch is
  admitted as soon as fill's fetch completes, not after fill's 1 s
  `maxMBps` throttle sleep.
- `source: populate joining an in-flight fetch bypasses the funnel` — a
  `populate` for an extent already being fetched joins the in-flight
  fetch and consumes no scavenger admission (dedup below the funnel,
  ADR-0012).
- `source: layer store fetch frees the funnel slot before retiring the fetch` —
  the starter's funnel permit covers exactly the remote fetch: the slot
  is already released when the completion bookkeeping (in-flight map
  retire) runs.
- `integration: structural warm-up fetches head and tail extents at bring-up` —
  the ADR-0012 cold-start floor end to end: with warm-up enabled,
  `open_image` alone (no device read) fetches head/tail window extents
  only the warm-up can reach (per-extent attribution on the mock;
  extent 4 pins the windows to the tar-VIEW byte space via the +512
  translation), a middle extent stays cold, and with
  `prefetch.enable = false` nothing is warmed.
- `integration: structural warm-up runs before the trace blob load` —
  ADR-0012 "floor first", pinned via the mock's ordered request log:
  both warm-up windows of the data blob (head extent, tail window's
  first extent) are served before the trace blob's first data GET, so a
  slow trace layer cannot delay the floor.
- `integration: layered stack stages over a mock registry` — the manual
  composition RegistrySource → LayerStore → TarOffsetSource → ZFile → LSMT
  merge reads the original content byte-exactly (the same chain image
  assembly wires, ADR-0011).
- `integration: cancelled connect probe does not break later io` — a DART
  probe against a dead port returns `false` quickly and subsequent registry
  IO on the same scheduler is unaffected (no leaked cancellation state).
- `integration: enabled-but-unreachable DART falls back to direct reads` —
  with `p2pConfig` enabled but nothing listening, image assembly still opens
  and serves the full image directly from the registry (ADR-0005 optional
  accelerator).
- `integration: background fill completes a layer through image assembly` —
  with `download.enable` set, a first open reads a prefix while the
  background fill warms every remaining extent to `overlaybd.commit`; a
  second open binds the commit with zero additional remote reads.
- `integration: open_image parks fills when later lower fails assembly` —
  a first remote lower opens with background fill enabled, then a later
  malformed lower fails assembly; `open_image` stops and parks the partial
  chain before unwinding it.
- `integration: unwritable layer dir degrades to remote-only reads` — a
  layer dir that can never be created (a file blocks its parent path) does
  not fail `open_image`: the image boots and serves byte-exact reads
  remote-only, with no persistence state written (ADR-0016).
- `integration: image assembly serves remote reads through the layer store` —
  image assembly wires `RegistrySource → LayerStore` for a remote lower
  and serves reads byte-exactly while persisting read-through state into
  the layer dir (ADR-0011).
- `integration: layer store restart serves warmed extents without remote reads` —
  a partially-warmed staging pair survives an assembly restart: the second
  open serves the warmed reads locally (mock remote-read counter flat).
- `integration: completed layer store commit binds read-only without remote reads` —
  a store driven to completion installs `overlaybd.commit`, which the next
  assembly binds locally with zero remote data reads and sweeps stale
  `.download.*`/`.bitmap.*` files beside the committed layer.
- `integration: admission funnel bounds on-demand latency under scavenger load` —
  against a serialized, latency-injected mock registry (capacity 1,
  25 ms), with a six-coroutine populate storm plus the background fill
  running, twelve sequential on-demand image reads each complete within a
  bounded 2 s, byte-exactly, and the funnel's `scavenger_waits` counter
  proves the storm was really throttled (ADR-0012).
- `integration: admission funnel collapses scavenger traffic under on-demand contention` —
  two lowers sharing the one per-device funnel: while eight concurrent
  on-demand readers stream cold extents of the top layer, the shadowed
  bottom layer's fill makes essentially no progress (≤ 6 fetches vs
  dozens of on-demand fetches), and resumes once the contention stops
  (ADR-0012).

No golden external files: blob contents are deterministic patterned bytes
generated in-test; the mock registry implements the RFC 7233 / OCI subset
directly.

## Limitations & TODO

- **The no-dir path has no local caching at all.** A remote layer without
  `lower.dir` is served remote-only (the snapshotter always sets `dir`;
  the empty case is the compatibility path). Its reads still pass the
  device's admission funnel (ADR-0012) through `AdmissionSource`. DART,
  when enabled, is the shared on-node cache in front of such reads.
- **In-flight scavenger requests are not cancelled on a competing miss.**
  ADR-0012 defers cancellation: the ~1 MiB scavenger size cap bounds the
  head-of-line delay an on-demand read can suffer behind an
  already-issued prefetch or fill request.
- **Construction-time probes bypass the funnel.** Registry size probes
  (`get_length`, one per remote layer at open) and the DART reachability
  probe are cold-path control traffic, not repeatable request flow; the
  funnel governs `pread`/`populate` traffic.
- **Single-request size bound** — one registry Range read is bounded by
  `RegistryClientConfig::max_response_size` (64 MiB default); larger single
  requests must be split by the caller (the format readers already read in
  bounded blocks).
- **URL-info cache lifetime is mode-dependent** — Redirect, anonymous,
  and Basic URL-info entries use the fixed 300 s lifetime; Self-mode
  Bearer entries are capped by the token proactive refresh deadline
  (ADR-0017). The lifetime is not config. Registries issuing shorter-lived
  redirect targets rely on the 401-drop-and-re-resolve path because
  redirect responses carry no comparable declared lifetime.
- **Prefetch is structural warm-up plus trace replay** — overlaybd's
  dynamic prefetcher remains out of scope (ADR-0007). TurboOCI random
  reads use the separate gzip restart-index source (ADR-0020). The
  structural head/tail warm-up (ADR-0012's cold-start floor) and the
  upstream-compatible trace blob (ADR-0013 — the trace layer
  is recognized in image assembly; see `docs/image.md`) both ride
  `populate` at the ADR-0012 Prefetch scavenger class. Supervisor-driven
  trace recording is implemented by a `TraceRecordSource` tap around the
  remote source path and records only fully satisfied OnDemand remote
  `pread`s: when a configured `dir` opens an active `LayerStore`, the
  tap observes class-tagged reads below that store; no-`dir` lowers and
  ADR-0016 degraded remote-only chains are tapped before the direct
  `AdmissionSource` wrapper. Prefetch and Fill traffic is filtered out
  by class. The dynamic file-list fallback is unsupported by design;
  detaching warm-up/replay off the bring-up path (now safe under the
  funnel) remains the open follow-up.
- **credentialConfig mode=file only** — inline/secret credential modes are
  ignored (see `docs/image.md` / `docs/config.md`).
- **Fill teardown goes through `park_image_fills`** — destroying a store
  with a fill in flight is a use-after-free (the fill coroutine touches
  members on resume), so assembled chains are parked before destruction
  (the device server calls `park_image_fills` during shutdown and
  post-open boot-failure cleanup; stores composed by hand use
  `park_fill()`; start delay, transient-error backoff, and throttle waits
  are slept in bounded slices so parking is prompt).
  `delay_extra_sec` uses `std::random_device` per store, so delays are
  not reproducible run-to-run.
- **Redirect caching trusts `Location` for 300 s** — a CDN URL that expires
  sooner surfaces as a re-resolution after a 401/403; other 4xx/5xx
  responses on a stale redirect map to their errno without extending the
  retry budget.
- **DART probing happens once at image open** — a proxy that comes up later
  is not picked up until reopen; conversely a proxy that dies after a
  successful probe surfaces as per-request errors (no mid-image re-probe).

### TurboOCI target byte spaces (ADR-0020)

The original target remains a complete tar or gzip blob. Remote target caching
stores these original bytes in a digest-keyed directory separate from filesystem
metadata; gzip restart decoding sits above that store. Structural warm-up may
populate both metadata and target sources. Upstream trace records address only
metadata sources, one slot per lower: target offsets are never recorded as
metadata offsets or inserted as extra layer IDs. Target reads still share the
device read-admission funnel and registry client.
