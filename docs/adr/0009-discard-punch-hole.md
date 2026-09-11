# ADR-0009: Discard/punch-hole with mask-with-zeroes semantics

- Status: accepted
- Date: 2026-09-08
- Supersedes: none
- Amends: ADR-0008 (which left DISCARD unsupported)
- Binds: src/format/writable.hpp, src/format/sparse_rw.cpp,
  src/format/lsmt_rw.cpp, src/format/merged_writable.cpp,
  src/source/blob_source.hpp, src/ublk/elio_bridge.cpp,
  src/ublk/ctrl.cpp, docs/format.md, docs/ublk.md,
  docs/operations.md, docs/design-assumptions.md

## Context

ADR-0008 delivered writable uppers but rejected `DISCARD` with
`EOPNOTSUPP`. Discard is not optional in practice: container workloads
(`fstrim`, filesystem mkfs, overlayfs upper cleanup, database
space reclamation) issue it routinely, and answering `EOPNOTSUPP` both
breaks `fstrim` and — worse — keeps every ever-written block allocated
forever, so the upper file grows monotonically even when the guest has
logically freed the space.

Two design questions needed an explicit answer:

1. **Visibility semantics**: after discarding a range the writable upper
   never wrote (so reads currently fall through to the lowers), should
   reads return the *lower's data* again (punch-through) or *zeroes*
   (mask)? Upstream OverlayBD's LSMT trim inserts zeroed records — mask
   semantics — and punch-through would be surprising: a guest that
   discards its scratch file does not expect long-deleted lower image
   data to reappear. Masking also matches the block-layer contract of
   "discarded blocks read as zeroes" that filesystems already rely on.
2. **Sparse upper granularity**: the sparse layer's segment index is
   sector-granular, but the kernel fiemap used for extent recovery is
   filesystem-block granular (typically 4 KiB). A sub-block punch-hole
   zeroes the range but cannot deallocate the block, so a recovered
   index may be *fatter* than the pre-restart one.

## Decision

Both writable contracts (`WritableLayer`, `WritableBlobSource`) gain
`discard(offset, len)` with **mask-with-zeroes semantics**: a discarded
range reads back as zeroes from this layer onwards and never falls
through to lower layers. Implementations:

- **LSMT-RW** inserts *zeroed segments* (the LSMT format's trim
  mechanism), splitting/trimming any overlapped segments exactly like
  `pwrite`. No data blocks are written; superseded blocks become garbage
  that `seal()` drops. A zeroed segment's `moffset` is never read, but
  must still lie inside the data region (`8 <= moffset <= index_end`)
  because the read-only loader validates the range — `seal()` therefore
  packs zeroed segments with the current data-end position rather than 0.
- **Sparse** performs a real
  `fallocate(FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE)` and records the
  discarded range as zeroed top-layer coverage. The mask is recorded in
  memory and persisted at `flush()`/`checkpoint()` in `<path>.zeroes`, a
  small sidecar containing encoded zeroed segment mappings, so
  `MergedWritable` still masks lowers after reopening a sparse upper.
  Deallocation follows filesystem-block granularity; reads are unaffected
  either way (punched blocks read back as zeroes), so a fatter fiemap
  extent after recovery is a cosmetic difference only.
- **MergedWritable** forwards to the top layer and rebuilds the merged
  index.

On the ublk data plane, `UBLK_IO_OP_DISCARD` maps to `discard()` and
`UBLK_IO_OP_WRITE_ZEROES` maps to `discard()` as well — except with
`UBLK_IO_F_NOUNMAP`, which must not deallocate and therefore becomes a
real write of zeroes through the writable layer. Read-only devices
answer `-EROFS`. `SET_PARAMS` advertises discard/write-zeroes limits
(sector granularity, matching the layer contract) only for writable
devices; read-only devices keep all discard limits at 0 so the kernel
never issues the commands.

## Consequences

- `fstrim` and friends work inside containers on writable devices, and
  the sparse upper can actually return blocks to the filesystem.
- A discard on a range covered only by lower layers permanently hides
  that lower data for the lifetime of the upper (mask semantics); this
  is intentional and matches upstream LSMT trim behavior.
- `seal()` of an LSMT-RW upper that saw discards produces a standard
  LSMT file whose zeroed segments survive: the sealed lower also masks
  anything beneath it, keeping read results identical before and after
  seal.
- The completion result for discard/write-zeroes commands is 0 on
  success (no byte count), per ublk convention for non-read/write ops.
