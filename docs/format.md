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
- **Writable layers** (`writable.hpp`, `sparse_rw.hpp`, `lsmt_rw.hpp`,
  `merged_writable.hpp`) — the ADR-0008 writable top of an image stack:
  `SparseRwLayer` (sparse file, fiemap recovery), `LsmtRwLayer` (unsealed
  single-file LSMT with in-place edit and seal compaction), and
  `MergedWritable` (copy-on-write merged view implementing
  `source::WritableBlobSource`).
- **Fixture writers** (`writer.hpp`) — synchronous cold-path writers that
  produce upstream-readable sealed ZFile and LSMT files; used by
  `obd-mkimage` and the test fixtures. The data plane never writes through
  these.
- **Trace codec** (`trace.hpp`) — the upstream OverlayBD prefetch trace
  blob (ADR-0013, proposed): an in-memory parser and conforming writer for
  the raw-struct wire format specified in
  [trace-format.md](./trace-format.md). The codec exists and is tested;
  record/replay against live devices is **not wired yet**.

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
  `sealed`, bit3 `gc_layer`, bit4 `sparse_rw`, bit5 `info_valid`. Only
  header/type/sealed are enforced by the reader.
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
`virtual_size`.

### Writable layers (ADR-0008)

A writable image has one writable top layer above zero or more sealed RO
lowers. Writes always land in the top (copy-on-write — lowers are never
modified); reads walk a merged index rebuilt after every write. Two top-layer
implementations exist: a sparse file with identity mapping
(`SparseRwLayer`), and an unsealed single-file LSMT with in-place edit
(`LsmtRwLayer`), which `seal()` compacts into a standard sealed LSMT RO
file.

### Trace blob (ADR-0013, proposed)

The prefetch trace blob is a 24-byte header (magic, `data_size`,
CRC-32C checksum) followed by fixed 24-byte records, an LP64
little-endian raw struct image with padding bytes included in the
checksum. The byte-level authority is
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
`kFlagShiftGcLayer` (3), `kFlagShiftSparseRw` (4), `kFlagShiftInfoValid` (5).

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
    virtual const std::vector<bytes::segment_mapping>& segments() const = 0;
    virtual source::BlobSource& data_source() = 0;
};
```

Sector-aligned writable layer (ADR-0008): the topmost layer of an image
stack. `pwrite`/`pread` offsets and counts must be 512-byte multiples (the
same alignment contract as the read side); both return the byte count or a
negative -errno. `pread` reads through this layer alone — holes read as
zeroes and fall-through to lower layers is the merger's job. `flush` is the
durability point (ublk FLUSH). `discard` (ADR-0009) masks a 512B-aligned
range with zeroes — reads of the range return zeroes from this layer
onwards and never fall through to lower layers. `checkpoint()` (ADR-0014)
persists whatever on-disk state an offline seal needs, without sealing; it
is called once by the device process on graceful shutdown after IO has
drained and is terminal (no `pwrite`/`discard` may follow). `segments()` is
the current index: sorted, disjoint, 512B sector units, tag 0.
`data_source()` is the file view segment data is read from.

### `src/format/sparse_rw.hpp` — `SparseRwLayer`

`src/format/sparse_rw.hpp::SparseRwLayer`

```cpp
class SparseRwLayer final : public WritableLayer {
public:
    static elio::coro::task<std::unique_ptr<SparseRwLayer>> open(
        const std::string& path, uint64_t vsize);
    // + WritableLayer overrides
};
```

A sparse file whose written extents form the layer's segment index with
**identity mapping** (`moffset == offset`). `open` requires `vsize` to be a
non-zero multiple of 512 (throws `obd::error(EINVAL)` otherwise), opens
`path` with `O_RDWR | O_CREAT` (existing content is kept — **no
truncation**), sizes it to `vsize` with `ftruncate`, and, for a pre-existing
file, rebuilds coverage from the kernel fiemap (`SEEK_DATA`/`SEEK_HOLE`),
rounding extent boundaries outward to whole sectors (only whole sectors are
ever written). Filesystem errors throw `obd::error`.

`pwrite`/`pread` return `-EINVAL` on unaligned or out-of-`vsize` requests;
writes are split at the 14-bit segment-length cap and merged into the
identity segment set (overlapping and adjacent extents coalesce).
`flush()` is `fdatasync` and returns 0 or `-errno`.

### `src/format/lsmt_rw.hpp` — `LsmtRwLayer`

`src/format/lsmt_rw.hpp::LsmtRwLayer`

```cpp
class LsmtRwLayer final : public WritableLayer {
public:
    ~LsmtRwLayer() override;

    static elio::coro::task<std::unique_ptr<LsmtRwLayer>> create(
        const std::string& path, uint64_t vsize);

    // + WritableLayer overrides

    bool sealed() const noexcept;
    elio::coro::task<int> seal(const std::string& user_tag = "");
    static elio::coro::task<int> seal_file(const std::string& path,
                                           const std::string& user_tag,
                                           std::string* sha256_hex,
                                           uint64_t* size);
};
```

An unsealed single-file LSMT with **in-place edit** (ADR-0008):

- `create` requires a non-zero sector-aligned `vsize` (throws
  `obd::error(EINVAL)` otherwise) and creates `path` with
  `O_RDWR | O_CREAT | O_TRUNC` — **any existing file is truncated** (v0.2
  limitation, see below). It writes an unsealed header and generates a fresh
  layer UUID.
- `pwrite`: for each ≤ 16383-sector piece of the request, subranges already
  covered by the layer's segment index **overwrite their data blocks in
  place**; previously-uncovered subranges **append** at the data end. The
  in-memory index is updated accordingly (old coverage split/trimmed, new
  subranges inserted and coalesced with contiguous neighbors). Returns
  `count`, `-EINVAL` on unaligned/empty/out-of-`vsize` requests, `-EROFS`
  once sealed, or a propagated -errno.
- `pread`: sector-aligned; holes and zeroed segments read as zeroes; clamped
  at `virtual_size()`.
- `flush()`: `fdatasync`; 0 or `-errno`.
- `discard` (ADR-0009): a real
  `fallocate(FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE)` plus a
  split/trim of the extent index. Note the granularity caveat: fiemap is
  filesystem-block granular, so a sub-block punch zeroes but cannot
  deallocate — a reopened index may be fatter than the pre-restart one,
  with identical reads (punched blocks read back as zeroes).
- `discard` (ADR-0009): inserts **zeroed segments** covering the range
  (split/trimming overlapped segments exactly like `pwrite`); no data is
  written and superseded blocks become garbage that `seal()` drops. Zeroed
  segments carry a valid in-data-region `moffset` (never read, but the
  read-only loader validates the range), so a sealed file keeps them
  intact. `-EINVAL` on unaligned/out-of-`vsize` ranges, `-EROFS` once
  sealed.
- `data_source()`: an fd-backed `BlobSource` view whose size **tracks
  appends** (an `std::atomic<uint64_t>` upper bound), unlike a
  `LocalFileSource` which pins `st_size` at open.
- `checkpoint()` (ADR-0014): appends the in-memory segment index plus an
  **unsealed trailer** at the data end (index region padded to 4096B,
  trailer in the file's last 4096 bytes) and fdatasyncs. Called by
  obd-device on graceful shutdown; it is terminal — `pwrite`/`discard`
  afterwards return `-EROFS`, as does a second `checkpoint()`. This is the
  only on-disk persistence of the RW index; a crash before it loses the
  unsealed writes.
- `seal(user_tag)`: compacts the file into a **standard sealed LSMT RO
  file**: live segments are copied out packed sequentially into
  `<path>.sealing.<pid>` (a per-process tmp name — two seals never
  interleave writes; garbage left behind by in-place edits and discards is
  dropped; zeroed segments consume no data space),
  followed by the padded index, a sealed header and trailer, `fdatasync`,
  and an **atomic rename** over `path`. Afterwards `sealed()` is true and
  `pwrite` returns `-EROFS`. Returns 0 or a negative -errno; on failure the
  temp file is unlinked and the original file is untouched.
- `seal_file(path, user_tag, sha256_hex, size)` (ADR-0014 offline commit):
  opens a **checkpointed** RW file without truncating (index loaded from
  the on-disk unsealed trailer, validated with the `LsmtLayer::open`
  rules, and the trailer's uuid/virtual_size cross-checked against the
  on-disk header at offset 0 — a trailer torn mid-write or forged cannot
  seal an empty or wrong layer), seals it in place, and reports the
  sealed file's sha256 hex digest and byte size. Used by the supervisor's `commit` command after
  the device process has exited. Error channels: `-ENOENT` (missing
  file), `-EALREADY` (already sealed), `-EINVAL` (not a valid checkpointed
  LSMT-RW file — e.g. the device crashed before checkpointing), other
  -errno propagated.
- **v0.2 limitation**: the segment index is memory-only until
  `checkpoint()` or `seal()`; an unsealed RW file is **not recoverable
  across process restarts** (`create()` truncates). Checkpoint on graceful
  shutdown and seal (possibly offline, via `seal_file`) to persist.

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
    const std::vector<bytes::segment_mapping>& merged_index() const noexcept;
};
```

The copy-on-write merged view (ADR-0008): sealed RO lowers (may be empty)
plus a writable top layer, as one block source.

- `open` throws `obd::error(EINVAL)` on a null `top`. The device virtual
  size is `max(top vsize, lowers vsize)`. Internally the stack is topmost
  first, tag 0 = writable top.
- `pread` walks the merged index exactly like `MergedLsmt::pread`
  (sector-aligned or `-EINVAL`; top wins; holes zero; short layer read
  retried once then zero-filled; clamped at `size()`).
- `pwrite` validates alignment and bounds, delegates to
  `top_->pwrite` — **RO lowers are never modified** — and then rebuilds the
  merged index (`rebuild_index()`). Write-heavy workloads should batch,
  since the rebuild is O(index size) per write.
- `flush()` delegates to the top layer's `flush()`.
- `discard()` (ADR-0009) delegates to the top layer and rebuilds the index;
  the discarded range then reads as zeroes even when lower layers have data
  there (mask semantics, matching upstream LSMT trim).
- As a `source::WritableBlobSource`, this is the device root the ublk bridge
  dispatches WRITE/FLUSH/DISCARD/WRITE_ZEROES to; a read-only image root
  simply does not implement the interface and writes/discards get `-EROFS`.

### `src/format/writer.hpp` — fixture writers (`namespace obd::format`)

`src/format/writer.hpp::generate_uuid`

```cpp
std::string generate_uuid();
```

Generates a random lowercase 8-4-4-4-12 UUID string (36 chars) from
`std::random_device`, with RFC-4122-ish version/variant nibbles (cosmetic;
upstream treats the field as an opaque string).

```cpp
struct LsmtWriteOptions {
    std::string uuid;         // generated when empty
    std::string parent_uuid;  // may be empty (bottom layer)
    std::string user_tag;     // commit message, may be empty
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
upstream overlaybd readers. Used by `obd-mkimage` and the test fixtures;
the data plane never writes.

### `src/format/trace.hpp` — `namespace obd::format::trace`

Dependency-free codec for the upstream prefetch trace blob (ADR-0013,
proposed; wire authority: [trace-format.md](./trace-format.md)). Pure
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
violation. The `finalize()` span borrows the writer; a memory buffer is
the whole deliverable here, file writing lands with the record/replay
features (not yet implemented).

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
  ublk queue threads. Accessor-returned references (`header()`,
  `segments()`, `merged_index()`, `jump_table()`) borrow the instance —
  they are invalidated by destruction.
- **Writers (layers).** `SparseRwLayer`, `LsmtRwLayer`, and
  `MergedWritable` hold mutable state (segment index, append position,
  seals). Concurrent `pread`s are safe, but `pwrite` / `flush` / `seal`
  must be **externally serialized** against each other and against reads of
  the affected range — the layers do not lock (the ublk bridge serializes
  WRITE/FLUSH; `MergedWritable` rebuilds its index synchronously inside each
  `pwrite`, so overlapping `pwrite` coroutines on one instance are not
  supported). `LsmtRwLayer` deliberately exposes appended data through its
  atomic-sized `data_source()` view so the merged view can read freshly
  written data.
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
  coverage recovered from the fiemap. Guards extent merging and
  SEEK_DATA/SEEK_HOLE recovery.
- `format: lsmt rw overwrites covered data in place` — the ADR-0008
  in-place-edit rule: a fully covered rewrite must not grow the file;
  disjoint and straddling writes append exactly their uncovered bytes; the
  index coalesces back to contiguous segments; the patched view reads back
  correctly with zeros in the holes.
- `format: lsmt rw seal compacts into a standard sealed layer` — after
  in-place edits, `seal()` produces the exact sealed geometry (packed data +
  padded index + trailer), post-seal `pwrite` returns `-EROFS`, and the
  sealed file loads through the read-only `LsmtLayer` path with the patched
  content. Guards compaction, atomic rename, and RO compatibility.
- `format: lsmt rw seal is deterministic for identical content` — the
  ADR-0014 seal determinism invariant: two uppers with identical write
  sequences seal to byte-identical files (equal sha256), their sealed
  uuids match, and a different write sequence yields a different digest.
  Catches accidental time/random fields in the sealed output.
- `format: lsmt rw checkpoint persists the index for offline seal` — the
  ADR-0014 offline-commit machinery: `checkpoint()` persists the index
  (writes afterwards get `-EROFS`), `seal_file()` seals the file from a
  fresh open with digest/size reported, the sealed output loads as a valid
  RO layer, and the error channels are precise (`-EALREADY` sealed,
  `-ENOENT` missing, `-EINVAL` never checkpointed).
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
  arbitrary op bytes parse fine; nonzero padding is checksummed but not
  interpreted (§7, §8).
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
  are not supported; LSMT `gc_layer` / `sparse_rw` / `info_valid` flags are
  parsed but not acted on, and `parent_uuid` chains are not validated.
- **`LsmtRwLayer` v0.2:** the segment index is memory-only until
  `checkpoint()` or `seal()`; an unsealed RW file is not recoverable as a
  writable layer across restarts (`create()` truncates any existing file),
  and a crash before the graceful-shutdown checkpoint loses the unsealed
  writes. Full RW restart recovery is future work.
- **`SparseRwLayer` extent granularity:** fiemap extent boundaries are
  rounded outward to whole sectors on recovery, so a filesystem that splits
  extents sub-sector could mark unwritten sectors covered (harmless: they
  read back as the on-disk zeros).
- **Merged index rebuild is O(index) per write.** `MergedWritable` rebuilds
  after every `pwrite`; write-heavy workloads should batch writes.
- **Trace record/replay is not wired.** The trace codec (`trace.hpp`) is
  implemented and tested, but no device path records or replays traces yet,
  and the blob is not packaged as an image layer (ADR-0013, proposed).
- **Writers are single-shot fixtures.** `write_lsmt_single_layer` covers the
  whole input contiguously (no sparse/zero segments); general-purpose image
  authoring belongs to upstream tools.
