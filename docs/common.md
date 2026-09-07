# common — shared utilities

Module document for `src/common/`: error types, CRC-32C, SHA-256, and
little-endian byte accessors (including the LSMT `SegmentMapping` encoding).
Shape: `module` (see `docs/README.md`).

## Overview

`src/common/` holds the small, dependency-light utilities every other module
builds on. It sits at the bottom of the dependency graph: nothing in
`src/common/` includes another project module, and everything above it
(format, source, image, ublk, supervisor) may include it.

What it provides today:

- **Error types** (`src/common/errors.hpp`) — the exception channel for
  setup/cold paths (`obd::error`, `obd::format_error`) plus the `verify`
  helper. Hot IO paths do not use these; they return negative errno values
  (see `docs/source.md` for the per-interface contract).
- **CRC-32C** (`src/common/crc32c.hpp`, `src/common/crc32c.cpp`) — the raw
  running CRC-32C that ZFile digests depend on, byte-identical to upstream
  OverlayBD's `src/overlaybd/zfile/crc32/crc32c.cpp`.
- **SHA-256** (`src/common/sha256.hpp`, `src/common/sha256.cpp`) — streaming
  SHA-256 over OpenSSL EVP, used for post-download blob integrity checks
  against the config digest.
- **Little-endian byte accessors** (`src/common/bytes.hpp`) — unaligned
  fixed-width LE load/store helpers and the 16-byte LSMT `SegmentMapping`
  codec, so on-disk parsing is identical on every supported platform.

There is deliberately nothing else here: no logging glue (logging comes from
Elio directly) and no byte-range type (range arithmetic lives inline in the
format module where the units are sectors).

## Concepts

### Two error channels

The project has two disjoint error channels, and `errors.hpp` implements
exactly one of them:

- **Cold paths** (parsing, setup, control) report failures by throwing
  `obd::error` / `obd::format_error`. Every thrown error carries an errno
  value so a process boundary (e.g. the device process reporting failure to
  its supervisor) can map it back to a system error.
- **Hot IO paths** (`BlobSource::pread`, the ublk data plane) return negative
  errno values and never throw.

### Raw running CRC-32C

Upstream OverlayBD's crc32c (Facebook folly lineage) is a **raw running
CRC-32C**: reflected Castagnoli polynomial `0x1EDC6F41`, the seed is passed
through directly, and there is **no initial inversion and no final XOR**.
Consequences that callers must understand:

- `crc32c(data, n)` is `crc32c_extend(data, n, 0)` — not the standard
  Rocksoft-model check value. The textbook check value `0xE3069283` for
  `"123456789"` applies to the init/xorout form; the raw form yields
  `0x58E3FA20`.
- Extending an empty buffer returns the seed unchanged.
- Chaining is associative: extending part B with the CRC of part A equals the
  one-shot CRC over A‖B.

ZFile uses two flavors of this CRC: plain `crc32c` (header/trailer region
digest, index CRC) and the **salted** variant
`crc32c_salt(data, n) = crc32c_extend(data, n, 100007)` for per-block verify
checksums, where `100007` is upstream's `NOI_WELL_KNOWN_PRIME`.

### Little-endian wire encoding

All OverlayBD on-disk structures are little-endian (ZFile format spec, LSMT
format spec). The `bytes.hpp` helpers assemble/disassemble integers byte by
byte instead of type-punning through struct pointers, so parsing behaves
identically on every supported platform and compiler. On little-endian
targets (the only ones supported today) this compiles down to plain unaligned
loads/stores.

### SegmentMapping bit packing

One LSMT index entry is 16 bytes: two little-endian 64-bit words whose bit
layout matches upstream's packed GCC bitfields on LE targets:

```
word0: bits  0..49  offset   (logical offset, 512B sectors)
       bits 50..63  length   (sectors, max 16383)
word1: bits  0..54  moffset  (mapped offset in the layer blob, sectors)
       bit  55      zeroed   (segment reads as zeroes, no data)
       bits 56..63  tag      (runtime: layer index assigned at merge time)
```

## Public API

### `src/common/errors.hpp` — `namespace obd`

`src/common/errors.hpp::error`

```cpp
class error : public std::system_error {
public:
    error(int err, std::string context);
    int errno_value() const noexcept;
};
```

Base class for all errors raised on setup/cold paths. `err` is stored both in
the `std::system_error` base (under `std::generic_category()`) and in
`err_`; `context` becomes the `what()` message. `errno_value()` returns the
original errno for callers that must map the failure back to a system error.

`src/common/errors.hpp::format_error`

```cpp
class format_error : public error {
public:
    explicit format_error(std::string context, int err = EINVAL);
};
```

Raised when on-disk data violates the OverlayBD format: bad magic, bad
checksum, out-of-range index entries, truncated structures. The `what()`
message is prefixed with `"format error: "`. Defaults to `EINVAL` unless a
more precise errno is given (e.g. the ZFile jump-table builder uses `EIO` for
an invalid compressed-size entry and `ERANGE` for a delta overflow).

`src/common/errors.hpp::throw_errno`

```cpp
[[noreturn]] inline void throw_errno(int err, std::string_view context);
```

Throws `obd::error(err, context)`. Never returns.

`src/common/errors.hpp::verify`

```cpp
inline void verify(bool cond, std::string_view context, int err = EINVAL);
```

Throws `obd::error` via `throw_errno` when `cond` is false; a no-op
otherwise. This is the standard cold-path precondition check.

### `src/common/crc32c.hpp` — `namespace obd::crc32`

`src/common/crc32c.hpp::crc32c_extend`

```cpp
uint32_t crc32c_extend(const void* data, size_t nbytes, uint32_t crc);
inline uint32_t crc32c_extend(std::string_view text, uint32_t crc);
```

Raw CRC-32C continuation from `crc` with no pre- or post-conditioning,
matching upstream's `crc32::crc32c_extend`. Empty input (`nbytes == 0`)
returns `crc` unchanged. Dispatches to a hardware path (SSE4.2 on x86-64 via
runtime `__builtin_cpu_supports` detection, or the CRC32 extension on
AArch64 when compiled in) and falls back to `crc32c_sw`; both paths are
bit-identical. O(n) time, O(1) space.

`src/common/crc32c.hpp::crc32c`

```cpp
inline uint32_t crc32c(const void* data, size_t nbytes);
inline uint32_t crc32c(std::string_view text);
```

Raw CRC-32C from seed 0, matching upstream's `crc32::crc32c`.

`src/common/crc32c.hpp::crc32c_salt`

```cpp
inline uint32_t crc32c_salt(const void* data, size_t nbytes);
```

The ZFile per-block checksum: `crc32c_extend(data, nbytes, 100007)`
(`NOI_WELL_KNOWN_PRIME`, upstream `zfile.cpp`'s `crc32c_salt`).

`src/common/crc32c.hpp::crc32c_sw`

```cpp
uint32_t crc32c_sw(const void* data, size_t nbytes, uint32_t crc);
```

Software table-driven reference (slice-by-1 over the Rocksoft-model table for
reflected poly `0x1EDC6F41`). Always available; used by tests to pin
semantics independently of the hardware path.

### `src/common/sha256.hpp` — `namespace obd::common`

`src/common/sha256.hpp::Sha256`

```cpp
class Sha256 {
public:
    Sha256();                                  // throws obd::error on EVP failure
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(const void* data, size_t size);
    std::string final_hex();
    static std::string hex(const void* data, size_t size);
};
```

Streaming SHA-256 via OpenSSL EVP (OpenSSL is already linked through Elio's
TLS support). Used for download integrity verification, matching upstream's
post-download sha256 check against the config digest.

- The constructor throws `obd::error(EIO)` if the OpenSSL context cannot be
  created or initialized.
- `update` feeds bytes; throws `obd::error(EIO)` on EVP failure.
- `final_hex` finalizes and returns the digest as 64 lowercase hex
  characters. The object must not be used afterwards.
- `hex` is the one-shot form: digest of a whole buffer as lowercase hex.

### `src/common/bytes.hpp` — `namespace obd::bytes`

`src/common/bytes.hpp::load_u16_le`, `src/common/bytes.hpp::load_u32_le`,
`src/common/bytes.hpp::load_u64_le`

```cpp
inline uint16_t load_u16_le(const void* p);
inline uint32_t load_u32_le(const void* p);
inline uint64_t load_u64_le(const void* p);
```

Load a little-endian 16/32/64-bit integer from an unaligned raw buffer. `p`
must point at 2/4/8 readable bytes; there is no bounds checking.

`src/common/bytes.hpp::store_u16_le`, `src/common/bytes.hpp::store_u32_le`,
`src/common/bytes.hpp::store_u64_le`

```cpp
inline void store_u16_le(void* p, uint16_t v);
inline void store_u32_le(void* p, uint32_t v);
inline void store_u64_le(void* p, uint64_t v);
```

Store an integer little-endian into an unaligned raw buffer.

`src/common/bytes.hpp::segment_mapping`

```cpp
struct segment_mapping {
    uint64_t offset = 0;    // logical offset in 512B sectors (50 bits)
    uint32_t length = 0;    // length in sectors (14 bits, max 16383)
    uint64_t moffset = 0;   // blob offset in sectors (55 bits)
    bool zeroed = false;    // zero-filled segment
    uint8_t tag = 0;        // layer index (assigned at merge time)

    static constexpr uint64_t kMaxOffset    = (uint64_t(1) << 50) - 1;
    static constexpr uint32_t kMaxLength    = (1u << 14) - 1;
    static constexpr uint64_t kMaxMoffset   = (uint64_t(1) << 55) - 1;
    static constexpr uint64_t kInvalidOffset = kMaxOffset;
    static constexpr size_t kEncodedSize    = 16;

    uint64_t end() const;   // offset + length
    uint64_t mend() const;  // zeroed ? moffset : moffset + length
};
```

In-memory form of one LSMT index entry. `kInvalidOffset` marks a dropped
on-disk entry (readers filter such entries out at load). `mend()` returns the
end of the mapped range for non-zeroed segments; for zeroed segments there is
no data, so `mend() == moffset`.

`src/common/bytes.hpp::load_segment_le`

```cpp
inline segment_mapping load_segment_le(const void* p);
```

Decodes one 16-byte on-disk entry (`p` must point at 16 readable bytes) using
the bit packing from §Concepts. Bits beyond each field's width are masked
off.

`src/common/bytes.hpp::store_segment_le`

```cpp
inline void store_segment_le(void* p, const segment_mapping& s);
```

Encodes `s` into 16 bytes. Field values are masked to their bit widths
(oversized values are truncated, not rejected).

## Invariants & Guarantees

- **Byte-exact CRC semantics.** `crc32c`, `crc32c_extend`, and `crc32c_salt`
  are bit-identical to upstream OverlayBD's `src/overlaybd/zfile/crc32/crc32c.cpp`
  on every input, on every platform, via either the hardware or the software
  path. This is a wire-format guarantee: ZFile digests must match bit for
  bit. Anchor: `src/common/crc32c.hpp::crc32c_extend`.
- **Chaining law.** For any buffers A and B,
  `crc32c_extend(B, crc32c(A)) == crc32c(A‖B)`.
- **Seed passthrough.** `crc32c_extend(data, 0, seed) == seed`; there is no
  hidden inversion anywhere in the pipeline.
- **Platform-independent parsing.** The `bytes.hpp` helpers never type-pun
  through struct pointers; parse results depend only on the input bytes, not
  on host alignment, padding, or (on supported targets) endianness behavior.
  Anchor: `src/common/bytes.hpp::load_u64_le`.
- **SegmentMapping round-trip.** `load_segment_le(store_segment_le(s)) == s`
  for any `s` whose fields fit their bit widths. Anchor:
  `src/common/bytes.hpp::segment_mapping`.
- **Errors carry errno.** Every `obd::error` (and therefore every
  `format_error`) exposes a meaningful `errno_value()`; `format_error`
  defaults to `EINVAL`.

## Concurrency & Call Permissions

- All `obd::crc32` functions and all `obd::bytes` helpers are **pure
  functions**: no instance state, no mutable globals, safe to call from any
  thread or coroutine, concurrently. The CRC hardware-availability probe is a
  function-local `static const bool` (x86), initialized once in a
  thread-safe manner.
- None of these functions mutate their inputs or take ownership of buffers.
- `Sha256` is a **stateful, non-copyable object**: not thread-safe; one
  instance must be driven from one thread/coroutine at a time, in the order
  constructor → `update`* → `final_hex`. After `final_hex` the object is
  spent. The OpenSSL context is owned by the instance and freed in the
  destructor.
- None of this module is coroutine-aware: everything here is synchronous CPU
  work and may run on an Elio worker, a queue thread, or a plain thread.
  `Sha256` performs no IO.
- These are cold/support utilities: throwing (`obd::error`, `format_error`)
  is permitted here, but callers on hot IO paths must not let these
  exceptions escape — hot paths use negative-errno returns instead.

## Stability Contract

- **Wire-format load-bearing (breaking, T1 ADR trigger):** the CRC-32C
  semantics (polynomial, raw seed passthrough, no inversion/XOR, salt seed
  100007) and the `SegmentMapping` 16-byte bit layout. Any change to either
  silently breaks compatibility with images built by upstream
  `overlaybd-*` tools. The layout and CRC flavors are pinned by golden-value
  tests (§Testing).
- **Internal API (source-compatible evolution):** `obd::error` /
  `format_error` / `throw_errno` / `verify`, the `bytes.hpp` load/store
  helpers, and `Sha256`. These may evolve with the codebase; signature
  changes require updating callers but no ADR.
- Per the ADR-0001 rule, this module keeps only the minimalist assumptions
  its consumers actually rely on; adding a new cross-module convention here
  (e.g. a shared logging facility) is a T4 ADR trigger.

## Testing

Unit tests live in `tests/unit/test_common.cpp` (binary `obd_unit_tests`,
Catch2 tag `[common]`). Run with
`ctest --test-dir build --output-on-failure` or
`./build/tests/obd_unit_tests "[common]"`.

- `common: crc32c matches the OverlayBD raw running CRC` — pins the raw
  running-CRC semantics against **golden values**: `crc32c("123456789") ==
  0x58E3FA20` and `crc32c_salt("123456789") == 0x3C9BD4E0`. The golden values
  come from upstream OverlayBD's crc32c
  (`src/overlaybd/zfile/crc32/crc32c.cpp`, whose documented raw form the
  header comment of `src/common/crc32c.hpp` restates) and were
  **cross-validated by an independent table implementation**, not generated
  by this project's own code. The test also pins the seed-passthrough law
  (empty input returns the seed), the chaining law (extend over parts equals
  one-shot over the concatenation), and that `crc32c_salt` is exactly
  `crc32c_extend(·, 100007)`.
- `common: little-endian load/store round-trips` — pins the byte order of
  the LE helpers (e.g. `store_u16_le(0xBEEF)` produces bytes `EF BE`) and
  load/store round-trips for all three widths.
- `common: segment_mapping encodes OverlayBD bit layout` — pins the golden
  bit layout `word0 = offset | length<<50`, `word1 = moffset | zeroed<<55 |
  tag<<56` with explicit constants, plus decode/encode round-trip and
  `end()` semantics. The layout is taken from upstream OverlayBD's packed
  bitfields (LSMT format spec), not from this project's own writer.
- (Adjacent: `common: base64 round-trips and decodes cred.json form` lives in
  the same file but covers `src/source/base64.hpp`; see `docs/source.md`.)

## Limitations & TODO

- **Little-endian targets only.** The byte-assembly helpers are correct on
  any host, but the project only supports little-endian platforms today; big
  endian support would require revisiting code outside this module too.
- **CRC hardware dispatch is coarse.** x86 uses SSE4.2 (runtime-detected);
  AArch64 uses the CRC32 extension when the compiler target enables it;
  everything else falls back to the slice-by-1 software table. There is no
  PCLMULQDQ folding path — throughput is adequate for 4 KiB ZFile blocks but
  not tuned beyond that.
- **`Sha256` is single-use.** No reset/re-init after `final_hex`; callers
  needing many digests construct one object per digest (or use `hex`).
- **No logging glue here.** Logging is Elio's; if a shared logging
  convention is ever needed it would be a new module and a T4 ADR.
