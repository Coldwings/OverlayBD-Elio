# format — OverlayBD on-disk formats (ZFile, LSMT, writable layers)

Module document for `src/format/`: readers and fixture writers for the
OverlayBD wire formats, the multi-layer merge, and the writable top layers
(ADR-0008). Shape: `module` (see `docs/README.md`).

## Overview

`src/format/` implements the OverlayBD on-disk formats byte-exactly, so
images built by upstream `overlaybd-*` tools load identically here:

- **ZFile** (`zfile_format.hpp`, `zfile.hpp`, `block_codec.hpp`) — the
  compressed, indexed blob format. `ZFileSource` is a read-only
  decompression view over a `source::BlobSource`, with LZ4 (always) and ZSTD
  (build-optional) raw-block compression, a jump-table index, and optional
  per-block `crc32c_salt` verification.
- **LSMT** (`lsmt_format.hpp`, `lsmt.hpp`) — the log-structured merge tree
  layer format. `LsmtLayer` parses one sealed layer; `MergedLsmt` merges a
  bottom-up stack of layers into the final read-only block view (topmost
  wins, holes read as zeroes).
- **Writable layers** (`writable.hpp`, `sparse_rw.hpp`, `lsmt_rw.hpp`, `hybrid_rw.hpp`,
  `merged_writable.hpp`) — the ADR-0008 writable top of an image stack:
  `SparseRwLayer` (sparse data plus exact persisted coverage, ADR-0024),
  `LsmtRwLayer` (unsealed
  data/index LSMT pair with in-place edit and seal compaction),
  `HybridRwLayer` (persisted mapping log and metadata-only seal, ADR-0026), and
  `MergedWritable` (copy-on-write merged view implementing
  `source::WritableBlobSource`).
- **Cold-path writers** (`writer.hpp`) — synchronous writers that produce
  upstream-readable sealed ZFile and LSMT files for `obd-mkimage`,
  `obd-convert`, and test fixtures. The data plane never writes through these.
- **Trace codec** (`trace.hpp`) — the upstream OverlayBD prefetch trace
  blob (ADR-0013): an in-memory parser and conforming writer for
  the raw-struct wire format specified in
  [trace-format.md](./trace-format.md). Replay is wired into device
  bring-up (`src/image/trace_replay.hpp`, docs/image.md) and recording
  into the supervisor's trace commands (`src/image/trace_record.hpp`,
  docs/supervisor.md).

Everything async in this module is an `elio::coro::task` running on the Elio
scheduler over the `source::BlobSource` abstraction; the writers are plain
blocking POSIX IO for cold paths only.

## Concepts

### ZFile layout

Little-endian throughout (upstream `zfile.cpp` and
`docs/specs/zfile_format_spec.md`):

```
| Header (512B) | dict (optional) | block0 [+crc 4B] | ... | blockN [+crc] |
  index (u32 array) | Trailer (512B) |
```

- **HeaderTrailer** occupies the first 96 bytes of each 512B region; the rest
  is zero padding. Fields: magic0 (8B, `"ZFile\0\1\0"`), magic1 (16B,
  `"tuji.yyf@Alibaba"`), `size` (= 96), `digest` (crc32c over the whole 512B
  region with the digest field zeroed), `flags`, `index_offset`,
  `index_size` (**entry count**; on-disk bytes = `index_size * 4`),
  `original_file_size`, `index_crc`, and 24 bytes of `CompressOptions`.
- **Flags**: bit0 `is_header` (1 header / 0 trailer), bit1 `data_file` (1) /
  `index_file` (0), bit2 `sealed`, bit3 `header_overwrite` (trailer info
  copied back into the header), bit4 `calc_digest`, bit5 `index compressed`.
- **Trailer-authoritative open**: unless the header carries
  `header_overwrite`, the trailer's fields are the effective ones; the
  trailer must be a sealed data file.
- **Index**: a u32 little-endian array of per-block compressed sizes
  (each including the trailing 4-byte CRC when `verify` is on). The in-memory
  **jump table** rebuilds absolute block offsets as partial group bases plus
  u16 prefix sums (`group_size = 65536 / block_size`, so `block_size` must be
  a power of two ≤ 65536).
- **Per-block verify**: when `CompressOptions.verify` is set, every block is
  followed by 4 bytes of `crc32c_salt` (seed 100007, the
  `NOI_WELL_KNOWN_PRIME`; see `docs/common.md`). The reader checks it only
  when the file carries checksums **and** the caller asked for verification
  (upstream: `verify = !is_local`, i.e. remote data).
- **Compression**: raw-block framing (no container headers). Algo ids: 0 =
  MINI_LZO (legacy, unsupported), 1 = LZ4, 2 = ZSTD (only when built with
  `OBD_ENABLE_ZSTD`). Dictionary compression is parsed but rejected at open.

### LSMT layout

Little-endian throughout (upstream `lsmt/file.cpp` and
`docs/specs/lsmt_format_spec.md`):

```
| Header (4096B) | data | index (SegmentMapping[16B] array) | Trailer (4096B) |
```

- **HeaderTrailer** occupies the first 390 bytes of each 4096B region.
  Fields: magic0 (8B, `"LSMT\0\1\2\0"`), magic1 (16B, UUID
  `{0xd2637e65,0x4494,0x4c08,0xd2a2,{0xc8,0xec,0x4f,0xcf,0xae,0x8a}}` as LE
  bytes), `size` (= 390), `flags` (u32), `index_offset` (bytes),
  `index_size` (entry count), `virtual_size` (bytes), `uuid` (37B),
  `parent_uuid` (37B), `version`/`sub_version` (1/1), `user_tag` (256B).
- **Flags**: bit0 `is_header`, bit1 `data_file` / `index_file`, bit2
  `sealed`, bit3 `gc_layer`, bit4 `sparse_rw`, bit5 `hybrid_rw`.
  Hybrid header/trailer flags must agree, use version 1.1, and contain no
  unsupported flags. Hybrid cannot be combined with warp or sparse layout.
  Ordinary LSMT retains header/type/sealed validation.
- **Sector units**: all index offsets/lengths are in 512-byte sectors. The
  data region begins at **sector 8** (byte 4096); a valid `moffset` lies in
  `[8, index_offset/512)`.
- **SegmentMapping** (16 bytes): `offset` 50 bits, `length` 14 bits
  (`<<50`), `moffset` 55 bits, `zeroed` 1 bit (`<<55`), `tag` 8 bits (`<<56`).
  Encoding/decoding lives in `docs/common.md`'s module
  (`src/common/bytes.hpp::segment_mapping`); the `tag` is a runtime field
  assigned at merge time (0 = topmost).

### Layer merge

An image is a stack of sealed LSMT layers. Merging is a recursive gap-fill:
walk the topmost layer's sorted disjoint segments; wherever a segment leaves
a gap in the query range, recurse into the next lower layer for that gap.
The result is a single sorted, disjoint index whose `tag` records the owning
layer (0 = topmost). Reads dispatch per segment to that layer's data source.
Topmost wins; holes (ranges no layer covers) and `zeroed` segments read as
zeroes. The device virtual size is the topmost non-zero layer
`virtual_size`. A sealed layer with an **empty index** (`index_size = 0`,
no data) is valid: every read falls into a hole, so it serves its whole
`virtual_size` as zeroes through the ordinary path — the ADR-0014 blank
(raw) device zero base is exactly such a layer
(`create_empty_lsmt_layer` in src/format/lsmt_rw.hpp).

### Writable layers (ADR-0008)

A writable image has one writable top layer above zero or more sealed RO
lowers. Writes always land in the top (copy-on-write — lowers are never
modified); reads walk a merged index rebuilt after every write. Two top-layer
implementations exist: a sparse file with identity mapping
(`SparseRwLayer`), and an unsealed data/index LSMT pair with in-place edit
(`LsmtRwLayer`), which `seal()` compacts into a standard sealed LSMT RO
file.

### Trace blob (ADR-0013)

The prefetch trace blob is a 24-byte header (magic, `data_size`,
CRC-32C checksum) followed by fixed 24-byte records, an LP64
little-endian raw struct image. The checksum covers record bytes,
including record padding, but not header bytes or header padding. The byte-level authority is
[trace-format.md](./trace-format.md); the codec in `trace.hpp` implements
its parser acceptance rules (§7) and conforming-writer contract (§10)
with no third-party dependencies, reusing the in-repo raw-chaining
CRC-32C (`src/common/crc32c.hpp`).

## Public API

### `src/format/zfile_format.hpp` — `namespace obd::format::zfile`

Constants: `kSpace = 512` (header/trailer region size), `kHeaderSize = 96`,
`kMaxIndexSize = 1000000000` (`MAX_ZFILE_INDEX_SIZE`),
`kNoiWellKnownPrime = 100007`, `src/format/zfile_format.hpp::kMagic0` /
`kMagic1` (the wire magics), flag shifts `kFlagShiftHeader` (0),
`kFlagShiftType` (1), `kFlagShiftSealed` (2), `kFlagShiftHeaderOverwrite`
(3), `kFlagShiftCalcDigest` (4), `kFlagShiftIdxComp` (5).

```cpp
enum Algo : uint8_t { kAlgoMiniLzo = 0, kAlgoLz4 = 1, kAlgoZstd = 2 };
```

`src/format/zfile_format.hpp::CompressOptions`

```cpp
struct CompressOptions {
    uint32_t block_size = 4096;
    uint8_t algo = kAlgoLz4;
    uint8_t level = 0;      // zstd level; lz4 ignores
    uint8_t use_dict = 0;   // parsed, rejected at open
    uint32_t reserved = 0;
    uint32_t dict_size = 0;
    uint8_t verify = 0;     // per-block trailing crc32c_salt

    void parse(const void* p);        // reads 24 bytes
    void serialize(void* p) const;    // writes 24 bytes
};
```

`src/format/zfile_format.hpp::HeaderTrailer`

```cpp
struct HeaderTrailer {
    uint32_t size = kHeaderSize;
    uint32_t digest = 0;
    uint64_t flags = 0;
    uint64_t index_offset = 0;
    uint64_t index_size = 0;           // entry count
    uint64_t original_file_size = 0;
    uint32_t index_crc = 0;
    uint32_t reserved_0 = 0;
    CompressOptions opt;

    bool get_flag_bit(int shift) const;
    void set_flag_bit(int shift);
    void clr_flag_bit(int shift);
    bool is_header() const;            // ! => is_trailer()
    bool is_data_file() const;
    bool is_sealed() const;
    bool is_header_overwrite() const;
    bool is_digest_enabled() const;

    static HeaderTrailer parse(const void* region);  // 512B region
    void serialize(void* region) const;              // 512B region
    bool region_digest_ok(const void* region) const;
    void set_region_digest(void* region);
};
```

- `parse` throws `obd::format_error` on magic mismatch or a `size` field
  other than 96. It does **not** verify the digest.
- `serialize` writes the 96 payload bytes plus zero padding; the digest
  field is written as-is (call `set_region_digest` afterwards when
  `calc_digest` is enabled).
- `src/format/zfile_format.hpp::region_digest_ok` returns true when the
  region's digest equals `crc32c` over the 512B region computed with the
  digest field zeroed; regions without the `calc_digest` flag trivially pass
  (upstream `is_valid()` warns and returns true in that case).
- `src/format/zfile_format.hpp::set_region_digest` computes the digest with
  the digest field zeroed and stores it into both the region and `digest`.

`src/format/zfile_format.hpp::JumpTable`

```cpp
class JumpTable {
public:
    using uinttype = uint16_t;
    static constexpr uinttype kUinttypeMax = UINT16_MAX;

    void build(const uint32_t* ibuf, size_t n, uint64_t offset_begin,
               uint32_t block_size, bool enable_crc);
    uint64_t operator[](size_t idx) const;  // idx in [0, n]
    size_t size() const;
    int group_size() const;
};
```

- `build` takes the on-disk u32 per-block compressed sizes (including the
  4-byte CRC when `enable_crc`), the byte offset of block 0
  (`512 + dict_size`), and the block size. Throws `obd::format_error`:
  `EINVAL` when `block_size` is not a power of two ≤ 65536, `EIO` when an
  entry is ≤ the minimum block size (4 with CRC, 0 without), `ERANGE` on u16
  delta overflow — upstream `build()` semantics.
- `operator[](idx)` is the byte offset of block `idx`; `idx == n` yields the
  end of the last block (the index region offset). O(1).
- `group_size = 65536 / block_size`.

### `src/format/block_codec.hpp` — `namespace obd::format`

`src/format/block_codec.hpp::BlockCodec`

```cpp
class BlockCodec {
public:
    virtual ~BlockCodec() = default;
    virtual int decompress(const void* src, size_t src_len, void* dst,
                           size_t dst_capacity, size_t expected) = 0;
    virtual int compress(const void* src, size_t src_len, void* dst,
                         size_t dst_capacity) = 0;
    virtual size_t compress_bound(size_t src_len) const = 0;
    virtual const char* name() const = 0;   // "lz4", "zstd"
};
```

Raw single-block codec, byte-compatible with upstream's `compressor.cpp`
(`LZ4_compress_default` / `ZSTD_compress`, no container framing). All methods
are synchronous CPU work. `decompress` requires `expected` to be the exact
uncompressed size and returns 0 on success, `-EIO` on malformed input,
`-EINVAL` on size mismatch, `-EOVERFLOW` when `dst_capacity` is insufficient.
`compress` returns the compressed size (> 0) or a negative -errno
(`-E2BIG` / `-EOVERFLOW` / `-EIO`); `dst_capacity` must be at least
`compress_bound(src_len)`. The ZSTD codec defaults level 0 to 3 (upstream
zfile's level).

```cpp
std::unique_ptr<BlockCodec> create_block_codec(uint8_t algo, uint8_t level);
bool block_codec_available(uint8_t algo);
```

`create_block_codec` returns `nullptr` for algos unsupported in this build
(MINI_LZO legacy, ZSTD without `OBD_ENABLE_ZSTD`, unknown ids).
`src/format/block_codec.hpp::block_codec_available` reports whether the build
can decode `algo`.

### `src/format/zfile.hpp` — `namespace obd::format`

`src/format/zfile.hpp::is_zfile`

```cpp
elio::coro::task<bool> is_zfile(source::BlobSource& src);
```

Probes whether `src` is a ZFile: reads the first 512 bytes and checks magic
plus (when `calc_digest` is set) the region digest, matching upstream
`is_zfile()`. Never throws; returns false on read errors or files smaller
than two regions.

`src/format/zfile.hpp::ZFileSource`

```cpp
class ZFileSource final : public source::BlobSource {
public:
    static elio::coro::task<std::unique_ptr<ZFileSource>> open(
        source::BlobSourcePtr src, bool verify);

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;
    uint64_t size() const noexcept override;        // original_file_size
    std::string_view label() const noexcept override;  // "zfile(<inner>)"

    const zfile::HeaderTrailer& header() const noexcept;
    const zfile::JumpTable& jump_table() const noexcept;

    static constexpr size_t kReadWindow = 512 * 1024;
};
```

- `open` takes ownership of `src`. `verify` enables per-block `crc32c_salt`
  checks **only when the file itself carries verify checksums** (effective
  verify = `opt.verify && verify`). Throws `obd::error(EINVAL)` on a null
  source; `obd::format_error` / `obd::error` on any validation failure:
  file smaller than two regions, short reads, header not a header or digest
  mismatch, trailer not sealed/data-file (when not `header_overwrite`),
  `index_size > kMaxIndexSize`, index region exceeding the file layout,
  dictionary compression in use, unsupported algo, or `index_crc` mismatch
  (when `calc_digest`).
- `pread` reads in the **uncompressed** address space
  `[0, original_file_size)`. Follows the `BlobSource` contract: returns
  `count`, clamped at EOF (0 at/after EOF), negative -errno on failure.
  Consecutive blocks are batched into one underlying pread per
  `kReadWindow` (512 KiB) window; full-block destinations decompress
  directly into the caller's buffer, partial blocks through a scratch
  buffer. Returns `-EIO` on a short underlying read or a `crc32c_salt`
  mismatch, and propagates codec errors. Complexity: O(blocks touched).

### `src/format/lsmt_format.hpp` — `namespace obd::format::lsmt`

Constants: `kSpace = 4096` (header/trailer region), `kHeaderSize = 390`,
`kAlignment = 512` (sector size), `kMaxRoIndexSize = 1000000`
(`MAX_LSMT_RO_INDEX_SIZE`), `kMaxStackLayers = 255` (`MAX_STACK_LAYERS`),
`kDataStartSector = 8`, magics `kMagic0`/`kMagic1`, flag shifts
`kFlagShiftHeader` (0), `kFlagShiftType` (1), `kFlagShiftSealed` (2),
`kFlagShiftGcLayer` (3), `kFlagShiftSparseRw` (4), `kFlagShiftHybridRw` (5).

`src/format/lsmt_format.hpp::HeaderTrailer`

```cpp
struct HeaderTrailer {
    uint32_t size = kHeaderSize;
    uint32_t flags = 0;
    uint64_t index_offset = 0;   // bytes
    uint64_t index_size = 0;     // entry count
    uint64_t virtual_size = 0;   // bytes
    std::string uuid;            // without NUL
    std::string parent_uuid;     // without NUL
    uint16_t reserved = 0;
    uint8_t version = 1;
    uint8_t sub_version = 1;
    std::string user_tag;        // without NUL padding

    bool get_flag_bit(int shift) const;
    void set_flag_bit(int shift);
    void clr_flag_bit(int shift);
    bool is_header() const;
    bool is_trailer() const;
    bool is_data_file() const;
    bool is_sealed() const;

    static HeaderTrailer parse(const void* region);  // 4096B region
    void serialize(void* region) const;              // 4096B region
};
```

- `parse` throws `obd::format_error` on magic mismatch or a `size` field
  other than 390. Strings are read as NUL-terminated within their fixed
  37/256-byte fields.
- `serialize` writes the 390 payload bytes plus zero padding; a string
  longer than its on-disk field throws `obd::format_error`.

### `src/format/hybrid_format.hpp` — bounded writable-log replay

`hybrid::validate_pair` validates the supported writable flags/version,
data/index UUID and parent identity, matching sector-aligned virtual size,
artifact sizes and log length before any buffer is allocated from metadata.
The limit is four million log records and one million live mappings.
`hybrid::Replay` consumes complete 16-byte records in bounded chunks, applies
later logical ranges over earlier ones, and clears runtime tags. Abandon the
replay object after an exception; it is a setup-only parser, not a transactional
live writer.

`finish()` rejects overlapping live DATA physical extents. ZERO backing is
retained only when its full physical range fits and is exclusive of all other
live DATA/ZERO ranges. Other valid ZERO markers become unbacked in memory;
this prevents treating OverlayBD EOF markers as AgentENV reusable storage.
Truncated records, invalid ranges, noncanonical padding and unsupported headers
fail closed. These structural checks cannot detect arbitrary corruption that
happens to encode another valid unchecksummed record. See ADR-0026.

Sealed Hybrid readers accept the all-ones 55-bit physical offset only for ZERO
records in a supported Hybrid layout. The marker is never dereferenced; DATA
still requires an in-file extent. Hybrid logical ranges must be nonempty and
fit the declared virtual size. Ordinary LSMT keeps its existing ZERO bounds.

### Hybrid artifact recovery and write engine

`hybrid::FilePair::open_existing` opens both existing artifacts without creation
or truncation. Setup filesystem calls run on the blocking pool; header and log
contents use native asynchronous reads. Both regular-file descriptors retain
shared reader or exclusive writer locks. Callers must drain operations before
destroying the pair; destruction releases both locks immediately even when
descriptor close is deferred by the runtime. Pair validation precedes bounded
64 KiB log reads; a sealed artifact is refused with `EALREADY`.

`hybrid::FilePair::create` publishes a new directory containing `data` and
`index`. Both headers carry the same generated UUID and virtual size. Initialization
writes and syncs both files, syncs the staging directory, publishes it with a
no-replace directory rename, then syncs the parent. Existing destinations are
never overwritten. Before publication, ordinary error cleanup removes only the
owned staging directory; after publication, a parent-sync error retains the
complete pair and reports failure. Recovery after process exit and publication
faults must be tested separately from successful creation.

Deterministic creation tests inject returned errors and separate-process exits
after each header sync, staging-directory sync, log-state activation, publication
rename and parent-directory sync. Before rename, returned errors clean up only
the owned staging directory; an abrupt exit can leave an unreferenced
`.hybrid-init-*` directory. Retry creates a new pair without adopting or deleting
that abandoned private directory. After rename, retry reopens the complete pair
and settles its parent-directory sync. These tests exercise publication and
retry control flow, not physical power-loss behavior.

`hybrid::Writer` owns an exclusively opened pair. Its asynchronous operation
mutex serializes direct reads, writes, discard and FLUSH. DATA rewrites reuse
current-upper physical ranges without appending mapping records. New ownership
and ZERO-to-DATA transitions prepare the replacement in-memory index first,
complete payload writes and then append the mapping record without a per-request
data sync. The
replacement becomes visible only after the complete record write. Discard
retains existing exclusive backing or writes a fresh zero allocation for an
unbacked range. Repeated discard/rewrite reuses that allocation. FLUSH syncs
data before index; an uncertain modifying I/O failure closes the engine to
further reads/writes until recovery, rather than exposing stale ownership.

`Writer::compact()` builds a current-only index. It reserves exclusive backing
for imported unbacked ZERO in a staged view, syncs payload, writes and syncs a
replacement index, renames it over the owned index, and syncs its directory.
Successful publication adopts the replacement descriptor and cursor even when
the following directory sync fails. A later FLUSH retries directory durability.
Before rename, failed compaction leaves the original descriptor/cursor active;
newly reserved, unpublished ZERO space can remain as orphan payload after a
failed attempt. No live payload is copied just to compact the mapping log.
The descriptor generation used to notice publication is runtime bookkeeping,
not an on-disk generation marker or stale-sidecar checksum.

Before a modifying fragment, automatic compaction runs when the log contains at
least `max(4096, 2 * live_mappings + 16)` records, counting upstream padding.
Adjacent physically and logically contiguous mappings of the same state are
coalesced up to the wire length limit. The hard replay and live-index ceilings
remain independent of this cadence. Compaction's ZERO reservation does not
append more records to a log already at its hard limit.

`Writer::checkpoint()` serializes with in-flight operations and runs the same
payload, index and directory durability barrier as FLUSH. Only a successful
barrier marks this writer instance terminal: subsequent writes, discard, grow,
compaction, seal and repeated checkpoint return `EROFS`. Reads and FLUSH remain
available. A failed barrier leaves checkpoint retryable. The terminal flag is
process-local; a fresh exclusive reopen can resume writes or perform offline
seal on the durable pair. Device shutdown checkpoints through this primitive before releasing the upper.

`Writer::grow()` runs off scheduler workers, rejects an overlapping operation
with `EBUSY`, and publishes the larger virtual size only after both headers are
durable. It does not extend or move payload. `Writer::seal()` serializes with
other operations, compacts the index, and appends only index metadata, alignment
padding and a sealed trailer to the existing data file. Successful seal leaves
direct reads available but makes subsequent mutations read-only; repeat seal
returns `EALREADY`.

Grow and seal use a local `<index>.txn` publication intent, separate from the
upstream data/index wire formats. The fixed 16 KiB version-1 record contains a
CRC32C, operation, both device/inode identities, original lengths, an index-record
CRC32C, old headers and the target header/trailer. Publication writes and syncs
an owned temporary intent, renames it without replacing an existing intent, and
syncs the directory before modifying either artifact. Grow syncs the data header
then the index header; seal appends the index and trailer then syncs data. Both
retire the intent and sync its directory after completing the operation.

Exclusive reopen validates a pending intent and rolls it forward before normal
pair parsing. Read-only open cannot perform this repair (`EBUSY`). Recovery is
idempotent across interrupted metadata writes and refuses truncated/corrupted
intents, changed file identities and mismatched index content before modifying
the artifacts. Copying a pending transaction to different inodes is unsupported;
recover it on its original files first. A completed seal is recognized as
`EALREADY`. Writable reopen also syncs the index directory and its parent to
settle namespace durability after a previous publication error.

The transaction checksum protects pending grow/seal metadata. Ordinary log
integrity uses separate local `user.overlaybd.hybrid.log` extended attributes
on both files. Each fixed 128-byte version-1 record has a CRC32C and bounded log
stamps (random generation, byte length and CRC32C of the mapping-record prefix).
The index attribute records completed append progress; the data attribute
anchors the last durable prefix. A new mapping completes ordinary payload I/O,
appends the complete upstream record, then updates index progress before
reporting success. In-place DATA rewrites do not change either log stamp.

Completed writes and discards, including their mapping transitions, survive a
process crash without FLUSH when the OS and filesystem continue running. Each
operation completes payload I/O, complete index-record writes and progress
publication before returning. This is not a guarantee that those bytes have
reached persistent storage. A process interrupted during an operation may leave
an absent or partial operation; in-place writes may leave mixed bytes.

After a host crash or power loss, operations not covered by successful FLUSH may
be absent, partial, or recovered with incomplete payload. A valid mapping CRC
checks index integrity, not payload durability. Recovery does not promise an
exact last-FLUSH image because subsequent in-place writes may already have
modified its physical blocks. Lower files remain immutable; a flushed ZERO mask
remains protective unless a later operation modifies that range. An unflushed
ZERO-to-DATA rewrite can lose that zero guarantee after host failure.

FLUSH syncs payload, the index including its xattr, and the index directory,
then publishes and syncs the data anchor. Reopen verifies the committed prefix
before accepting a newer complete progress prefix. An incomplete progress update
or extra unconfirmed complete records may recover to an earlier verified prefix;
a writable reopen truncates that suffix only after semantic replay succeeds.
Read-only recovery uses the selected prefix without modifying files. A corrupt
committed prefix, unrelated generation or missing/corrupt integrity metadata
fails closed. Partial 16-byte records still fail structural validation.

Compaction gives its new index a fresh generation and progress stamp. Before
rename, it syncs both candidate logs and publishes a durable data anchor naming
both generations. After rename and directory sync it keeps only the new stamp.
A failure after rename adopts the new descriptor, mappings and cursor even if
an exception propagates. The next FLUSH or writable reopen settles the pending
anchor; an old generation cannot become authoritative after successful commit.

Writable unmanaged upstream imports are semantically validated before adoption.
An installing data anchor is synced before creating index progress; interruption
there can resume only the same verified prefix. Managed writable pairs require
filesystem support for user xattrs. Read-only unmanaged upstream artifacts do
not. Backups intended to retain local protection must preserve both attributes.
For exchange with another writer, stop/drain and copy the two ordinary byte
artifacts to a fresh destination without local attributes; the receiving writer
then owns that copy. Upstream writers do not maintain these local attributes.
Removing both attributes makes a pair indistinguishable from an unmanaged
upstream import; the metadata is not a record of externally deleted history.

Deterministic error tests interrupt integrity adoption after the installing
anchor sync, index progress write, and stable anchor write. Both upstream fixture
families must reopen without changing their ordinary bytes. A failed append
progress publication requires closing and reopening the writer; a completely
published failed operation may appear after recovery. A checkpoint anchor error
leaves the writer nonterminal so another write and checkpoint retry can succeed.
These injected errors model interruption at the boundary, not an actual torn
xattr or a filesystem's power-loss persistence behavior.

Native writer error tests also stop after half/full payload writes, before index publication,
and half/full index-record appends. Unpublished mappings remain invisible after
recovery; a half-record tail fails closed without truncation. A partial in-place
payload overwrite can survive reopen as mixed old/new bytes, as the documented
non-atomic overwrite contract permits. Separate FLUSH error tests refuse each
data/index/directory sync, then verify another write and checkpoint retry.
The writer submits complete records directly through native asynchronous IO;
there is no separate userspace buffered-index flush path. These are controlled
IO boundary/short-write simulations, not a storage power-loss model.

Process-exit tests cover intent publication, each header sync, seal index/trailer
writes, data sync and intent retirement. They do not simulate storage-cache loss
or prove power-loss atomicity of in-place payload overwrites. The extended
attributes do not alter the upstream file byte layout or reader protocol.

`HybridRwLayer` adapts the engine to `WritableLayer`: direct logical reads,
writes, discard, grow, FLUSH and checkpoint delegate to the serialized engine.
Its physical `BlobSource` reads the existing payload descriptor and reports the
current allocated payload boundary. Segment snapshots are owned copies; the
legacy borrowed `segments()` view requires external exclusion of other calls.
Creation and existing-pair open remain separate, so recovery cannot accidentally
create an empty upper when files are missing.

`HybridRwLayer::seal_file(data, index, tag, digest, size, virtual_size, reject)`
provides the offline operation after the device has stopped. It opens the existing
pair exclusively, validates an optional grow-only size override, uses the grow
transaction if needed, then seals and hashes the resulting artifact in bounded
1 MiB asynchronous reads. Both file locks remain held through digest completion;
it hashes the same data descriptor used for seal, without reopening a pathname.
The returned SHA-256 and byte count describe the whole sealed artifact and are
published to the output arguments only on success. No payload is copied for
compaction, though computing the digest necessarily reads the complete file.

Invalid alignment/range, shrink and oversized tags return `EINVAL` with a reason;
missing files return `ENOENT`, another owner prevents the exclusive open, and
already sealed output returns `EALREADY`. Existing files are never recreated.
An operational error after publication may leave a complete sealed artifact or a
recoverable intent; failure does not promise rollback to a writable pair. Reopen
settles the intent before reporting already sealed. This helper accepts a valid
recoverable Hybrid log without requiring the terminal LSMT checkpoint format.
Supervisor commit dispatch records the data/index paths at device creation and
uses this helper after stopping and reaping the child. Later config edits cannot
redirect either artifact.

Hybrid requests `serialize_merged_reads()`. `MergedWritable` then registers
reads with its grow admission gate and holds its operation mutex from snapshot
selection through completion of every physical read. Mutations and their index
rebuilds use that same mutex. The lock order is merged operation, upper engine
operation, then the short snapshot mutex; no lock is held across a reverse call
into the merger. Existing tops retain their current read scheduling. Once an
upper belongs to a merger, mutations must use that merged interface; direct
lifecycle access is for callers that have drained merged IO. Taking a snapshot
and reading the physical view independently does not establish a read transaction.

Image configuration accepts explicit `upper.type = "hybrid"` and assembles the
pair under `<upper.dir>/overlaybd.hybrid`. An absent pair directory is atomically
created; existing incomplete pairs fail closed. Reopen preserves a larger
persisted size or grows to a larger requested size, while an explicit override
that would shrink the recovered upper is rejected during ordinary creation.
USER_RECOVERY instead requires an existing pair and retains its persisted size,
even when the original create-time override predates online grow. Supervisor commit selects
the Hybrid offline helper using paths captured at creation. Blank-device type
selection uses `blank.upper_type`; see ADR-0026 and the test documentation for
fault and performance validation limits.

### `src/format/lsmt.hpp` — `namespace obd::format`

`src/format/lsmt.hpp::LsmtLayer`

```cpp
class LsmtLayer {
public:
    static elio::coro::task<std::unique_ptr<LsmtLayer>> open(
        source::BlobSourcePtr src);

    const std::vector<bytes::segment_mapping>& segments() const noexcept;
    uint64_t virtual_size() const noexcept;
    const lsmt::HeaderTrailer& header() const noexcept;
    source::BlobSource& data_source() const noexcept;
    const std::string& layer_label() const noexcept;  // "lsmt(<inner>)"
};
```

One sealed read-only LSMT layer: the parsed segment index plus the
underlying data source (typically a `ZFileSource` view of the layer blob).
`open` takes ownership of `src` and validates, mirroring upstream
`do_load_index()` / `create_memory_index()`:

1. header region: magic, `is_header`, `is_data_file`;
2. trailer region: magic, `is_trailer`, `is_data_file`, `is_sealed` —
   trailer fields are authoritative;
3. `index_size ≤ kMaxRoIndexSize`, `index_offset` lies inside
   `[kSpace, trailer_offset]` (bounds checked BEFORE the subtraction —
   an out-of-range `index_offset` must not underflow the fit check into
   accepting an empty index), and the index region fits before the
   trailer;
4. index entries with `offset == segment_mapping::kInvalidOffset` are
   dropped; tags are cleared;
5. remaining entries must be strictly ordered (`a.end() <= b.offset`);
6. every `moffset` must lie within the data region
   `[kDataStartSector, index_offset/512)` sectors (for zeroed segments the
   inclusive form `8 ≤ moffset ≤ end`).

Any violation throws `obd::format_error` (or `obd::error` on short reads /
null source).

`src/format/lsmt.hpp::MergedLsmt`

```cpp
class MergedLsmt final : public source::BlobSource {
public:
    static elio::coro::task<std::unique_ptr<MergedLsmt>> open(
        std::vector<std::unique_ptr<LsmtLayer>> layers_bottom_up);

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;
    uint64_t size() const noexcept override;
    std::string_view label() const noexcept override;  // "lsmt-merged(N layers)"

    const std::vector<bytes::segment_mapping>& merged_index() const noexcept;
    const std::vector<std::unique_ptr<LsmtLayer>>& layers() const noexcept;  // top-first

    static void merge_indexes(
        const std::vector<const std::vector<bytes::segment_mapping>*>& layers,
        std::vector<bytes::segment_mapping>& out);
};
```

- `open` takes 1..255 layers in **bottom-up** order (`layers[0]` =
  bottom-most), matching upstream `open_files_ro()`; internally the stack is
  reversed so index tag 0 addresses the topmost layer. The device virtual
  size is the topmost non-zero layer `virtual_size`. Throws `obd::error` /
  `obd::format_error` on an empty stack, more than `kMaxStackLayers` layers,
  no non-zero virtual size, or a merged index larger than
  `kMaxRoIndexSize`.
- `pread` is **sector-aligned**: `offset` and `count` must be multiples of
  512 or it returns `-EINVAL` (the LSMT contract; ublk request granularity
  guarantees it). Holes and zeroed segments are memset to zero; covered
  ranges are read per segment from the owning layer's `data_source()`.
  Clamped at `size()`; a short layer read is retried once and then the tail
  is zero-filled (upstream `file.cpp` behavior). Returns `count` (or the
  remaining bytes at EOF), negative -errno on failure.
- `src/format/lsmt.hpp::merge_indexes` is the static gap-merge helper,
  exposed for unit tests: `layers[i]` is the segment list of layer `i`,
  **topmost first**; appends the merged, sorted, disjoint list to `out` with
  `tag = layer index`. Throws `obd::format_error` if the merged index
  exceeds `kMaxRoIndexSize`.

### `src/format/writable.hpp` — `namespace obd::format`

`src/format/writable.hpp::WritableLayer`

```cpp
class WritableLayer {
public:
    virtual ~WritableLayer() = default;
    virtual elio::coro::task<ssize_t> pwrite(const void* buf, size_t count,
                                             uint64_t offset) = 0;
    virtual elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                            uint64_t offset) = 0;
    virtual elio::coro::task<int> flush() = 0;
    virtual elio::coro::task<int> discard(uint64_t offset,
                                          uint64_t len) = 0;
    virtual elio::coro::task<int> checkpoint() = 0;
    virtual uint64_t virtual_size() const = 0;
    virtual int grow(uint64_t vsize) = 0;
    virtual const std::vector<bytes::segment_mapping>& segments() const = 0;
    virtual std::vector<bytes::segment_mapping> segments_snapshot() const;
    virtual source::BlobSource& data_source() = 0;
};
```

Sector-aligned writable layer (ADR-0008): the topmost layer of an image
stack. `pwrite`/`pread` offsets and counts must be 512-byte multiples (the
same alignment contract as the read side); both return the byte count or a
negative -errno. `pread` reads through this layer alone — holes read as
zeroes and fall-through to lower layers is the merger's job. `flush` is the
durability point (ublk FLUSH). `discard` (ADR-0009) is the writable-root
hook for masking a 512B-aligned range with zeroes in the merged device
contract. The LSMT-RW
implementation satisfies that contract by retaining zeroed segments; the
sparse implementation punches holes and persists a sidecar zero map so
reopen preserves the lower-layer mask. `checkpoint()` (ADR-0014) persists
whatever on-disk state an offline seal needs, without sealing; it
is called once by the device process on graceful shutdown after IO has
drained and is terminal (no `pwrite`/`discard` may follow). `segments()` is
the current index: sorted, disjoint, 512B sector units, tag 0.
`segments_snapshot()` returns a stable copy for callers that cannot borrow
`segments()` across concurrent writable-layer mutations. `data_source()` is
the file view segment data is read from.

`grow(vsize)` (D3) extends the layer's write window to `vsize` bytes so
`pwrite`/`discard` accept the new range. Grow-only: a request smaller
than the current size returns `-EINVAL`; an equal request is an
idempotent no-op (retried grows after a partial kernel failure land
here). It is **BLOCKING** (header rewrite + fsync for LSMT, ftruncate +
fdatasync for sparse) — run it off an Elio worker via
`elio::spawn_blocking`, as the device resize executor does
(docs/supervisor.md). LSMT direct concurrent `grow()`/`checkpoint()`/`seal()`
calls are rejected with `-EBUSY`. Sparse direct `grow()` also
coordinates with `pwrite()`/`discard()`/`flush()` and `checkpoint()`;
overlapping direct calls return `-EBUSY` rather than letting a resize
overtake data or sidecar durability. The supervisor normally serializes
those operations before they reach the layer.

### `src/format/sparse_rw.hpp` — `SparseRwLayer`

`src/format/sparse_rw.hpp::SparseRwLayer`

```cpp
class SparseRwLayer final : public WritableLayer {
public:
    static elio::coro::task<std::unique_ptr<SparseRwLayer>> open(
        const std::string& path, uint64_t vsize);
    static elio::coro::task<std::unique_ptr<SparseRwLayer>> open_existing(
        std::string path);
    static elio::coro::task<SparseExportResult> export_file(
        std::string source, std::string destination,
        std::string parent_uuid = {}, std::string user_tag = {});
    // + WritableLayer overrides
};
```

A sparse data file whose live DATA sectors form identity mappings
(`moffset == offset`) plus ZERO mappings for discarded ranges. `open`
requires `vsize` to be a non-zero multiple of 512 (throws
`obd::error(EINVAL)` otherwise), first opens an existing inode and otherwise
creates one with `O_CREAT | O_EXCL`, and sizes it to the maximum of the supplied
`vsize` and the current sparse file length. A valid `<path>.zeroes`
sidecar may declare that same trusted size or any smaller sector-aligned
size; sidecars that declare a larger size are rejected as corrupted. This
preserves online-grown uppers when the data file or caller-supplied size
already records the grown window without allowing sidecar metadata alone to
expand the device.

Both entrypoints require a regular file and hold a nonblocking exclusive
advisory inode lock before metadata recovery or growth, until layer destruction.
Another cooperating Sparse owner, including one opened through a hardlink,
causes `EBUSY`; checkpoint retains the lock. External mutation or pathname
replacement remains outside this cooperative locking contract. Keep the data
and metadata names stable while owned (ADR-0027).

`open_existing(path)` never creates a missing data file and never grows it.
It derives capacity from the opened descriptor, rejects empty/invalid files,
and uses the same exact coverage and compatibility-mask recovery rules.
Recovery may repair or import metadata; this is not a read-only open. Closing
its instance permits normal read-write reopen. This source-admission API does
not by itself provide the sparse export operator workflow tracked in #161.

The internal `sparse_export_detail` staging writer validates sorted exact
ownership, combines adjacent same-state ranges before canonical 16,383-sector
splitting, packs only DATA, emits ZERO, and omits INHERIT. It uses bounded
payload/index buffers and native asynchronous writes followed by staging sync.
Its UUID follows the existing deterministic seal digest convention: little-endian
virtual size, packed DATA, then encoded index; parent/tag remain explicit header
inputs. This internal entrypoint requires a stable source and a fresh staging
file from the export transaction. It does not yet provide destination alias
admission or publication itself; those belong to `SparseRwLayer::export_file`.

`export_file` exclusively recovers the existing source, rejects destination
names/inodes aliasing its data or metadata, checkpoints its own instance,
then writes a fresh sibling staging file. It returns `uuid`, `sha256`, and
`size` only after file synchronization and destination-directory synchronization.
The source remains reusable and read-write reopenable. A different existing
destination is never overwritten. A byte-identical destination is accepted as
an idempotent retry only after verification and renewed synchronization.

Publication uses a no-replace rename. Before publication, failures remove only
the private staging file; abrupt process exit can leave an unpublished temporary
that is never treated as a completed export. After publication, a directory-sync
failure returns an error but retains the visible output. Retry verifies the
complete identical artifact and retries the durability boundary. Keep source
paths and the destination directory stable against external replacement during
export. The offline command is `obd-export-sparse`; eligible supervisor devices
use `obdctl commit` after the stop/reap boundary (ADR-0027).

New uppers persist exact 512-byte INHERIT, DATA, and ZERO ownership in
`<path>.coverage` (ADR-0024). A sparse two-bit map page describes 8 MiB and
has two 4160-byte generation slots; CRC and a commit word let reopen select
the newest complete slot and fall back to the other slot after an interrupted
update. Invalid headers, recognized committed pages without a valid rollback,
identity mismatches, invalid states, and size errors stop open. A
random ID stored in a user xattr binds coverage to the data file and is the
primary identity even if the inode changes; filesystems without user xattrs
use the weaker device+inode fallback documented by ADR-0024.

The coverage artifact has this little-endian v2 layout:

| Region | Bytes | Meaning |
|---|---:|---|
| header | `0..4095` | magic `OBDSPCV2`, version 2, header size 4096, sector size 512, flags, initial virtual size, backing `st_dev`/`st_ino`, 128-bit identity, then CRC-32C over bytes `0..63` |
| page `p`, slot `s` | `4096 + p*8320 + s*4160` | 4160-byte generation record; `s` is 0 or 1 |

The header fields are: magic at 0 (8 bytes), `u32` version at 8, `u32` header
size at 12, `u32` sector size at 16, `u32` flags at 20, `u64` initial virtual
size at 24, `u64 st_dev` at 32, `u64 st_ino` at 40, identity at 48 (16 bytes),
and `u32` CRC at 64. Flag bit 0 requires the identity xattr; bit 1 marks a
legacy import; bit 2 proves the complete initial snapshot. Bit 2 is required
for an authoritative file or resumable complete temporary; other bits are
rejected. Bytes 68 through 4095 are reserved and
ignored by this version.

Each slot contains magic `OBDCVP2\0` at 0 (8 bytes), `u64` page number at 8,
nonzero `u64` generation at 16, a 4096-byte payload at 24, `u32` CRC at 4120
over slot bytes `0..4119`, reserved bytes at 4124 through 4151, and an eight-byte
commit word at 4152. Payload byte
`q/4`, bits `(q%4)*2..+1`, records sector `p*16384+q`: `00` INHERIT, `01`
DATA, `10` ZERO, and `11` invalid. The writer clears the inactive slot's
commit word, writes bytes `0..4151`, then writes the commit word. The file's
logical size is `4096 + ceil(vsize/(512*16384))*8320`; untouched page regions
remain sparse holes. The virtual size cannot exceed the 50-bit segment offset
window.

A page without a recognized commit marker has an implicit initial INHERIT
state. A corrupt recognized committed candidate requires a valid rollback slot
or open fails. A damaged sole first-generation commit marker cannot be
distinguished from interrupted first publication; arbitrary marker corruption
is outside this detection guarantee.

The snapshot-complete flag is set, with a refreshed header CRC, only after every
initial imported page has been written. An incomplete unmarked temporary
restarts legacy import. Before an xattr marker exists, temporary recovery checks
device/inode; afterwards a matching marker permits marker-preserving inode
replacement. Recovery synchronizes a complete temporary before installing its
marker and treats reopened coverage as potentially dirty, so FLUSH after a
process restart synchronizes it even without a new write.
Recovered exact ZERO also marks the compatibility mask dirty: an interrupted
discard may have committed ZERO before publishing `.zeroes`. Before a write
removes such ZERO, the complete protective mask is republished. A later
partial DATA publication can then retain the same ZERO view live and on reopen.

For a pre-existing v1 file with no coverage sidecar, `open` performs a
one-time `SEEK_DATA`/`SEEK_HOLE` import bounded by the original EOF, overlays
`.zeroes`, then atomically
publishes v2 coverage. The imported view preserves what v1 currently exposes;
it cannot reconstruct original sub-block write intent. Once v2 exists,
filesystem extents never broaden ownership. `.zeroes` is loaded after v2 and
remains a protective ZERO-only overlay for migration and rolling
compatibility. A `.zeroes` file recorded at an older, smaller vsize is dirty
until the next durability boundary republishes its current size.

Creation and migration write and sync `<path>.coverage.tmp`, validate every
header and committed page, then expand the data file, install the data-inode
identity xattr, and sync the data inode,
then rename the sidecar and sync the parent directory. A marker with neither
final coverage nor a valid temporary artifact fails closed. Existing
zero-length inodes remain existing and cannot trigger stale-sidecar cleanup.
The complete snapshot precedes expansion even on filesystems that allocate
zero-filled bytes: interrupted fresh creation cannot import those bytes as
guest DATA, and a legacy grow leaves its new tail inherited. A complete
installing temporary may declare a virtual size beyond the pre-open EOF;
all DATA sectors must still lie within that EOF. Authoritative final coverage
also rejects an initial virtual size beyond EOF. On no-xattr filesystems,
deletion of all coverage
artifacts leaves no durable marker and is indistinguishable from a legacy v1
upper; the implementation then attempts legacy import.

`pwrite`/`pread` return `-EINVAL` on unaligned or out-of-`vsize` requests.
Writes are split at the 14-bit segment-length cap, write backing bytes, then
publish every affected coverage page before updating the in-memory index and
returning success. Adjacent DATA extents coalesce without absorbing INHERIT
sectors. Rewrites wholly inside existing DATA skip ownership publication and
leave the mapping generation unchanged; backing bytes still participate in
FLUSH. Pending ZERO-to-DATA transitions settle before this check. `discard`
first publishes exact ZERO coverage and the protective
compatibility mask in `<path>.zeroes`, then performs
`fallocate(FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE)`. Punch-hole remains
best-effort physical reclamation: coverage preserves zero reads if it is
unsupported or fails.

`pwrite`, `discard`, and `flush` serialize metadata updates so publication
cannot overtake an earlier operation. A failure partway through a multi-page
coverage update can leave the committed prefix visible; the request has not
completed, the running index and reopen both expose that prefix, and
USER_RECOVERY reissues the whole idempotent operation.
Planning allocation failures in sparse `pwrite`/`discard` return `-ENOMEM`
(or `-EFBIG` for an oversized container), including after an earlier piece
commits. The merged view refreshes its index even on a negative result; if a
top instead throws after publishing, it refreshes or invalidates the index
before propagating the original exception.
For each page, all vector/map allocation precedes the first slot write and the
commit is followed only by a non-throwing runtime swap. When DATA replaces a
legacy ZERO mask, the runtime retains that ZERO until `.zeroes` removal changes
the namespace; a rename/unlink followed by directory-sync failure exposes the
new DATA view while retaining dirty state for a later durability retry.
`flush()` offloads blocking compatibility-sidecar work and preserves the
existing zero-mask order: writes that remove ZERO sync data before removing
`.zeroes`, while discard publishes ZERO before syncing a punched file. It
then syncs exact coverage, so a successful guest FLUSH never makes DATA
ownership durable ahead of its backing bytes. It returns 0 or `-errno`.

Direct `grow()` calls are rejected with `-EBUSY` while
`pwrite()`/`discard()`/`flush()` or `checkpoint()` is active, and those
coroutine operations likewise reject a concurrently active grow with
`-EBUSY`. `checkpoint()` uses the same flush core, then marks the sparse
upper terminal: later `pwrite()`, `discard()`, `grow()`, and a second
`checkpoint()` return `-EROFS`.

On reopen, exact coverage is authoritative and `.zeroes` overlays only ZERO.
Thus a 512-byte write inside a filesystem-reported 4 KiB extent does not hide
the seven adjacent lower sectors, while a sub-block discard remains a
top-layer zero mask independently of physical hole-punch granularity.
A successful discard remembers which zero-mask generation it already published.
The next FLUSH can reuse that unchanged publication while still synchronizing
payload and exact coverage in the existing order. Failed publication, a newer
mask generation, grow, and reopen do not acquire this in-memory shortcut.
The persisted format and recovery precedence remain unchanged.

Opening this format with a pre-v2 binary is unsafe because that binary ignores
`.coverage` and reconstructs ownership from allocation extents. There is no
in-place downgrade contract.

#### Validated filesystem coordinates and costs

The 2026-10-26 validation used a Linux `6.18.18.c1126-trim` guest with
separate, newly formatted regular-file disks and no networking. ext4 and XFS
used 4096-byte blocks, `noatime`, and their default allocation settings.

| Filesystem coordinate | Identity | Observed validation |
|---|---|---|
| ext4 | user-xattr ID | 51 sparse/migration/merged cases, 35,730 assertions |
| XFS | user-xattr ID | 51 sparse/migration/merged cases, 35,730 assertions |
| VFAT, FAT16, 4096-byte clusters | device/inode fallback | eight bounded cases, 213 assertions; coverage header flags confirmed the weak binding |

A separate fixture fed real `TagEngine` handlers WRITE_ZEROES with and
without NOUNMAP, plus DISCARD, through `MergedWritable` into the sparse upper.
On all three filesystems, completed requests followed by `_exit` without an
upper FLUSH or destructors recovered ZERO/DATA/ZERO respectively and preserved
every adjacent nonzero lower byte. Request inputs were scripted; this fixture
did not register a kernel ublk device. A read-only data descriptor also forced
physical reclamation to fail while preserving old backing bytes; logical ZERO
still survived process exit.

VFAT returned `EOPNOTSUPP` for user xattrs, FIEMAP, ZERO_RANGE, and punch-hole;
actual zero writes and the NOUNMAP DATA path succeeded. The selected metadata
design uses persisted ZERO for logical discard, replacing the physical-zero
fallback proposed for extent-based recovery (ADR-0024). It does not depend on
FIEMAP or ZERO_RANGE. VFAT is a bounded feature-absence fixture, subject to its
file-size and allocation limits; this result establishes neither a
production-capacity nor a cross-mount inode-identity guarantee.

Both ext4 and XFS also passed six cases (137 assertions each) with only
coverage-file `SEEK_DATA`/`SEEK_HOLE` calls forced to return `EINVAL`;
legacy data-file discovery remained real. This verifies the full-scan fallback,
without claiming those filesystems lack extent enumeration. On both reference
filesystems, earlier FIEMAP probes widened a 512-byte DATA write to a written
4096-byte extent after synchronization. A partial ZERO_RANGE and plain
preallocation both produced UNWRITTEN extents. A genuine 4096-byte gap between
two DATA extents remained a gap; the suspected gap-coalescing behavior did not
reproduce. Other kernels, allocation hints, and power cuts were not validated
by this matrix.

A local public-API control compared the
[pre-v2 implementation](https://github.com/Coldwings/OverlayBD-Elio/tree/96515944e0d3e0be48b95660aed568e0aa99ce6c)
with the
[v2 implementation](https://github.com/Coldwings/OverlayBD-Elio/tree/22b187bde3670021dc0ce0613cd59708320be93d).
Both used test-enabled RelWithDebInfo builds on an Intel i3-N305, one Elio
worker and one blocking thread, buffered files, 64 MiB, and 16,384 random
4096-byte requests per trial. The same measurement program used each
variant's actual headers, compiler options, and libraries. Three repetitions
alternated variant order; observed peak API depth matched the requested depth.
Values below are medians of the three per-trial results.

| Operation / depth | CPU us/request, v1 → v2 | Mean API latency us, v1 → v2 |
|---|---:|---:|
| First write / 1 | 31.61 → 91.57 | 32.66 → 92.75 |
| First write / 16 | 35.23 → 95.92 | 577.88 → 1559.98 |
| DATA rewrite / 1 | 20.94 → 19.87 | 21.74 → 20.59 |
| DATA rewrite / 16 | 20.90 → 21.79 | 354.34 → 356.99 |
| Read / 1 | 2.03 → 2.13 | 2.00 → 2.10 |
| Read / 16 | 1.66 → 1.66 | 26.41 → 26.42 |

First-write publication remains a measured cost. Read/depth-16 CPU observations
ranged from 1.58–1.68 us in v1 and 1.54–1.70 us in v2; these short, overlapping
samples do not establish a stable improvement or regression. CPU includes
coroutine dispatch and joins; final FLUSH and content checks were outside the
timing window. Rewrite/read files were filled and flushed first, but residency,
host load, and frequency were uncontrolled. These are source-API diagnostics,
not ublk/fio latency or a device scaling claim.

Fresh-process recovery controls used a 1 TiB sparse virtual device, one 4 KiB
DATA block in each of 64/512/4096 distinct 8 MiB regions, with the same kernel
remaining alive after preparation and FLUSH. Cache eviction was not requested
and residency was not verified. All prepared DATA blocks were checked.

| Touched regions | Open wall ms, v1 → v2 | Open CPU ms, v1 → v2 | Process RSS delta KiB, v1 → v2 |
|---|---:|---:|---:|
| 64 | 0.17 → 0.84 | 0.14 → 0.84 | 60 → 168 |
| 512 | 0.68 → 4.99 | 0.65 → 5.03 | 172 → 252 |
| 4096 | 10.32 → 40.91 | 10.29 → 40.99 | 400 → 900 |

The coverage file's logical length was 1,090,523,136 bytes; allocated storage
was 520/4108/32824 KiB respectively. RSS deltas include allocator/runtime
effects and are not pure metadata residency. These recovery timings used
allocated-range enumeration. Without metadata `SEEK_DATA`, recovery scans the
entire logical coverage file and builds its candidate-page set by capacity;
large-capacity fallback time and temporary memory were not benchmarked here.

### `src/format/lsmt_rw.hpp` — `LsmtRwLayer`

`src/format/lsmt_rw.hpp::LsmtRwLayer`

```cpp
class LsmtRwLayer final : public WritableLayer {
public:
    ~LsmtRwLayer() override;

    static elio::coro::task<std::unique_ptr<LsmtRwLayer>> create(
        const std::string& path, uint64_t vsize);

    static elio::coro::task<std::unique_ptr<LsmtRwLayer>> open_or_create(
        const std::string& path, uint64_t minimum_size, bool existing_only = false);
    static elio::coro::task<std::unique_ptr<LsmtRwLayer>> open_existing(
        const std::string& data_path, const std::string& index_path);
    static elio::coro::task<std::unique_ptr<LsmtRwLayer>> create_persistent(
        const std::string& data_path, const std::string& index_path, uint64_t vsize);
    static elio::coro::task<std::unique_ptr<LsmtRwLayer>> migrate_checkpoint(
        const std::string& data_path, const std::string& index_path);

    // + WritableLayer overrides (grow() updates both persistent headers)

    bool sealed() const noexcept;
    elio::coro::task<int> seal(const std::string& user_tag = "");
    static elio::coro::task<int> seal_file(const std::string& path,
                                           const std::string& user_tag,
                                           std::string* sha256_hex,
                                           uint64_t* size,
                                           uint64_t virtual_size = 0,
                                           std::string* reject = nullptr);
};
```

The writable layout is an upstream append-type LSMT data/index pair
(ADR-0029): a 4096-byte data header with flags 3 and a 4096-byte index header
with flags 1 followed by standard 16-byte mappings. Pair UUID, parent identity,
capacity, versions and mapping bounds are validated before recovery changes
anything. The latest mapping owns each logical range. Invalid-offset upstream
padding is ignored, runtime tags are cleared, and final live DATA extents must
not physically alias because writes can update payload in place.

- `create(path, vsize)` creates `path` and `path + ".index"` exclusively;
  existing artifacts are never truncated. Capacity must be positive and
  sector-aligned. Creation stages complete headers before publishing a
  restartable intent and links the final names without replacing them.
- `open_existing(data, index)` opens a valid pair; `open_or_create(path, size,
  existing_only)` also migrates valid legacy checkpoints and, unless recovery
  is requested, creates when both artifacts are absent. Recovery never creates
  an empty substitute. Existing larger capacity is preserved; ordinary open
  can grow to a larger minimum. A complete sealed artifact refuses writable
  reopening even when a stale index sidecar remains.
- `pwrite` overwrites live DATA in place and appends payload for uncovered or
  ZERO-covered subranges. New payload completes before its mapping record,
  which completes before success is returned. Mapping insertion splits/trims
  previous coverage. A failed mapping append poisons the writer until reopen.
- `discard` publishes ZERO mappings before reporting success; ZERO owns no
  physical payload. Splits preserve the placeholder offset. Holes inherit the
  lower in a merged view, while ZERO masks it. Direct upper reads return zero
  for both holes and ZERO; reads and writes remain sector-aligned.
- Completed operations recover after process exit without a prior FLUSH while
  the OS continues running. Interrupted operations may be absent or partial,
  including mixed bytes for an interrupted in-place overwrite. This is not
  rollback to the last FLUSH. `flush()` synchronizes payload, index and directory
  metadata in order, propagating errors. Power loss before FLUSH has weaker
  guarantees. There is no per-record data synchronization barrier.
- Replay validates all complete records before trimming an incomplete EOF
  record. Malformed full records fail without resetting data. Index history is
  bounded through compact replacement; payload and the new index are synced
  before rename. Once published, the new descriptor remains authoritative even
  if directory sync fails. Stable pathname and inode locks exclude cooperating
  owners through replacement. Do not modify a pair with external tools while
  it is owned.
- `grow()` runs on the blocking pool. It publishes a checksummed intent with
  both target headers before rewriting either; recovery validates identity and
  mappings, then rolls forward and retires the intent. Failed growth blocks
  ordinary mutations until recovery. Direct overlapping grow/checkpoint/seal
  operations return `-EBUSY`; production IO is serialized by `MergedWritable`.
- `checkpoint()` flushes and terminates the current instance: subsequent
  mutations and another checkpoint return `-EROFS`. The unsealed pair can reopen
  for further writes. It does not append a legacy checkpoint trailer.
- `seal()` copies live payload in logical order into a temporary, appends the
  compact index and sealed metadata, syncs it, atomically replaces the data
  pathname, and syncs its parent directory. Existing reads rebind to the new
  inode. Before publication, failure preserves the original; after publication,
  a directory-sync error leaves the sealed artifact terminal. Mutations and
  repeat seal return `-EROFS`. Stable ownership remains until destruction.
- `seal_file` exclusively opens the pair or migrates a validated legacy
  checkpoint, seals it and returns the digest and size. A pair needs no prior
  checkpoint. Optional capacity override is grow-only and sector-aligned.
  Sealed input returns `-EALREADY`; invalid mappings fail before migration;
  setup diagnostics are returned through the optional rejection string.
- A legacy checkpoint migrates by publishing its index sidecar before any old
  metadata can be retired. Old checkpoint bytes remain unreferenced payload
  until seal compacts them away. A legacy memory-only upper without a valid
  checkpoint has no recoverable ownership; a header-only file is not proof of
  empty ownership because discard may have written only to the missing index.
- `data_source()` borrows the layer's descriptor handle and tracked size.
  Callers must drain operations before destruction. The pair owns its data and
  index descriptors; the read view has a duplicate data descriptor. Successful
  seal releases old payload ownership and rebinds the view to the sealed inode.
  Offline seal releases all descriptors on return, including error paths.

`src/format/lsmt_rw.hpp` — `create_empty_lsmt_layer`

```cpp
elio::coro::task<int> create_empty_lsmt_layer(const std::string& path,
                                              uint64_t vsize,
                                              const std::string& user_tag = "");
```

The ADR-0014 blank (raw) device **zero base** writer: creates a **sealed,
empty** LSMT RO layer at `path` — `virtual_size = vsize`, `index_size = 0`
(no segments, no data), so reads through the normal merge path serve the
whole `[0, vsize)` range as zeroes. Layout is the same as `seal()`'s
compaction output for an empty index: header region at byte 0, trailer
region as the file's last 4096 bytes, `index_offset = 4096` (the
data-region start / first allowed index position), total file size 8192 —
every reader bound check in `LsmtLayer::open` is satisfiable. Returns 0 or
a negative -errno (`-EINVAL` when `vsize` is not a positive multiple of
512). Setup and serialization may throw, including rejection of a user tag
longer than 255 bytes. Once the output is opened, both error returns and
exception unwinding close its descriptor and remove the partial file;
success retains the output.

**Determinism.** The layer obeys the seal determinism rule with no data
and no index entries, so its bytes are a pure function of `vsize` (and the
empty default `user_tag`): the uuid derives from
`sha256(vsize as LE u64)`. In particular, committing a blank upper that
never received a write (empty content, empty `user_tag`) reproduces the
zero base byte-for-byte.

### `src/format/merged_writable.hpp` — `MergedWritable`

`src/format/merged_writable.hpp::MergedWritable`

```cpp
class MergedWritable final : public source::WritableBlobSource {
public:
    static elio::coro::task<std::unique_ptr<MergedWritable>> open(
        std::vector<std::unique_ptr<LsmtLayer>> layers_bottom_up,
        std::unique_ptr<WritableLayer> top);

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;
    elio::coro::task<ssize_t> pwrite(const void* buf, size_t count,
                                     uint64_t offset) override;
    elio::coro::task<int> flush() override;

    uint64_t size() const noexcept override;
    std::string_view label() const noexcept override;  // "merged-writable(N layers)"

    WritableLayer& writable_top() const noexcept;
    std::vector<bytes::segment_mapping> merged_index() const;
};
```

The copy-on-write merged view (ADR-0008): sealed RO lowers (may be empty)
plus a writable top layer, as one block source.

- `open` throws `obd::error(EINVAL)` on a null `top`. The device virtual
  size is `max(top vsize, lowers vsize)`. Internally the stack is topmost
  first, tag 0 = writable top.
- `pread` walks an immutable merged-index snapshot exactly like
  `MergedLsmt::pread` (sector-aligned or `-EINVAL`; top wins; holes zero;
  short layer read retried once then zero-filled; clamped at `size()`). The
  snapshot is shared and immutable, so hot reads do not copy the full
  merged index on every request.
- `pwrite` validates alignment and bounds, delegates to
  `top_->pwrite` — **RO lowers are never modified** — and then rebuilds the
  merged index (`rebuild_index()`). Write-heavy workloads should batch,
  since the rebuild is O(index size) per write.
- `flush()` delegates to the top layer's `flush()` and rebuilds the merged
  index if a deferred ownership transition changes its generation.
  `pwrite()`, `discard()`, and `flush()` also rebuild or invalidate the index
  before propagating an exception from the top.
- `grow()` widens the writable top before publishing the merged view size;
  direct overlap with `pwrite()`/`discard()`/`flush()` returns `-EBUSY` so
  a resize cannot overtake a top mutation or durability boundary.
- `discard()` (ADR-0009) delegates to the top layer and rebuilds the index.
  With an LSMT-RW top, discarded ranges stay covered by zeroed segments and
  mask lower-layer data (matching upstream LSMT trim). With a sparse top,
  discarded ranges publish a sidecar zero map before best-effort top-file
  hole punching, preserving the same lower-layer mask semantics.
- As a `source::WritableBlobSource`, this is the device root the ublk bridge
  dispatches WRITE/FLUSH/DISCARD/WRITE_ZEROES to; a read-only image root
  simply does not implement the interface and writes/discards get `-EROFS`.

### `src/format/writer.hpp` — cold-path writers (`namespace obd::format`)

`src/format/writer.hpp::generate_uuid`

```cpp
std::string generate_uuid();
```

Generates a random lowercase 8-4-4-4-12 UUID string (36 chars) from
`std::random_device`, with RFC-4122-ish version/variant nibbles (cosmetic;
upstream treats the field as an opaque string).

```cpp
struct LsmtPayloadRange { uint64_t offset = 0, length = 0; }; // sectors
struct LsmtWriteOptions {
    std::string uuid;         // generated when empty
    std::string parent_uuid;  // may be empty (bottom layer)
    std::string user_tag;     // commit message, may be empty
    std::vector<LsmtPayloadRange> tail_ranges = {};
};
```

`src/format/writer.hpp::write_lsmt_single_layer`

```cpp
void write_lsmt_single_layer(int in_fd, uint64_t in_size,
                             const std::string& out_path,
                             const LsmtWriteOptions& opts = {});
```

Builds a sealed single-layer LSMT file at `out_path` from the raw image at
`in_fd` (read from offset 0). `in_size` must be a non-zero multiple of 512
(throws `obd::error(EINVAL)` otherwise). The index covers the whole input,
one segment per ≤ 16383-sector chunk, data region starting at sector 8.
Throws `obd::error` / `obd::format_error` on IO or serialization failure.
Layout: `header 4096 | data | index | trailer 4096`.
Optional `tail_ranges` supply sorted, disjoint logical sector ranges for stable
physical tail packing (ADR-0028). Ordinary payload comes first, then
selected payload, both in logical order; the index remains logically sorted.
No classification is serialized and full logical coverage is preserved.
Adjacent ranges normalize before splitting at the ordinary segment-length bound.
Invalid geometry, overlaps or excessive index counts fail before output mutation.
An empty plan keeps the original layout. Classification and converter selection
are separate responsibilities; this API does not infer filesystem structure.
The regression cases `format: lsmt tail placement preserves bytes with ordinary logical mappings`
and `format: lsmt invalid tail placement preserves an existing output` cover
physical ordering, logical readback, normalized determinism and preflight errors.
`format: lsmt tail packing splits wire boundaries and normalizes uniform classes`
checks cross-segment physical offsets, full logical bytes and equality of uniform
front/tail plans at the 16,383-sector wire boundary.

```cpp
struct ZFileWriteOptions {
    uint32_t block_size = 4096;       // power of two, <= 65536
    uint8_t algo = zfile::kAlgoLz4;   // kAlgoLz4 or kAlgoZstd
    uint8_t level = 3;                // zstd level; ignored for lz4
    bool verify = true;               // append crc32c_salt per block
    bool calc_digest = true;          // header/trailer digest + index crc
};
```

`src/format/writer.hpp::write_zfile`

```cpp
void write_zfile(int in_fd, uint64_t in_size, const std::string& out_path,
                 const ZFileWriteOptions& opts = {});
```

Compresses the file at `in_fd` (`in_size` bytes, read from offset 0) into a
sealed ZFile at `out_path`. Throws `obd::error(EINVAL)` on a bad
`block_size` or unsupported algo, `obd::error(EIO)` if a block fails to
compress, and `obd::error` on IO failure. Every block is compressed raw and
optionally followed by its 4-byte `crc32c_salt`; header and trailer carry
region digests and the index CRC when `calc_digest` is set.

Both writers are **synchronous cold-path utilities** (plain POSIX IO, retry
on `EINTR`, throw on error). Produced files are byte-compatible with
upstream overlaybd readers. Used by `obd-mkimage`, `obd-convert` and the test
fixtures; the data plane never writes. Callers that need byte-identical output
must pass deterministic writer options; the raw writer's default UUID is random
for fixture convenience.

### `src/format/trace.hpp` — `namespace obd::format::trace`

Dependency-free codec for the upstream prefetch trace blob (ADR-0013,
accepted; wire authority: [trace-format.md](./trace-format.md)). Pure
in-memory: no IO, no coroutines. Expected failure modes (corrupt input,
contract-violating appends) are reported by result value, never by
exception; the only exceptional way out is allocation failure
(`bad_alloc` → terminate), matching the codebase's hot-path stance.

Constants: `src/format/trace.hpp::kMagic` (0xC2EF1820), `kHeaderSize` /
`kRecordSize` (both 24), `kMaxRecordCount` (1048576 — the 1 MiB
replay-buffer cap, trace-format.md §8/§10).

`src/format/trace.hpp::TraceRecord`

```cpp
struct TraceRecord {
    char op;               // 'R' (READ) or 'W' (WRITE); any byte parses
    uint32_t layer_index;  // 0-based index into the image's lower list
    uint64_t count;        // read length in bytes
    int64_t offset;        // byte offset within the layer blob file
};
```

`src/format/trace.hpp::parse`

```cpp
ParseResult parse(std::span<const uint8_t> blob);
```

Validates per the parser acceptance rules (trace-format.md §7) in check
order and returns either the decoded records or a `TraceParseError`
carrying a precise `TraceError` kind (`Truncated`, `BadMagic`, `BadSize`,
`ChecksumMismatch`) plus a message. `ParseResult` is a hand-rolled
std::expected-shaped result (the toolchain floor, GCC 12, predates
`<expected>`). Any failure rejects the whole blob. Record fields are not
validated: unknown `op` bytes and nonzero padding reach the caller
verbatim (§7/§8).

`src/format/trace.hpp::TraceWriter`

```cpp
class TraceWriter {
public:
    TraceWriter();                        // header with checksum = 0
    bool append(const TraceRecord& rec) noexcept;
    size_t record_count() const noexcept;
    uint32_t checksum() const noexcept;   // running raw-chaining CRC-32C
    std::span<const uint8_t> finalize() noexcept;
};
```

Implements the conforming-writer contract (trace-format.md §10): zero
padding bytes, raw-chaining CRC-32C over the records as written, header
present from construction with checksum 0 and rewritten in place by
`finalize()` (mirroring upstream's `PrefetcherImpl::dump`). `append`
enforces §10 rule 4 — `op == 'R'`, `1 <= count <= kMaxRecordCount`,
`offset >= 0` — and returns false (record rejected, blob unchanged) on a
violation. The `finalize()` span borrows the writer; this codec stays
purely in-memory, while live-device recording writes the finalized blob to
an output file in `src/image/trace_record.hpp` / `.cpp`.

## Invariants & Guarantees

- **Byte-exact wire compatibility.** Header/trailer layouts, magics, flag
  bits, index encodings, compression framing, and all CRC flavors match
  upstream OverlayBD bit for bit, in both directions: files written by
  upstream tools load here, and files produced by the fixture writers (and
  by `LsmtRwLayer::seal`) load in upstream readers. Anchors:
  `src/format/zfile_format.hpp::HeaderTrailer`,
  `src/format/lsmt_format.hpp::HeaderTrailer`.
- **Trailer-authoritative open.** For both formats the trailer is the
  effective metadata (ZFile: unless `header_overwrite`); an unsealed or
  mistyped trailer is rejected.
- **Sorted, disjoint indexes.** Every segment index exposed by this module
  (per-layer, merged, writable) is sorted by `offset` and disjoint
  (`a.end() <= b.offset`); merged indexes carry `tag = layer position` with
  0 = topmost.
- **Topmost wins; holes are zero.** Any byte covered by a higher layer
  shadows all lower layers; any byte covered by no layer reads as 0; any
  `zeroed` segment reads as 0.
- **Copy-on-write.** Writes through `MergedWritable` / a `WritableLayer`
  never modify a sealed RO lower's bytes (pinned by test, §Testing).
- **Seal atomicity.** `LsmtRwLayer::seal` publishes the compacted file via
  fsync + atomic rename; a failed seal leaves the original file untouched
  and unsealed.
- **Seal determinism (ADR-0014).** The sealed file is a **pure function of
  the upper's content plus the caller-supplied `user_tag`**: identical
  content and tag seal to identical bytes, no wall-clock, randomness, or
  process-derived fields. Concretely, the sealed header/trailer `uuid` is
  derived from the content digest, replacing the random create-time uuid:

  ```
  content_digest = sha256( virtual_size as LE u64
                           || packed data bytes, in segment order
                           || packed index entries as stored (16B LE each) )
  uuid = content_digest hex chars [0,32) formatted 8-4-4-4-12 (36 chars)
  ```

  `user_tag` is caller input and deliberately excluded from the digest;
  `parent_uuid` stays empty (an RW upper has no recorded parent).
  Stacking is unaffected: neither this stack's merge path
  (`src/format/lsmt.cpp`) nor upstream validates a child's `parent_uuid`
  against the parent's `uuid` — the field is informational. Pinned by
  `format: lsmt rw seal is deterministic for identical content`.
- **Empty-layer determinism (ADR-0014).** A sealed empty layer
  (`create_empty_lsmt_layer`) follows the same rule with no data and no
  index entries — digest = `sha256(vsize as LE u64)` — so identical
  `vsize` writes byte-identical files (pinned by
  `format: empty lsmt layer bytes are deterministic per virtual size`);
  an empty commit (`seal` of a never-written upper, empty `user_tag`)
  reproduces the base exactly.
- **Converter determinism (ADR-0019).** `obd-convert` writes a deterministic
  raw filesystem image first, then calls `write_lsmt_single_layer` with an
  explicit UUID derived from that raw image's SHA-256 digest and a fixed
  converter tag, using a private temporary workspace before atomically
  publishing the final path. The resulting LSMT file is a standard sealed lower;
  pinned by `cli: obd-convert builds a deterministic ext2 layer from tar` and
  `cli: obd-convert defaults to libe2fs when the backend is enabled`.
- **Error channels.** Cold paths (`open`, `parse`, writers) throw
  `obd::format_error` / `obd::error`; hot paths (`pread`/`pwrite`/`flush`)
  return negative -errno and never throw (the `source::BlobSource`
  contract).

## Concurrency & Call Permissions

- **Coroutine context.** Every `elio::coro::task` API here must run on the
  Elio scheduler (or via `elio::run` in tests); they drive the async IO
  backend and never block a worker on a synchronous syscall. The fixture
  writers are the opposite: plain blocking POSIX IO for **plain threads /
  cold paths only** — never call them from an Elio worker.
- **Readers.** `ZFileSource`, `LsmtLayer`, and `MergedLsmt` are immutable
  after `open` returns; concurrent `pread`s on one instance are safe (the
  `source::BlobSource` contract), including from multiple coroutines and
  fixed ublk tag handlers. Accessor-returned references (`header()`,
  `segments()`, `MergedLsmt::merged_index()`, `jump_table()`) borrow the
  instance — they are invalidated by destruction. `MergedWritable::pread()`
  uses an immutable shared index snapshot; `MergedWritable::merged_index()`
  returns a copy because writes can rebuild that index.
- **Writers (layers).** `SparseRwLayer`, `LsmtRwLayer`, and
  `MergedWritable` hold mutable state (segment index, append position,
  seals). Concurrent `pread`s are safe. `SparseRwLayer` internally
  serializes `pwrite` / `discard` / `flush` metadata publication, and
  `MergedWritable` serializes top-layer mutations with merged-index rebuilds
  and publishes immutable merged-index snapshots for reads. Direct
  `MergedWritable::grow()` overlap with `pwrite()`/`discard()`/`flush()`
  returns `-EBUSY`, so a resize cannot overtake a top mutation or
  durability boundary. Callers that
  require application-level read-after-write ordering must still order reads
  of the affected range after the write/discard completion. `LsmtRwLayer`
  deliberately exposes appended data through its atomic-sized
  `data_source()` view so the merged view can read freshly written data.
- **Ownership.** `open`/`create` take ownership of the source chain
  (`BlobSourcePtr`, layers vector). Buffers passed to `pread`/`pwrite` are
  caller-owned and only accessed until the task completes.
- **Preconditions.** `MergedLsmt::pread`, `MergedWritable::pread`/`pwrite`,
  and both writable layers require 512B-aligned `offset`/`count` (`-EINVAL`
  otherwise); `seal()` and post-seal `pwrite` ordering is enforced by
  `-EROFS`.

## Stability Contract

- **Breaking (T1 ADR trigger):** any change to ZFile or LSMT parse/serialize
  semantics — header/trailer field layout, magics, flag-bit meanings, index
  encodings (`u32` block sizes, `SegmentMapping` bit packing), jump-table
  construction, CRC flavors (`crc32c` region/index digests,
  `crc32c_salt(100007)` per-block checksums), or compression framing (raw
  LZ4/ZSTD blocks). Images built by upstream `overlaybd-*` tools must keep
  loading byte-identically. The same applies to files this module produces
  (fixture writers, `seal` output): they must keep loading in upstream
  readers.
- **Breaking (T1):** the writable-layer on-disk artifacts — the unsealed
  LSMT RW header, the checkpoint trailer layout (`checkpoint()`'s index
  region + unsealed trailer consumed by `seal_file`), the sealed output of
  `LsmtRwLayer::seal` (including the content-derived uuid rule), and
  sparse-file extent semantics relied upon at reopen.
- **Contract changes need ADRs.** The writable-layer rules on this page
  (interface shape, in-place edit, seal compaction, copy-on-write) are the
  ADR-0008 contract; weakening or reversing them requires a superseding
  ADR. Changes to this Stability Contract section are themselves a T3 ADR
  trigger. The module also follows the ADR-0001 minimalist-assumptions rule:
  only assumptions its consumers actually rely on are guaranteed here.
- **Not contract:** internal buffer sizes (`kReadWindow`), label strings,
  and in-memory index representations may change freely.

## Testing

Unit tests live in `tests/unit/test_format.cpp`,
`tests/unit/test_trace.cpp`, and
`tests/unit/test_writable.cpp` (binary `obd_unit_tests`, Catch2 tag
`[format]`). Run with `ctest --test-dir build --output-on-failure` or
`./build/tests/obd_unit_tests "[format]"`.

Sparse export source-admission regressions cover:

- `format: sparse existing open refuses a missing source without creation`.
- `format: sparse ownership excludes ordinary and existing opens through aliases`.
- `format: sparse existing open preserves persisted growth and exact ownership`.
- `format: sparse export layout preserves gaps and explicit zero masks`.
- `format: sparse export layout is independent of ownership fragmentation`.
- `format: sparse export layout rejects invalid ownership before output`.
- `format: sparse delta writer preserves parent bytes and deterministic output`.
- `format: sparse export transaction retains a reusable independent source`.
- `format: sparse export transaction rejects source aliases and active owners`.
- `format: sparse export resolves directory symlinks before parent traversal`.
- `format: sparse export recovers completed child operations and grown tails` —
  real helper processes exit without flushing or destructors; checks process
  recovery plus surviving compatibility masks, not host power-loss durability.
- `format: sparse export failures preserve source and retry publication` —
  deterministic partial-payload, index/header/trailer, file-sync, publication,
  and post-publication directory-sync refusals; these are not power-loss tests.

Sparse export recovery regression cases include
`format: sparse export imports legacy zero masks without flattening parents`
and `format: sparse export rejects damaged recovery metadata before publication`.
They pin compatibility-mask import and exact DATA/ZERO/INHERIT composition,
plus refusal without publication or source-artifact mutation when recovery
metadata or the pinned data identity is invalid.

Golden byte values (magics, field offsets, flag bits, CRC check values) come
from the upstream OverlayBD sources and format specs
(`src/overlaybd/zfile/zfile.cpp`, `src/overlaybd/lsmt/file.cpp`,
`docs/specs/zfile_format_spec.md`, `docs/specs/lsmt_format_spec.md`), never
from this project's own writers alone; round-trip tests then prove the
writers and readers agree on the same bytes.

- `format: zfile header bytes match the OverlayBD wire format` — pins the
  golden header bytes of a writer-produced ZFile: magic0 `"ZFile\0\1\0"`,
  magic1 `"tuji.yyf@Alibaba"`, `size = 96`, the header/type/sealed flag
  bits, and `original_file_size` at its documented offset.
- `format: zfile round-trip reads back the original content` — full
  write→open→read round-trip with per-block verify enabled, including an
  unaligned tail read and a past-EOF read (returns 0). Guards jump-table
  construction, batching, and EOF clamping.
- `format: zfile reader rejects a corrupted block digest` — flips one byte
  inside a compressed block and requires `pread` to fail with `-EIO` via the
  `crc32c_salt` check. Guards the per-block verify path end to end.
- `format: lsmt header bytes match the OverlayBD wire format` — pins the
  golden LSMT header bytes: magic0 `"LSMT\0\1\2\0"`, the 16-byte magic1
  UUID, `size = 390`, the header/type/sealed flag bits, and `virtual_size`.
- `format: lsmt round-trip and multi-layer merge semantics` — single-layer
  read (data begins at sector 8), then a two-layer merge where the top
  layer covers the whole device and wins everywhere; also pins the sector
  contract (unaligned read → `-EINVAL`).
- `format: merge falls through holes to lower layers` — direct
  `merge_indexes` test: top covers `[4,8)`, bottom covers `[0,16)`; asserts
  the merged index is bottom/top/bottom with correct tags and that a clipped
  lower segment's `moffset` shifts by the clipped head. Guards gap-merge
  recursion and clipping arithmetic.
- `format: sparse layer writes, reads and recovers extents` — write,
  partial rewrite (identity segments merge into one extent), unaligned-write
  rejection, holes-read-as-zero, then **reopen** and require the same
  exact coverage recovered from v2 metadata. Additional sparse-coverage tests
  pin adjacent lower fall-through, legacy `.zeroes` import, dual-slot
  rollback, corruption/identity rejection, multi-page retry, and grown-tail
  reopen semantics.
- `format: sparse interrupted fresh creation preserves lower sectors` — real
  child-process exits before/after the initial header and before/after data
  expansion preserve every lower byte without fabricating DATA ownership.
- `format: sparse legacy growth leaves new sectors inherited` — importing a
  smaller legacy upper owns only the original EOF range, live and on reopen.
- `format: sparse recovered ZERO stays coherent after partial write failure`
  — a child exits after partial ZERO publication; missing and stale compatibility
  masks preserve the same merged view after a later cross-page write fails,
  with or without a subsequent FLUSH. Retrying the write publishes DATA.
- `format: merged sparse allocation failure preserves committed prefix` — a
  controlled allocation failure after the first large-request piece returns
  `-ENOMEM` and preserves the same live/recovered prefix and untouched lower.
- `format: merged writable refreshes ownership after top exceptions` — an
  exceptional write, discard, or flush publishes its new DATA/ZERO ownership
  to subsequent merged reads while preserving the original exception.
- `format: lsmt rw overwrites covered data in place` — the ADR-0008
  in-place-edit rule: a fully covered rewrite must not grow the file;
  disjoint and straddling writes append exactly their uncovered bytes; the
  index coalesces back to contiguous segments; the patched view reads back
  correctly with zeros in the holes.
- `format: lsmt rw pwrite over a discarded range appends fresh data` —
  rewritten zeroed coverage appends without corrupting a surviving live
  neighbor; reopened virtual reads match, and the sealed digest equals an
  ordinary overwrite with the same final content.
- `format: lsmt rw straddling pwrite keeps live and discarded parts apart` —
  live subranges remain in place, discarded subranges append, and virtual
  reads before and after sealing preserve the remaining zeroed tail.
- `format: lsmt rw partial zeroed rewrites survive offline sealing` —
  pwrite/discard splits preserve valid zeroed placeholders through
  checkpoint/offline seal, including a maximum-length piece boundary;
  reopened virtual reads verify the patch, live neighbor and zero ranges.
- `format: lsmt rw seal compacts into a standard sealed layer` — after
  in-place edits, `seal()` produces the exact sealed geometry (packed data +
  padded index + trailer), post-seal `pwrite` returns `-EROFS`, and the
  sealed file loads through the read-only `LsmtLayer` path with the patched
  content. Guards compaction, atomic rename, and RO compatibility.
- `format: lsmt rw seal rebinds live reads to the compacted inode` — after
  reverse physical allocation and a zeroed segment, successful live `seal()`
  keeps original-layer `pread`, a retained `data_source()` reference, current
  `segments()` plus `data_source()`, and a fresh `LsmtLayer` reopen coherent
  with the compacted published file.
- `format: lsmt rw failed live seal keeps original backing` — an exception
  during live seal serialization removes the unpublished output and keeps the
  original backing descriptor, bytes and live reads intact.
- `format: lsmt rw direct terminal and grow gates reject overlaps` — direct
  overlapping `grow()`/`checkpoint()`/`seal()` calls return `-EBUSY` while
  the in-flight operation retains its gate, and the rejected operation can be
  retried after release.
- `format: lsmt rw seal is deterministic for identical content` — the
  ADR-0014 seal determinism invariant: two uppers with identical write
  sequences seal to byte-identical files (equal sha256), their sealed
  uuids match, and a different write sequence yields a different digest.
  Catches accidental time/random fields in the sealed output.
- `format: lsmt rw destruction releases its backing descriptor` — repeated
  create/read/reset cycles leave no descriptor for the backing device/inode.
- `format: lsmt rw offline seal releases replaced inode descriptors` —
  checkpoint/offline seal releases its reopened old inode and digest descriptors;
  destroying the creator releases its remaining reference, and data round-trips.
- `format: lsmt rw rejected seal releases its reopened descriptor` — unaligned
  and shrinking overrides plus output-open failure preserve the checkpoint and
  release the reopened owner.
- `format: lsmt rw seal exceptions release temporary descriptors` — a header
  serialization exception closes both temporary descriptors and removes the
  compaction file while preserving the original checkpoint.
- `format: lsmt rw setup failures release their descriptors` — staged creation
  failures in owned temporary directories release their descriptors without
  publishing a final data file while a separate live layer remains readable.
- `format: lsmt rw checkpoint persists the index for offline seal` — the
  ADR-0014 offline-commit machinery: `checkpoint()` persists the index
  (writes afterwards get `-EROFS`), `seal_file()` seals the file from a
  fresh open with digest/size reported, the sealed output loads as a valid
  RO layer, and the error channels are precise (`-EALREADY` sealed,
  `-ENOENT` missing, `-EINVAL` legacy upper without a valid checkpoint).
- `format: empty sealed lsmt layer is a zero base of its virtual size` —
  a sealed empty LSMT layer opens as a valid RO layer with the requested
  `virtual_size`, zero segments, minimal 8192-byte geometry
  (`index_offset = 4096`, empty index), and a merged single-layer view
  reads the whole requested range — head and tail — as zeroes. Guards the
  ADR-0014 zero-base format.
- `format: empty lsmt exceptions release output ownership` — an oversized
  tag causes serialization to throw; the helper closes its inode-specific
  descriptor and removes the partial output even during exception unwinding.
- `format: empty lsmt preserves validation and tag boundaries` — invalid
  virtual sizes fail before truncation, missing parents retain the open errno,
  and a 255-byte tag produces a valid empty layer.
- `format: empty lsmt layer bytes are deterministic per virtual size` —
  two empty layers of the same `vsize` are byte-identical; a different
  `vsize` differs. The header uuid is pinned against an independently
  computed `sha256(vsize as LE u64)` digest (the content-derived rule for
  an empty index), not just asserted by self-consistency.
- `format: merged writable falls through and copy-on-writes` — before any
  write the merged view is pure fall-through; after a patch write, reads see
  the patch while the lower blob is verified **byte-identical** afterwards.
  Guards copy-on-write and index rebuild.
- `format: trace crc32c golden vectors match the spec` — pins the
  raw-chaining CRC-32C against the trace-format.md §4 vectors (empty →
  0x00000000, one record → 0xBA691A13, two chained records → 0xD29283DD),
  including the chaining property and the writer's incremental CRC. Golden
  values come from the specification, never from the writer under test.
- `format: trace decodes the spec worked example byte-for-byte` — parses
  the golden 72-byte blob from the trace-format.md §13 appendix (header
  fields at their documented offsets, both records, checksum position) and
  requires the conforming writer to reproduce it byte-for-byte.
- `format: trace writer round-trips through the parser` — 64 records
  written, parsed back to an identical list; the empty trace (header only)
  is a valid 24-byte blob with zero records (§8).
- `format: trace parser rejects corrupt headers and checksums` — the four
  precise failure kinds: truncated header, bad magic, exact-size mismatch
  (both truncated and padded blobs), checksum mismatch (§7).
- `format: trace parser accepts and ignores a non-multiple tail` — a
  `data_size = 25` blob decodes its one full record and ignores the
  unchecksummed trailing byte (§7 rule 4, §12.2).
- `format: trace parser exposes unknown op bytes to the caller` — 'W' and
  arbitrary op bytes parse fine; nonzero record padding is
  checksummed but not interpreted (§7, §8).
- `format: trace writer enforces the conforming-writer contract` — count 0
  and > 1 MiB, op 'W', and negative offsets are rejected; boundary values
  pass; rejected appends leave the blob unchanged (§10 rule 4).
- Cross-module: `image: writable upper assembles and serves writes` covers
  assembly of a `MergedWritable` device root from a config (see
  `docs/image.md`).

## Limitations & TODO

- **MINI_LZO (algo 0) is not supported** — legacy files are rejected at
  open. ZSTD is optional (`OBD_ENABLE_ZSTD`, on by default); without it,
  ZSTD files are rejected.
- **Dictionary compression is rejected** (`use_dict`/`dict_size` nonzero →
  `format_error` at open).
- **ZFile index files** (bit1 clear) and the compressed-index flag (bit5)
  are not supported. Ordinary LSMT `gc_layer` / `sparse_rw` flags are
  parsed but not acted on, and read-only `parent_uuid` chains are not validated.
  Hybrid rejects conflicting layout flags and recognizes unbacked ZERO markers.
- **Legacy LSMT migration requires a checkpoint.** Current pairs recover
  completed writes and ZERO mappings; old memory-only uppers without a valid
  checkpoint cannot reconstruct ownership. Preserve both artifacts when
  copying a stopped writable pair (ADR-0029).
- **Legacy sparse migration freezes the v1 view.** A v1 file did not record
  exact DATA intent, so its one-time extent import can retain an already
  over-wide filesystem-block extent. All writes after v2 publication and all
  fresh uppers recover exactly (ADR-0024).
- **Merged index rebuild is O(index) per write.** `MergedWritable` rebuilds
  after every `pwrite`; read requests reuse the published immutable snapshot
  instead of copying the full index. Write-heavy workloads should batch writes.
- **Trace packaging is external.** The trace codec (`trace.hpp`) is
  implemented and tested; replay is wired into device bring-up
  (`trace_replay.hpp`, ADR-0013) and recording into the supervisor's
  trace commands (`trace_record.hpp`, supervisor protocol v3) — but
  packaging the recorded blob as an image layer stays with the external
  CLI (ADR-0014 boundary).
- **The native raw-image writer has full coverage.** `write_lsmt_single_layer`
  represents every logical input sector (no sparse/zero segments). Transient
  tail placement changes physical order only; it is not a differential writer.

### TurboOCI warp mappings (ADR-0020)

`LsmtLayer::open_warp(metadata, target)` opens a committed warp layer with two
byte sources. Upstream normalization subtracts the minimum surviving mapping
tag: normalized tag 0 addresses filesystem metadata, and tag 1 addresses the
original tar payload. Invalid-offset padding does not participate. A singleton
tag therefore normalizes to metadata; writers must retain a metadata anchor
when emitting remote mappings.

The reader validates nonempty, ordered logical extents against virtual size,
metadata offsets against the metadata data region, and target offsets against
the original tar size. It translates target addresses into a private composite
source space before ordinary layer merging. Merged-layer tags retain their
existing priority meaning. Zero mappings do not fetch payload bytes.

Gzip targets use `GzipIndexSource`: explicit little-endian decoding of the
333-byte `ddgzidx` v1.0 header and 29-byte entries, header CRC32C verification,
32 KiB restart dictionaries, and raw/zlib dictionary and entry-table encodings.
The encoded and decoded entry tables are each limited to 64 MiB. Dictionary
payloads are validated when read; malformed compressed dictionaries return an
I/O error. Every read owns its inflater and restores both the partial-byte
DEFLATE state and dictionary. Index checks do not replace verification of the
original OCI blob's digest.

## Implementation notes

Scheduler-facing writable-file transfers use Elio v0.6.0 positional helpers;
format reads still map early EOF to `-EIO`. LSMT flush/checkpoint/seal and sparse
open/flush/reclamation use Elio file operations, with native support or bounded
blocking fallback. Borrowed descriptors and buffers remain alive until return.
Sparse writes use `pwrite_some` so each completed full-sector prefix is indexed
before the next transfer; this format-specific publication loop is intentional.
Data-before-zero-mask and zero-mask-before-reclamation ordering stays explicit.
Atomic sidecar publication, directory sync, locked trace-start truncation and blocking grow/seal
utilities remain synchronous transactions on their existing execution paths.

Hybrid seal state comes from a complete trailer and self-contained compact index,
for both native and imported artifacts. Header identity, geometry, logical order and
payload bounds must validate; local log attributes cannot override a valid seal.
No terminal-seal xattr is created or required, and old terminal attributes are
ignored for classification. Seal synchronizes its appended metadata before
retiring the recoverable intent. See ADR-0026 for ordering.

Without a complete trailer, an artifact cannot be loaded as a sealed lower,
regardless of xattrs. Reading an unsealed writable pair through its independent
index is a separate API. Complete trailer-shaped payload is classified by its
bytes; matching magic alone is insufficient. Trailer removal to resume writing
is not performed implicitly. All metadata IO runs off scheduler workers.
