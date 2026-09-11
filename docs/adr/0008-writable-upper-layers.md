# ADR-0008: Writable upper layers: sparse file and in-place-edit LSMT-RW

- Status: accepted
- Date: 2026-09-07
- Supersedes: ADR-0007
- Binds: src/format/writable.hpp, src/format/sparse_rw.cpp,
  src/format/lsmt_rw.cpp, src/format/merged_writable.cpp,
  src/image/config.cpp, src/ublk/elio_bridge.cpp,
  docs/design-assumptions.md, docs/format.md, docs/config.md,
  docs/operations.md

## Context

ADR-0007 deferred writable layers to harden the read path first. The read
path is now proven, and writable uppers are an explicit requirement:
containers need a scratch area above the lazy-loaded lowers, and operators
need to choose between a thin scratch file and a format-compatible layer
that can later join the image stack as a sealed lower.

Upstream OverlayBD's LSMT RW implementation is append-only: every pwrite
appends a new data record and inserts a segment that supersedes older ones,
with garbage reclaimed at commit. For a top layer that sees repeated
partial overwrites (fs metadata, journals, databases), append-only
accumulates dead records until seal and grows the RW file without bound.
The requirement here is stricter: when a written range is already covered —
fully or partially — by the writable layer, the covered part must overwrite
the existing data blocks in place; only previously-uncovered subranges may
append. Compaction still happens at seal, when the layer becomes a standard
sealed LSMT RO file.

## Decision

An image may carry a writable upper layer, configured by the per-image
`upper` object (`dir` plus `type`: `lsmt` (default) or `sparse`); the
device then exposes a writable block source whose topmost layer is the
upper, and the LSMT-RW implementation must overwrite already-covered
subranges in place and append only newly-covered subranges, compacting to
a standard sealed LSMT file at seal time.

## Consequences

- Two implementations are bound to one sector-aligned `WritableLayer`
  contract (pwrite/pread/flush plus a segment view for the merger):
  - **sparse**: a sparse file whose live written extents are rebuilt from
    the kernel fiemap (`SEEK_DATA`/`SEEK_HOLE`) on open, paired with a
    small `<path>.zeroes` sidecar for discard zero masks — so live writes
    and lower-layer masks survive restarts;
  - **lsmt**: an unsealed single-file LSMT with in-place edit as decided
    above. Its segment index is memory-only until `seal()`; an unsealed
    file is **not** recoverable across process restarts (creation
    truncates), and durability before seal is a flush-level property only.
    `seal()` compacts live data, writes the standard header/index/trailer,
    fsyncs, and atomically renames — the result loads through the ordinary
    read-only LSMT path and can serve as a lower.
- Writes are copy-on-write: lower layers are never modified. Reads merge
  the writable top over the RO lowers with top-wins semantics and zeroed
  holes, reusing the same merge rules as the read-only stack.
- The ublk data plane forwards `WRITE`/`FLUSH` to the writable root when
  present and keeps answering `-EROFS` otherwise; the device advertises
  `UBLK_ATTR_READ_ONLY` only for read-only images. `DISCARD`/punch-hole
  reaches the writable root when advertised and masks lower layers with
  zeroes.
- Config compatibility: unknown `upper` types are rejected with `EINVAL`;
  an absent or empty `upper` keeps the read-only behavior. The `upper`
  field name and `dir` semantics follow overlaybd-snapshotter convention;
  `type` is an extension field unknown consumers ignore.
- The in-place-edit layout is a deliberate deviation from upstream's
  append-only RW files. It changes only the *unsealed* RW file layout
  (which nothing else consumes); sealed output stays byte-compatible with
  the LSMT format, so upstream tooling can consume sealed layers.
- ADR-0007's "rejected at parse time" rule is lifted exactly for the two
  sanctioned upper types; TurboOCI remains out of scope.

## Alternatives considered

- **Append-only LSMT-RW like upstream** — rejected: unbounded RW file
  growth under repeated partial overwrites until seal; the requirement
  explicitly calls for in-place edit with compaction at seal.
- **Only the sparse layer** — rejected: a sparse scratch file can never
  become a format-compatible lower; the lsmt upper exists so a sealed
  layer rejoins the image stack through ordinary tooling.
- **Persisting the LSMT-RW index incrementally (index file like
  upstream's RW pair)** — deferred, not rejected: it would make unsealed
  RW data crash-recoverable, but adds a second wire artifact; v0.2 keeps
  the memory-only index and documents the durability boundary.
- **Writable layers in the same process as the supervisor** — rejected by
  ADR-0004 independently; writable uppers run inside the per-device child
  like everything else.
