# trace-format — upstream OverlayBD prefetch trace blob (wire specification)

Byte-exact specification of the **prefetch trace blob** produced and
consumed by upstream OverlayBD's static trace prefetcher. This is a
*specification of an upstream format*: overlaybd-elio implements the
dependency-free codec (`src/format/trace.hpp`); record/replay against
live devices is not wired yet — the governing decision is the proposed
[ADR-0013](./adr/0013-trace-prefetch.md) (record/replay prefetch traces in
the upstream format with a dependency-free codec; this document is its
step 0, "specification before code"). Shape: `policy` (see
`docs/README.md`).

Every byte-layout claim below cites its upstream source as
*(repo `path`, `symbol`)*. Sources were read at branch `main` of the two
upstream repositories (fetched 2026-09; upstream has no format version
field, so "current `main`" is the only version anchor that exists):

- `containerd/overlaybd`: `src/prefetch.h`, `src/prefetch.cpp`,
  `src/image_file.cpp`, `src/test/trace_test.cpp`,
  `src/overlaybd/zfile/crc32/crc32c.h`, `src/overlaybd/zfile/crc32/crc32c.cpp`
- `containerd/accelerated-container-image`: `cmd/ctr/record_trace.go`,
  `pkg/convertor/convertor.go`, `pkg/label/label.go`,
  `pkg/snapshot/overlay.go`, `pkg/snapshot/storage.go`,
  `docs/trace-prefetch.md`

## 1. Serialization model (what the format actually is)

The trace blob is **not** produced by photon's RPC serializer or any other
serialization library. The writer dumps two C++ structs to a file with
plain `write()`, and the reader `read()`s them back into the same structs
(overlaybd `src/prefetch.cpp`, `PrefetcherImpl::dump` and
`PrefetcherImpl::reload`). Consequently the format is a **raw struct
image** and inherits the host C++ ABI:

- **Byte order: little-endian.** Upstream's supported build targets are
  x86-64 and AArch64 Linux — the only architectures with code paths in
  the checksum translation unit (`src/overlaybd/zfile/crc32/crc32c.cpp`,
  `crc_init`) — and both are little-endian. The format has no byte-order
  marker and no endianness conversion anywhere.
- **Word size: LP64.** `size_t` and `off_t` are 8 bytes each on those
  targets (64-bit Linux, large-file offsets). There is no 32-bit or
  big-endian encoding. The LP64 little-endian layout is the **documented
  scope of this specification**: it describes the format as produced on
  upstream's supported builds; any other ABI would emit a different byte
  image and would need its own verification.
- **Alignment/padding: the platform ABI's natural struct layout**
  (verified empirically with GCC on x86-64: `sizeof` = 24 for both
  structs, offsets as tabulated below). Padding bytes are part of the
  byte stream and are **covered by the checksum** (§4).

The struct definitions that define the wire format (overlaybd
`src/prefetch.cpp`, `PrefetcherImpl::TraceHeader` /
`PrefetcherImpl::TraceFormat`; `TraceOp` in `src/prefetch.h`,
`Prefetcher::TraceOp`):

```cpp
enum class TraceOp : char { READ = 'R', WRITE = 'W' };   // 1 byte

struct TraceFormat {       // 24 bytes on LP64
    TraceOp  op;           // offset 0,  1 byte  (+3 padding)
    uint32_t layer_index;  // offset 4,  4 bytes
    size_t   count;        // offset 8,  8 bytes (unsigned)
    off_t    offset;       // offset 16, 8 bytes (signed)
};

struct TraceHeader {       // 24 bytes on LP64
    uint32_t magic;        // offset 0,  4 bytes (+4 padding)
    size_t   data_size;    // offset 8,  8 bytes
    uint32_t checksum;     // offset 16, 4 bytes (+4 padding)
};
```

## 2. Blob layout

A trace blob is a 24-byte header followed by a contiguous record stream.
There is **no trailer**: the checksum lives in the header (the writer
first writes the header with `checksum = 0`, appends all records, then
rewrites the header in place with the final checksum — `PrefetcherImpl::dump`).

```
offset  size  content
0       24    TraceHeader
24      24    record 0 (TraceFormat)
48      24    record 1
...     ...   ...
24+24N  —     end of blob (N = data_size / 24)
```

Total blob size = `24 + data_size` bytes; the reader enforces this
**exactly** (§7).

### 2.1 Header fields

| Offset | Size | Type | LE bytes | Meaning |
|---|---|---|---|---|
| 0 | 4 | u32 | `20 18 EF C2` | `magic` = 3270449184 (0xC2EF1820). |
| 4 | 4 | padding | any | Struct alignment padding; **not** read as a field. |
| 8 | 8 | u64 | — | `data_size` = number of record bytes = `24 × record_count`. |
| 16 | 4 | u32 | — | `checksum` = CRC-32C over the whole record stream (§4). |
| 20 | 4 | padding | any | Struct tail padding; **not** read as a field. |

- `magic`: constant `TRACE_MAGIC = 3270449184` with the upstream comment
  "CRC32 of `Container Image Trace Format`" (overlaybd `src/prefetch.cpp`,
  `PrefetcherImpl::TRACE_MAGIC`). Verified independently: 3270449184 is
  exactly the **IEEE CRC-32** (zlib `crc32`, polynomial 0x04C11DB7
  reflected, init/xorout 0xFFFFFFFF) of the 28 ASCII bytes
  `Container Image Trace Format` (no NUL terminator). Note this is the
  IEEE flavor, *not* the CRC-32C used for the record checksum — the two
  CRCs in this format use different polynomials.
- `data_size`: byte count of the record stream only (header excluded);
  set to `sizeof(TraceFormat) * m_record_array.size()` by the writer
  (`PrefetcherImpl::dump`).
- `checksum`: see §4. Written as 0 in the first header pass and patched
  to the final value afterwards (`PrefetcherImpl::dump`).

### 2.2 Record fields

Each record describes one read I/O against one layer blob, from the layer
blob's own byte-space perspective (overlaybd `src/image_file.cpp`,
`ImageFile::open_lower_layer` wraps each per-layer blob file with
`PrefetchFile`; `src/prefetch.cpp`, `PrefetchFile::pread` records the raw
`pread` offset/count on that file).

| Offset | Size | Type | Meaning |
|---|---|---|---|
| 0 | 1 | char | `op`: `0x52` (`'R'`, READ) or `0x57` (`'W'`, WRITE). |
| 1 | 3 | padding | Struct alignment padding; checksummed, never interpreted. |
| 4 | 4 | u32 | `layer_index`: 0-based index into the image's lower-layer list. |
| 8 | 8 | u64 | `count`: read length in bytes. |
| 16 | 8 | i64 | `offset`: byte offset within the layer blob file. |

Semantics and practical ranges:

- **`op`.** The enum defines READ and WRITE (`src/prefetch.h`,
  `Prefetcher::TraceOp`), but the only recording call site
  (`PrefetchFile::pread`) always emits READ; no code path ever records a
  WRITE. At replay, records whose `op` is not READ are popped and
  **silently skipped** (`PrefetcherImpl::replay_worker_thread` executes
  the prefetch `pread` only under `trace.op == TraceOp::READ`). The
  reader performs **no validation** of the `op` byte: any byte value is
  accepted by the parser and ignored unless it is `0x52`.
- **`layer_index`.** Position of the layer in the image's `lowers`
  array, counted after the trace (acceleration) layer itself has been
  popped (`src/image_file.cpp`, `ImageFile::init_image_file`). At replay,
  an index with no registered source file is silently skipped
  (`PrefetcherImpl::replay_worker_thread`, `m_src_files.find` miss →
  `continue`). No range validation at parse time.
- **`count`.** Whatever length the recorded `pread` carried; no
  alignment is required or enforced. See §8 for the 1 MiB replay-buffer
  hazard.
- **`offset`.** Signed 64-bit (`off_t`); in practice non-negative. No
  validation at parse or replay time.
- **No timestamps, no sequence numbers, no per-record sizes.** Records
  are fixed 24 bytes; ordering is purely positional (§5).

## 3. Magic and format identification inside the blob

The magic is the blob's only self-identifying field — there is no version
number, no flags word, no record-type tag beyond `op`. Two upstream
consumers check it, with different consequences (§7):

- `new_prefetcher` (overlaybd `src/prefetch.cpp`) reads the first 24
  bytes; a size-0 file selects **Record** mode, a matching magic selects
  the static trace **Replay** path, and *any other non-empty content*
  silently selects the **dynamic prefetcher** (a line-based text file
  list — `DynamicPrefetcher`). A trace blob whose first 4 bytes are
  corrupted is therefore not rejected; it is reinterpreted as a (usually
  garbage) file list.
- `PrefetcherImpl::reload` hard-rejects a magic mismatch
  (`LOG_ERROR_RETURN`, "trace magic mismatch") — but this path is only
  reached after `new_prefetcher` already matched the magic.

## 4. Checksum (record stream)

`hdr.checksum` is a running CRC over the **entire record stream** —
`sizeof(TraceFormat)` = 24 bytes per record, **including the 3 padding
bytes** — computed as (overlaybd `src/prefetch.cpp`,
`PrefetcherImpl::dump` / `PrefetcherImpl::reload`):

```
crc := 0
for each record (in file order):
    crc := crc32c_extend(record_bytes[0..24], crc)
```

`crc32c_extend` (overlaybd `src/overlaybd/zfile/crc32/crc32c.cpp`,
`crc32::crc32c_extend`) is **CRC-32C (Castagnoli)** — reflected,
polynomial 0x1EDC6F41 (reflected form 0x82F63B78) — in **raw-register
chaining form**: the state is initialized to 0, each byte updates it as
`state = (state >> 8) ^ T[(state ^ b) & 0xFF]`, the state is carried
verbatim across calls, and there is **no initial complement and no final
complement**. This matches the SSE4.2 `CRC32` instruction chain seeded
with 0 (the hardware path `crc32c_hw`) and the bundled table-driven
software path (`singletable_crc32c` / `multitable_crc32c`), which agree
bit-for-bit.

**This is not the catalogue "CRC-32/ISCSI" convention** (init =
0xFFFFFFFF, xorout = 0xFFFFFFFF, check value 0xE3069283 for
`123456789`). The two are related by
`iscsi(data) = raw(data, seed=0xFFFFFFFF) ^ 0xFFFFFFFF`; a codec using a
library CRC-32C must pass the running state without complementing at call
boundaries (or equivalently seed with `~state` and complement each
result).

Test vectors (raw-chaining convention, little-endian record bytes with
zero padding):

| Input | Result |
|---|---|
| empty stream (0 records) | `0x00000000` |
| one record: op `'R'`, layer_index 1, count 4096, offset 0 | `0xBA691A13` |
| that record, then: op `'R'`, layer_index 0, count 8192, offset 16384 | `0xD29283DD` |

The second vector's record bytes are
`52 00 00 00 01 00 00 00 00 10 00 00 00 00 00 00 00 00 00 00 00 00 00 00`
and
`52 00 00 00 00 00 00 00 00 20 00 00 00 00 00 00 00 40 00 00 00 00 00 00`;
the result equals the one-shot CRC over the 48-byte concatenation
(chaining property, verified).

**Build-variant caveat (upstream quirk).** The optional `ENABLE_DSA`
code path in `crc32c.cpp` (`crc32c_dml`) complements the seed and result
(`crc_seed = ~crc`, `return result.crc_value ^ 0xFFFFFFFF`) and therefore
produces *different* values than the default SSE4.2/software paths.
DSA is not enabled in upstream's default build, but a DSA-built recorder
and a non-DSA replayer are mutually incompatible. This specification
standardizes the default (raw-chaining) behavior.

## 5. Record granularity and ordering

- **Granularity:** one record per *fully satisfied* `pread` on a
  prefetch-wrapped layer file during recording — short reads
  (`n_read != count`) are not recorded (`src/prefetch.cpp`,
  `PrefetchFile::pread`). There is no coalescing, splitting, or
  deduplication on the static record path; offsets/counts are
  unaligned in general.
- **Ordering:** the writer appends records to an in-memory vector in
  arrival order and dumps them in that order (`PrefetcherImpl::record`,
  `PrefetcherImpl::dump`), so file order is the recording-time
  chronological order. (Upstream appends without a lock from multiple IO
  threads, so exact interleaving of concurrent reads is a race — the
  *intent* is chronological.) At replay the queue is drained by
  `concurrency` worker threads (`PrefetcherImpl::do_replay`), so
  **execution order is deliberately not preserved**. Order is therefore
  present in the format but is not load-bearing for upstream replay
  correctness; ADR-0013's "replay in recorded order" is this project's
  stricter choice, not an upstream guarantee.
- **Replay effect:** each READ record triggers one `pread` of `count`
  bytes at `offset` on the registered source file for `layer_index`,
  purely to warm the cache; a short/failed replay read is logged and
  skipped (`PrefetcherImpl::replay_worker_thread`). WRITE records and
  unknown layer indexes are no-ops.

## 6. Trace-layer identification (image-level packaging)

The trace blob does not travel alone; it is packaged as an ordinary
**uncompressed OCI tar layer** that MUST be the **uppermost** layer of the
image manifest (accelerated-container-image `docs/trace-prefetch.md`:
"Both priority list and I/O trace are stored as an independent image
layer, and MUST always be the uppermost one"). The packaging chain, end
to end:

1. **Recording.** `ctr record-trace` (`cmd/ctr/record_trace.go`,
   `recordTraceCommand`) runs a temporary container whose snapshot
   carries the labels `containerd.io/snapshot/overlaybd/record-trace=yes`
   and `containerd.io/snapshot/overlaybd/record-trace-path=<file>`
   (`pkg/label/label.go`, `RecordTrace` / `RecordTracePath`). The
   backstore records into that file and dumps the blob when the
   `<file>.lock` file disappears, then signals completion by creating
   `<file>.ok` (overlaybd `src/prefetch.h` header comment;
   `PrefetcherImpl::dump`; collection loop `collectTrace` in
   `record_trace.go`). The same labels can trigger record/replay without
   `ctr` via `--snapshotter-label` (`docs/trace-prefetch.md`).
2. **Packaging.** The trace file is loaded into the content store by
   `NewContentLoaderWithFsType(true, fsType, ContentFile{SrcFilePath:
   traceFile, DstFileName: "trace"})` (`record_trace.go`), which writes a
   tar stream containing **exactly one regular file named `trace`**
   (mode 0444, `pkg/convertor/convertor.go`, `contentLoader.Load`) and
   produces a layer descriptor with:
   - media type `application/vnd.oci.image.layer.v1.tar`
     (`ocispec.MediaTypeImageLayer` — uncompressed tar);
   - annotation `containerd.io/snapshot/overlaybd/acceleration-layer:
     "yes"` (`label.AccelerationLayer`);
   - annotations `containerd.io/snapshot/overlaybd/blob-digest` and
     `.../blob-size` (the tar blob's own digest/size), and
     `.../blob-fs-type` when known;
   - annotation `containerd.io/snapshot/overlaybd/version`
     (`label.OverlayBDVersion`), set unconditionally by the convertor and
     ignored by the backstore's trace handling.
   The new manifest appends this layer as the last entry
   (`createImageWithAccelLayer` in `record_trace.go`).
   (`record_trace.go` also defines an unused constant
   `traceNameInLayer = ".trace"` — dead code; the actual tar member name
   and the name the backstore looks up is `trace`, no dot.)
3. **Pull-time recognition.** containerd propagates layer-descriptor
   annotations carrying the `containerd.io/snapshot/` prefix onto
   snapshot labels at unpack; the overlaybd snapshotter observes
   `label.AccelerationLayer == "yes"` on the prepared snapshot and treats
   it as the acceleration layer — it is downloaded and tar-extracted like
   any layer, "Neither image manifest nor container snapshotter needs to
   know if it is a trace layer" (`docs/trace-prefetch.md`;
   `pkg/snapshot/overlay.go`, `Prepare`). The snapshotter writes the
   backstore config (`config.v1.json`) with `accelerationLayer: true`
   and the acceleration layer's extracted directory appended as the last
   lower (`pkg/snapshot/storage.go`, `updateSpec` /
   `constructSpecForAccelLayer`).
4. **Backstore recognition.** On device bring-up the backstore reads its
   config (overlaybd `src/image_file.cpp`,
   `ImageFile::init_image_file`): when `conf.accelerationLayer()` is set
   and lowers are non-empty, it **pops the last lower**, looks for
   `<accel_layer_dir>/trace`, and — if that file exists and is non-empty
   (`Prefetcher::detect_mode` = Replay) — constructs the prefetcher on
   it. `new_prefetcher` then applies the §3 magic test: magic match →
   static trace replay (this format); otherwise → dynamic file-list
   mode. A `recordTracePath` set together with an acceleration layer is
   a hard configuration error.

A backstore conforming to this contract therefore recognizes a trace
layer by: **uppermost layer + annotation
`containerd.io/snapshot/overlaybd/acceleration-layer = "yes"` →
extracted member file `trace` → magic `20 18 EF C2` at offset 0**. The
annotation marks the layer's *role*; the magic marks the blob's
*format* (the same layer packaging can carry a text priority list, which
takes the dynamic-prefetch path instead).

## 7. Parser acceptance rules (versioning)

The format has **no version field** and no extension mechanism; upstream's
parser accepts exactly one layout. Acceptance, in check order
(`PrefetcherImpl::reload`, reached in Replay mode after the §3 magic
routing; note `PrefetcherImpl`'s constructor **ignores** `reload`'s
return value, so every rejection below degrades to "prefetch disabled",
never to a device-bring-up failure):

1. **Header readable**: first `read` must return exactly 24 bytes, else
   error ("reload header failed").
2. **Magic**: `hdr.magic == 3270449184`, else error ("trace magic
   mismatch").
3. **Exact size**: `file_size == hdr.data_size + 24`, else error ("trace
   file size mismatch"). Truncated *and* padded blobs are both rejected.
4. **Record stream**: `floor(data_size / 24)` records are read, 24 bytes
   each; a short read errors ("reload content failed"). (If `data_size`
   is not a multiple of 24 but rule 3 passes, the trailing 1–23 bytes are
   never read and not checksummed — never produced by the upstream
   writer; see §8.)
5. **Checksum**: the §4 running CRC over the records as read must equal
   `hdr.checksum`; on mismatch the loaded queue is **cleared** and reload
   errors ("reload checksum error").

No record field (`op`, `layer_index`, `count`, `offset`, padding) is
validated at parse time; all semantics are deferred to replay, where
non-READ ops and unknown layer indexes are silently skipped. Unknown
"versions" do not exist: any blob that fails rules 1–3 at the
`new_prefetcher` routing stage is not an error at all — it is
reinterpreted as a dynamic-prefetch text file list (§3).

## 8. Edge cases

- **Empty file (0 bytes).** Not a trace blob at all: `detect_mode`
  selects **Record** mode (`src/prefetch.cpp`, `Prefetcher::detect_mode`).
- **Empty trace (header only, `data_size = 0`, `checksum = 0`).** A
  valid 24-byte blob: passes all §7 checks, loads zero records, replay is
  a no-op (`PrefetcherImpl::replay` returns 0 on an empty queue). This is
  what a recording with no full reads produces.
- **Truncated blob / trailing garbage.** Rejected by the exact-size rule
  (§7 rule 3); prefetch silently disabled.
- **Checksum mismatch.** Record queue cleared; prefetch silently
  disabled (§7 rule 5).
- **Short read mid-stream** (file shrinks between `stat` and read — the
  size check passed first). `reload` errors, but records already pushed
  **stay in the queue** and will be partially replayed: only the
  checksum-mismatch path clears the queue. A strict third-party reader
  should reject the whole blob instead (ADR-0013's reader must not
  reproduce this partial-replay behavior).
- **`data_size` not a multiple of 24.** Accepted if the total size
  matches; the final 1–23 bytes are ignored and unchecksummed (§7 rule
  4). Upstream writers never produce this.
- **`op` byte other than `0x52`** (including `0x57` WRITE and arbitrary
  values): parsed fine, skipped at replay.
- **`layer_index` with no registered layer**: skipped at replay.
- **`count > 1 MiB` hazard.** Replay allocates a fixed
  `MAX_IO_SIZE = 1024*1024` buffer and issues `pread(buf, trace.count,
  ...)` unconditionally (`PrefetcherImpl::replay_worker_thread`,
  `PrefetcherImpl::MAX_IO_SIZE`): a record with `count > 1048576`
  **overflows that buffer** in upstream. The static recorder can in
  principle emit such counts (it logs raw pread lengths); the dynamic
  prefetcher slices its own synthetic records at 1 MiB
  (`DynamicPrefetcher::get_extents`). A conforming writer MUST cap
  `count ≤ 1048576`, splitting larger ranges into consecutive records.
- **Padding bytes.** The writer's record struct is aggregate-initialized
  (`TraceFormat trace = {op, layer_index, count, offset};`), whose
  padding-byte values are **not guaranteed by C++** — empirically zero at
  GCC `-O2`, but observed non-zero (0x02 in byte 1) at `-O0`. Upstream
  trace files may therefore contain arbitrary padding bytes; the
  checksum keeps them self-consistent because the reader recomputes the
  CRC over the bytes as stored. Readers MUST include padding bytes in
  the checksum and MUST NOT require them to be zero; writers SHOULD emit
  zero padding (see §10).
- **Corrupt magic.** Not a parse error: the blob is reinterpreted as a
  dynamic-prefetch file list (§3), normally a harmless no-op.

## 9. Load-bearing vs ignorable

Replay correctness = "the recorded ranges are fetched from the right
layer blobs". Nothing in the blob gates device bring-up.

| Element | Status | Why |
|---|---|---|
| `magic` | **Load-bearing** | Selects static-trace parsing at all (§3). |
| `data_size` | **Load-bearing** | Frames the record stream; exact-size check. |
| `checksum` | **Load-bearing (as a gate)** | Mismatch discards the whole trace. Its *value* carries no semantics beyond integrity. |
| `op` | **Semi-load-bearing** | Only `0x52` acts; everything else is a skip. A writer should emit only `'R'`. |
| `layer_index` | **Load-bearing** | Selects the target layer blob; wrong indexes silently drop records (replay skips misses). |
| `count`, `offset` | **Load-bearing** | The actual prefetch ranges; also the 1 MiB safety cap (§8). |
| Record order | **Ignorable for correctness** | Upstream replays concurrently without order guarantees; chronological order is an optimization hint only. |
| Header/record padding bytes | **Ignorable values, but checksummed** | Any values accepted; must be included in the CRC. |
| `.lock` / `.ok` side files | **Operational only** | Recording handshake; never part of the blob or the image layer. |

## 10. Minimal writer checklist

Two contracts must not be conflated:

**Parser acceptance** — what upstream's parser actually accepts. Per §7
the *only* gates are: a readable 24-byte header, `magic` = 3270449184,
exact file size (`24 + data_size`), and a matching checksum over the
`floor(data_size / 24)` records. Upstream does **not** parse-reject
oversized `count`s, out-of-range offsets, a non-multiple `data_size`
tail, unknown `op` bytes, or nonzero padding (§8).

**Conforming-writer contract** — what a writer (in particular the
ADR-0013 codec) MUST produce for safe, deterministic interop with
upstream replayers:

1. Exactly 24-byte header, then exactly `24 × N` record bytes, nothing
   else (total size `24 + data_size`); `data_size` = `24 × N` as u64
   little-endian.
2. `magic` = 3270449184 (`20 18 EF C2` little-endian).
3. `checksum` = CRC-32C raw-chaining (seed 0, no complements) over **all
   record bytes as written, padding included**, u64/u32 fields
   little-endian.
4. Each record: `op = 0x52` (`'R'`); `layer_index` within the target
   image's lower count; `1 ≤ count ≤ 1048576` (split larger ranges —
   larger counts are parse-accepted but overflow the upstream replay
   buffer, §8); `offset ≥ 0` and `offset + count` within the layer
   blob's size (out-of-range records replay as logged failures, not
   errors).
5. Emit zero padding bytes (upstream accepts any values, but zeros make
   produced blobs deterministic and match optimized upstream builds).
6. Image packaging, if the blob ships as a layer: single tar member
   named `trace`; layer media type
   `application/vnd.oci.image.layer.v1.tar`; annotation
   `containerd.io/snapshot/overlaybd/acceleration-layer: "yes"`;
   uppermost layer in the manifest (§6).

A blob satisfying the writer contract always satisfies parser
acceptance; the converse is false.

## 11. Codec guidance (dependency-free C++17/20)

ADR-0013 mandates a hand-rolled codec. Mapping this format to one:

**Must reproduce exactly:**

- Fixed 24-byte header and 24-byte records, little-endian, field offsets
  as tabulated in §2. Use explicit offset arithmetic or
  `memcpy` from/into byte buffers — do **not** `reinterpret_cast` a local
  struct and dump it (that would re-import the padding-value lottery of
  §8; a writer must control every byte). C++20
  `std::endian`/`byteswap`-free LE helpers suffice since the format is
  LE-only; on a big-endian host byte swapping would be required (upstream
  has none — such hosts are simply out of scope, matching upstream).
- The CRC-32C raw-chaining checksum (§4), computed over the serialized
  record bytes *including the three padding bytes per record*. A ~20-line
  table-driven implementation (reflected poly 0x82F63B78, byte-at-a-time)
  is sufficient; verify against the §4 test vectors. On SSE4.2/AArch64
  the hardware `crc32` instruction chain seeded with 0 is bit-identical.
- Reader acceptance order mirroring §7: magic → exact size → stream read
  → checksum, with whole-blob rejection on any failure (strictly stronger
  than upstream's partial-queue quirk, and safe because replay is
  opportunistic per ADR-0013).
- Writer checklist §10 (especially the 1 MiB `count` cap and zero
  padding).

**May be simplified:**

- No photon, no varints, no string/vector encodings exist in this format
  — nothing to port.
- A reader may drop the checksum *verification* only behind an explicit
  "accept corrupt traces" policy (upstream never skips it); a writer may
  never drop checksum *generation*.
- A reader may ignore `op` values other than `'R'` at parse time
  (upstream defers the skip to replay; same net effect).
- A reader need not implement the dynamic file-list fallback
  (`DynamicPrefetcher`): that is a different feature (rejected in
  ADR-0013), not part of this wire format. But note §3: magic-mismatch
  blobs take that path upstream, so "not a trace" must degrade to
  "no prefetch", never to an error.
- Concurrency of replay is free: order is not load-bearing (§9).

## 12. Ambiguities and open questions

Points where the upstream code is ambiguous or internally inconsistent,
with the empirical test that would resolve each:

1. **Padding-byte values** (§8). The C++ standard does not pin them and
   builds differ (observed 0x02 at `-O0`, 0x00 at `-O2`). *Resolution
   not needed for interop* (reader recomputes the CRC over stored bytes);
   for byte-identical reproduction of a specific upstream build's
   output, hexdump a real `ctr record-trace` artifact from that build.
2. **`data_size % 24 ≠ 0` tail** (§7 rule 4). Ignored and unchecksummed
   by the reader; unreachable from the writer. Empirical test:
   hand-craft `data_size = 25`, `checksum` over the one full record —
   expect acceptance with the last byte ignored.
3. **DSA build divergence** (§4). `ENABLE_DSA` builds checksum
   differently from default builds. Empirical test: record a trace on a
   DSA-enabled overlaybd and replay on a stock build — expect checksum
   rejection. (Default builds only are in scope here.)
4. **Partial replay after mid-stream short read** (§8). Code reading
   says the partial queue survives; only reachable via a stat/read race.
   Empirical test: truncate the file between `stat` and `read`
   (e.g. via a FUSE shim) and observe replay of the leading records.
   This project deliberately does not reproduce it.
5. **Unused `.trace` constant** (§6 step 2). `record_trace.go` defines
   `traceNameInLayer = ".trace"` but packages the member as `trace`, and
   the backstore opens `<dir>/trace`. The no-dot name is authoritative
   (two independent consumers agree); the constant is dead code.
6. **Whether upstream-produced records can exceed 1 MiB** (§8). The
   recorder logs unfiltered pread lengths; whether real read paths ever
   issue > 1 MiB preads against layer files is workload-dependent.
   Empirical test: record traces of large sequential reads and scan for
   `count > 1048576`. The writer cap in §10 is mandatory regardless.

## 13. Appendix: worked example

A complete, valid 72-byte blob with two records (layer 1: 4096 B at
offset 0; layer 0: 8192 B at offset 16384), zero padding, checksum
0xD29283DD (§4 vectors):

```
0000  20 18 ef c2 00 00 00 00  30 00 00 00 00 00 00 00  |magic| pad  |data_size = 48
0010  dd 83 92 d2 00 00 00 00  52 00 00 00 01 00 00 00  |checksum| pad|op R| pad|layer 1
0020  00 10 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |count = 4096       |offset = 0
0030  52 00 00 00 00 00 00 00  00 20 00 00 00 00 00 00  |op R| pad|layer 0|count = 8192
0040  00 40 00 00 00 00 00 00                           |offset = 16384
```
