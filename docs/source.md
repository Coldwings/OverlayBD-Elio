# Source Module (`src/source`)

Pluggable blob sources behind one async byte-source interface: local files,
OCI registry HTTP range reads, a DART P2P proxy front-end, an in-memory chunk
cache, a background downloader, a remote→local switch source, and a
sparse-file layer store.

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
- **ChunkCache** — an in-memory LRU cache of ~64 KiB chunks in front of a
  slow remote source.
- **TarOffsetSource** — auto-detection and removal of the tar wrapper that
  overlaybd puts around layer blobs.
- **Downloader / SwitchSource** — an optional background full-blob download
  into the per-layer directory, with an atomic switch of the read path once
  the local copy is verified and installed.
- **LayerStore** — sparse-file layer persistence with a sidecar extent map
  and per-extent CRC32 (ADR-0011): every remote byte served is persisted
  into a local staging file, so remote dependence shrinks monotonically and
  survives restarts. This is the component image assembly wires behind
  `RegistrySource` for every remote layer with a per-layer directory.
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
  of the source. This is what makes caching (ChunkCache) and wrapping
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

  (A remote layer without a per-layer `dir` is the one exception: it keeps
  the legacy `RegistrySource → ChunkCache` chain until part 3 — see
  `docs/image.md`. The format module then wraps this with ZFile/LSMT
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
(`docs/design-assumptions.md` §S-3):

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
  *without* auth — CDNs reject unexpected headers).
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

### Background download and the remote→local switch

**Retired from image assembly (ADR-0011 part 2).** Assembly no longer
composes `Downloader`/`SwitchSource` — the `LayerStore` replaced them and
the `ChunkCache` in the remote-layer chain. The components remain in the
module (their tests compose them manually) until the ADR-0011 follow-up
(part 3), which ports background fill into the LayerStore and decides
their removal. The historical mechanism, kept for reference:

When `download.enable` was set, a background `Downloader` pulled the whole
blob into the per-layer directory while reads kept being served remotely
(overlaybd's download contract):

- staging file `<dir>/.download`, `ftruncate`'d sparse to the blob size;
- resume via `SEEK_HOLE` (a previous partial download continues where it
  stopped; filesystems without `SEEK_HOLE` restart from 0);
- chunked reads (`block_size`) with a per-second throughput window
  (`maxMBps`);
- on completion the file is sha256-verified against the image-config digest
  (mismatch → discard and restart, up to `tryCnt` attempts), then atomically
  `rename()`d to `<dir>/overlaybd.commit`;
- the `SwitchSource` in the read path flips to the local file via a shared
  control block — whole-file, not per-extent
  (`docs/design-assumptions.md` §S-4).

### Sparse layer persistence (ADR-0011)

`LayerStore` persists every remotely-served byte into a sparse local staging
file, so a layer's dependence on the remote shrinks monotonically and
survives restarts. Image assembly wires it behind `RegistrySource` for
every remote layer with a non-empty `dir` (part 1 landed the component;
part 2 wired it in place of ChunkCache/SwitchSource; part 3 ports
background fill into it):

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
  into a bypass state — writes stop, reads continue purely remote;
- when every extent is present, the staging file is sha256-verified against
  the image-config digest and atomically renamed to
  `<dir>/overlaybd.commit` — the same committed-layer contract the
  Downloader installs; a mismatch restarts fresh (bounded by `try_count`).

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
  (ADR-0013, proposed; `src/image/trace_replay.hpp::replay_trace`) drives
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
and returns 0 or a negative `-errno`. The ublk bridge answers `-EROFS` when
the image root does not implement this interface.

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
300 s; 401/403 on a data request drops the cached URL info and re-resolves
with a strictly newer token generation; a 206 whose `Content-Range` does not
start at `offset` invalidates the cache and retries. Both caches and the
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

### `chunk_cache.hpp` — ChunkCache

```cpp
class ChunkCache final : public BlobSource {
public:
    struct Config {
        size_t chunk_size = 64 * 1024;
        size_t max_bytes = 256ULL * 1024 * 1024;
    };
    static elio::coro::task<std::unique_ptr<ChunkCache>> open(
        BlobSourcePtr inner);
    static elio::coro::task<std::unique_ptr<ChunkCache>> open(
        BlobSourcePtr inner, Config cfg);
    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;
    uint64_t size() const noexcept override;
    std::string_view label() const noexcept override;
    uint64_t hits() const noexcept;
    uint64_t misses() const noexcept;
};
```

`src/source/chunk_cache.hpp::ChunkCache` — an in-memory LRU cache of
fixed-size chunks in front of a slow source (registry or DART). It stands in
for overlaybd's file-based cache: the per-device process model makes a
private in-memory cache simpler and safer than shared on-disk cache files,
and when DART is in the path DART itself provides the shared on-node disk
cache (`docs/design-assumptions.md` §S-4). Since ADR-0011 part 2, image
assembly composes it **only on the no-dir exception path** (a remote layer
without `lower.dir`, where no persistence is possible): dir-configured
remote layers use the `LayerStore` instead, with the kernel page cache
over the staging/commit file as the L1; the component remains until the
part-3 follow-up decides its removal.

- `open(inner, cfg)` — takes ownership of `inner`, which must report a
  stable size. Throws `obd::error(EINVAL)` on a null source or an invalid
  config (`chunk_size == 0` or `max_bytes < chunk_size`). The one-argument
  overload uses the defaults (64 KiB chunks, 256 MiB budget).
- `pread` — serves the request chunk by chunk. On a miss the whole
  containing chunk is filled from the inner source *outside* the lock, then
  inserted; eviction runs from the LRU back until within `max_bytes` (the
  just-filled chunk is never its own victim). Returns the full clamped count
  on success. On an inner error: bytes already served are returned, or the
  inner error / `-EIO` (short fill) when nothing was served yet.
- `hits()` / `misses()` — monotonic chunk-request counters (relaxed
  atomics); observability only.

Complexity: O(1) map/list operations per chunk touched, plus one memcpy per
chunk; a miss costs one inner `pread` of one chunk. Memory is bounded by
`max_bytes` plus one in-flight fill per concurrent miss.

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

### `downloader.hpp` — DownloadConfig, Downloader

```cpp
struct DownloadConfig {
    bool enable = false;
    uint32_t delay_sec = 300;        // start delay after device open
    uint32_t delay_extra_sec = 30;   // + random(0, extra)
    uint32_t max_mbps = 100;         // throttle, MiB/s
    uint32_t try_count = 5;
    uint32_t block_size = 256 * 1024;
};
```

`src/source/downloader.hpp::DownloadConfig` — the overlaybd download knobs
(config field names: `enable`, `delay`, `delayExtra`, `maxMBps`, `tryCnt`,
`blockSize`; see `docs/image.md` and `docs/config.md`).

```cpp
class Downloader {
public:
    enum class Status : int { kIdle, kWaiting, kDownloading, kVerifying,
                              kDone, kFailed };
    Downloader(BlobSourcePtr remote, std::string dir,
               std::string expected_sha256, DownloadConfig cfg);
    void start();
    Status status() const noexcept;
    int last_error() const noexcept;
    uint64_t bytes_done() const noexcept;
    uint64_t bytes_total() const noexcept;
    static std::string staging_path(const std::string& dir);  // dir/.download
    static std::string target_path(const std::string& dir);   // dir/overlaybd.commit
    std::function<elio::coro::task<void>(const std::string&)> on_complete;
};
```

`src/source/downloader.hpp::Downloader` — pulls one remote blob into the
per-layer directory in the background. (Not wired by image assembly since
ADR-0011 part 2; see Concepts §"Background download and the remote→local
switch".)

- The constructor takes ownership of `remote` (the raw, un-cached source —
  the download must not pollute the read-path chunk cache) and snapshots
  `bytes_total` from `remote->size()`. `expected_sha256` is the hex digest
  payload from the image config; empty disables verification (logged as a
  warning).
- `src/source/downloader.hpp::Downloader::start` — spawns the download
  coroutine (`elio::go`) and returns immediately. **Requires a running Elio
  scheduler on the calling thread.**
- Lifecycle: `kWaiting` (start delay `delay_sec + random(0,
  delay_extra_sec)`) → `kDownloading` → `kVerifying` → `kDone`, or `kFailed`
  after `try_count` attempts. Each attempt opens
  `src/source/downloader.hpp::Downloader::staging_path`
  (`O_RDWR | O_CREAT`, mode 0644), truncates it sparse to the blob size,
  resumes from the first `SEEK_HOLE` hole, then copies `block_size` chunks
  under a per-second `max_mbps` MiB token window. A sha256 mismatch (or any
  verification failure) discards the staging file and restarts from scratch
  after a 1 s pause; `last_error` records the most recent errno.
- On success the staging file is atomically `rename()`d to
  `src/source/downloader.hpp::Downloader::target_path`, status becomes
  `kDone`, and `on_complete(target_path)` is `co_await`ed on the scheduler.
  The callback runs before `run()` finishes; it must not outlive the
  scheduler.
- `status()`, `last_error()`, `bytes_done()`, `bytes_total()` are relaxed
  atomic snapshots — safe to poll from any thread (the supervisor/obdctl
  status path does so).
- The download is best-effort: `kFailed` never fails reads (the read path
  keeps using the remote source). There is no cancellation API; the
  downloader object must be kept alive until `kDone`/`kFailed` (the owning
  `SwitchSource` holds it in a `shared_ptr`).

### `switch_source.hpp` — SwitchSource

```cpp
class SwitchSource final : public BlobSource {
public:
    static elio::coro::task<std::unique_ptr<SwitchSource>> open(
        BlobSourcePtr remote, BlobSourcePtr raw_remote, std::string dir,
        std::string expected_sha256, const DownloadConfig& dl_cfg);
    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;
    uint64_t size() const noexcept override;
    std::string_view label() const noexcept override;
    bool switched() const noexcept;
    Downloader* downloader() const noexcept;
    struct Control { std::atomic<std::shared_ptr<BlobSource>> local; };
};
```

`src/source/switch_source.hpp::SwitchSource` — the atomic remote→local
switch of a blob's read path (overlaybd `SwitchFile`, simplified to a
whole-file switch per `docs/design-assumptions.md` §S-4). (Not wired by
image assembly since ADR-0011 part 2; see Concepts §"Background download
and the remote→local switch".)

- `open(remote, raw_remote, dir, expected_sha256, dl_cfg)` — `remote` is the
  read path used until the switch (typically the chunk-cached
  registry/DART source); `raw_remote` is the un-cached source the downloader
  reads from. Throws `obd::error(EINVAL)` on a null `remote`, or on
  `dl_cfg.enable` with a null `raw_remote`.
  - If `<dir>/overlaybd.commit` already exists (downloaded by a previous
    run), it is opened immediately, reads bind to it, and **no download is
    started**.
  - Else, when `dl_cfg.enable` is set, the background download starts
    immediately (requires a running scheduler; see `Downloader::start`).
- `pread` — one atomic load per request: serves from the local file once
  switched, else from `remote`. Never throws; errors propagate from
  whichever source served the read.
- `src/source/switch_source.hpp::SwitchSource::switched` — true once reads
  are served locally. `src/source/switch_source.hpp::SwitchSource::downloader`
  — the owned downloader (or null), for status reporting.
- The `Control` block is shared with the downloader's completion coroutine
  via `shared_ptr`, so the switch can land even if the `SwitchSource` itself
  has been released by then; a failed local open is logged and non-fatal
  (reads keep working over the remote). Public only for the completion
  helper — not part of the module API.

### `layer_store.hpp` — LayerStore

```cpp
class LayerStore final : public BlobSource {
public:
    struct Config {
        uint32_t extent_size = 64 * 1024;
        uint64_t queue_max_bytes = 4ULL * 1024 * 1024;
        uint32_t try_count = 5;
    };
    enum class State : int { Filling = 0, Complete = 1, Bypass = 2 };
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
  thread). Otherwise it scans `dir` for a `.download.X`/`.bitmap.X` pair
  whose sidecar header (magic, version, extent size, blob size, extent
  count, layer sha256, nonce, header CRC32) matches this layer; a valid
  pair's records are loaded and filling resumes, anything unpaired or
  invalid is deleted and the layer starts fresh with a new random nonce.
  Filesystem probing and staging-pair creation run through
  `elio::spawn_blocking`, so setup disk I/O does not occupy an Elio worker.
  A zero-length layer starts its writer in the completion check and is
  committed immediately.
  Throws `obd::error` on unrecoverable setup problems (null remote, bad
  config, missing/unwritable `dir`, unloadable commit file, staging-pair
  creation failure).
- `pread` — splits the request into extents. In `Complete` state: plain
  positional reads from the commit file. In `Filling` state, a present
  extent is read locally and CRC32-verified (the tail extent over its actual
  length); a mismatch clears the bit (memory and sidecar), counts
  `crc_failures`, and re-fetches. A missing extent (or any extent in
  `Bypass`) is fetched whole from the remote with in-flight coalescing —
  concurrent readers of the same missing extent join one fetch
  (`coalesced_joins`) — and the fetched bytes are enqueued for write-behind
  (not in `Bypass`). Returns the clamped count, or a negative `-errno` /
  `-EIO` on remote error/short fill (matching ChunkCache). Never throws.
- `src/source/layer_store.hpp::LayerStore::populate` — warms every missing
  extent in `[offset, offset+len)` through the same coalesced fetch and
  write-behind path without delivering data; returns 0 or a negative
  `-errno`. No-op (0) in `Complete` and `Bypass`.
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
  the pair and restarts fresh with a new nonce, bounded by
  `Config::try_count`; exhaustion leaves the store serving remotely in
  `Bypass` (logged as an error).
- Observability — `state()` (`src/source/layer_store.hpp::LayerStore::State`),
  `extents_present()`/`extents_total()`, `dropped_writes()`,
  `crc_failures()`, `remote_fetches()`, `coalesced_joins()` are relaxed
  atomic snapshots, safe to poll from any thread.
- `src/source/layer_store.hpp::LayerStore::set_test_write_hook` — test-only
  hook invoked by the writer thread before persisting each entry; a non-zero
  return is treated as a pwrite failure with that errno. Not part of the
  module API.

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
- **ChunkCache correctness under races** — concurrent fills of the same
  chunk are harmless (the data is immutable; last insert wins) and every
  reader serves correct bytes; the cache never serves partial chunks.
- **Switch atomicity** — each `SwitchSource::pread` is served entirely by
  the remote or entirely by the local copy; the flip is a single atomic
  pointer load, so no read can observe a torn mix of the two.
- **Download integrity** — `overlaybd.commit` only ever appears via an
  atomic `rename()` after a successful sha256 verification (when a digest is
  configured); a `.download` staging file is never served to readers. This
  holds for both the Downloader and LayerStore (whose staging file is
  per-extent CRC-verified before any byte of it is served).
- **LayerStore crash rule** — a sidecar bit is set only after the extent's
  data write completed, and every local read verifies the extent's CRC32:
  a crash or torn write degrades to a re-fetch, never to bad data.
- **Registry byte-exactness** — `RegistryClient::get_data` returns `count`
  or an error, never a short count; a server short body maps to `-EIO`.

## Concurrency & Call Permissions

- **Scheduler context** — every `open`, `pread`, `pwrite`, `flush`,
  `populate`, `get_data`, `get_length`, `dart_proxy_reachable`, and the
  downloader coroutine must run on a thread with a running Elio scheduler.
  `start()` additionally requires it at call time (`elio::go`).
  `CredentialStore` and the base64 helpers are synchronous and
  scheduler-free. LayerStore's blocking disk writes (and its completion
  read-back) run on its own dedicated plain `std::thread`, never on an Elio
  worker — the same precedent as the ublk queue threads.
- **Concurrent reads** — concurrent `pread`s on one source are safe for
  every type in this module. `LocalFileSource` and `RegistrySource` are
  stateless per read; `ChunkCache` serializes its index under an
  `elio::sync::mutex` and fills outside the lock; `SwitchSource` reads an
  atomic pointer; `RegistryClient`'s caches are mutex-guarded and its token
  exchanges are single-flight per key (concurrent re-auths share one
  exchange, ADR-0015); `LayerStore`
  shares its extent map with the writer thread through per-extent atomics
  and coalesces concurrent fetches of one extent to a single remote read.
- **Instance state** — mutable state per instance: `ChunkCache` (chunk map,
  LRU, counters), `RegistryClient` (token and URL-info caches), `Downloader`
  (status atomics), `SwitchSource::Control` (the atomic local pointer),
  `LayerStore` (extent map, write-behind queue, state/counter atomics). No
  hidden global mutable state anywhere in the module.
- **Buffer ownership** — `buf` arguments are borrowed for the duration of
  the `co_await`; sources never retain pointers into caller buffers. Cached
  chunk data is held by the cache as `shared_ptr<const …>`.
- **Object lifetimes** — sources are owned via `BlobSourcePtr`
  (`unique_ptr`); wrapping takes ownership. A `LocalFileSource` may be
  destroyed with reads in flight (the destructor orders the fd close against
  the io_uring backend). A `Downloader` must outlive its coroutine — keep
  the owning `SwitchSource` (or an explicit `shared_ptr<Downloader>`) alive
  until `kDone`/`kFailed`. A `LayerStore` must not be destroyed while
  `pread`/`populate` coroutines are in flight on it (a suspended fetch or
  joiner touches members on resume).
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
  on top of that wire behavior is ours (ADR-0015): token lifetime is 80%
  of the server's `expires_in` (30 s fallback) with single-flight,
  generation-counted re-auth; the redirect/URL-info cache keeps its fixed
  300 s. Registries and CDNs that work with overlaybd must keep working
  here (`docs/design-assumptions.md` §S-3).
- **The DART integration shape** (ADR-0005): DART stays an external process
  reached by prefix passthrough (`base + "/" + full upstream URL`, embedded
  scheme preserved); in-process P2P is rejected. Enabled-but-unreachable
  DART must keep falling back to direct registry reads — a device must never
  fail to open because the accelerator is down.
- **On-disk download artifacts**: staging name `.download`, target name
  `overlaybd.commit`, sparse-truncate + `SEEK_HOLE` resume, sha256-verify +
  atomic rename. Other tools (and earlier runs of this project) rely on
  these names and states.
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
`tests/unit/test_layer_store.cpp`, and
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
- `source: chunk cache serves repeats from memory` — a repeated range read
  issues no additional inner reads (chunk hits), byte content is exact, and
  the hit counter advances.
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
- `integration: layered stack stages over a mock registry` — the manual
  composition RegistrySource → ChunkCache → TarOffsetSource → ZFile → LSMT
  merge reads the original content byte-exactly (a component-composition
  test; image assembly itself now wires RegistrySource → LayerStore,
  ADR-0011).
- `integration: cancelled connect probe does not break later io` — a DART
  probe against a dead port returns `false` quickly and subsequent registry
  IO on the same scheduler is unaffected (no leaked cancellation state).
- `integration: enabled-but-unreachable DART falls back to direct reads` —
  with `p2pConfig` enabled but nothing listening, image assembly still opens
  and serves the full image directly from the registry (ADR-0005 optional
  accelerator).
- `integration: downloader writes, verifies and installs the blob` — the
  downloader reaches `kDone`, `bytes_done == size`, and
  `<dir>/overlaybd.commit` contains the blob byte-exactly after sha256
  verification and rename.
- `integration: switch source swaps reads to the local copy` — reads start
  on the remote (`!switched()`), the switch flips after the download
  completes, and post-switch reads serve byte-exact content from the local
  copy.
- `integration: image assembly serves remote reads through the layer store` —
  image assembly wires `RegistrySource → LayerStore` for a remote lower
  and serves reads byte-exactly while persisting read-through state into
  the layer dir (ADR-0011 part 2).
- `integration: layer store restart serves warmed extents without remote reads` —
  a partially-warmed staging pair survives an assembly restart: the second
  open serves the warmed reads locally (mock remote-read counter flat).
- `integration: completed layer store commit binds read-only without remote reads` —
  a store driven to completion installs `overlaybd.commit`, which the next
  assembly binds locally with zero remote data reads.

No golden external files: blob contents are deterministic patterned bytes
generated in-test; the mock registry implements the RFC 7233 / OCI subset
directly.

## Limitations & TODO

- **No background fill yet (ADR-0011 part 3).** The `LayerStore` warms
  read-through only: extents nobody reads stay remote until they are read.
  Background whole-blob fill (the retired Downloader's role) returns in
  the part-3 follow-up as LayerStore fill driven through `populate`.
- **ChunkCache is memory-only and per-device.** It loses its content on
  restart. Since ADR-0011 part 2, image assembly uses it only on the
  no-dir exception path (a remote layer without `lower.dir`);
  dir-configured layers are persisted by the sparse-file LayerStore, with
  the kernel page cache as the L1. It remains as a composable component
  until part 3 decides its removal. DART, when enabled, is the shared
  on-node cache.
- **Downloader/SwitchSource are unused by assembly.** The whole-file
  switch never migrated extents incrementally
  (`docs/design-assumptions.md` §S-4); ADR-0011 made locality gradual per
  extent and retired the pair from image assembly in part 2. They remain
  as composable components until part 3.
- **Single-request size bound** — one registry Range read is bounded by
  `RegistryClientConfig::max_response_size` (64 MiB default); larger single
  requests must be split by the caller (the format readers already read in
  bounded blocks).
- **Redirect cache lifetime is fixed** — the redirect/URL-info cache
  lifetime is a 300 s constant, not config (the token cache honors
  `expires_in`, ADR-0015; redirect responses carry no comparable declared
  lifetime). Registries issuing shorter-lived redirect targets rely on the
  401-drop-and-re-resolve path.
- **Prefetch is trace-replay only** — overlaybd's dynamic prefetcher and
  TurboOCI paths are out of scope (ADR-0007). The upstream-compatible
  trace blob IS replayed through `populate` (ADR-0013, proposed — the
  trace layer is recognized in image assembly; see `docs/image.md`);
  trace recording, the dynamic file-list fallback, and the B-phase
  admission funnel (ADR-0012) remain open.
- **credentialConfig mode=file only** — inline/secret credential modes are
  ignored (see `docs/image.md` / `docs/config.md`).
- **Downloader has no cancellation** — a running download finishes or fails
  on its own; device teardown relies on process exit (per-device isolation,
  ADR-0004). `delay_extra_sec` uses `std::random_device` per download, so
  delays are not reproducible run-to-run.
- **Redirect caching trusts `Location` for 300 s** — a CDN URL that expires
  sooner surfaces as a re-resolution after a 401/403; other 4xx/5xx
  responses on a stale redirect map to their errno without extending the
  retry budget.
- **DART probing happens once at image open** — a proxy that comes up later
  is not picked up until reopen; conversely a proxy that dies after a
  successful probe surfaces as per-request errors (no mid-image re-probe).
