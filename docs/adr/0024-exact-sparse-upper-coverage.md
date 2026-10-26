# ADR-0024: Persist exact sector coverage for sparse uppers

- Status: accepted
- Date: 2026-10-22
- Supersedes: none
- Amends: ADR-0009 (replaces its sparse recovery mechanism while preserving
  mask-with-zeroes semantics)
- Binds: src/format/sparse_rw.cpp, src/format/sparse_rw.hpp,
  src/format/writable.hpp, src/format/merged_writable.cpp, docs/design-assumptions.md,
  docs/architecture.md, docs/config.md, docs/format.md, docs/image.md,
  docs/operations.md, docs/testing.md, docs/ublk.md

## Context

A sparse upper has three states for every 512-byte sector: INHERIT reads the
lower layer, DATA reads bytes from the upper file, and ZERO masks the lower
with zeroes. The backing filesystem exposes allocation extents, not these
application states. ext4 and XFS both report a single 4 KiB extent after a
512-byte write, so reconstructing DATA from `SEEK_DATA`/`SEEK_HOLE` or FIEMAP
hides seven adjacent lower sectors after a device-process restart. FIEMAP's
UNWRITTEN flag also cannot distinguish an intentional ZERO from filesystem
preallocation.

Sparse uppers must survive a device-server process crash while the kernel and
page cache remain alive. Whole-machine power-loss durability is required only
after a successful guest FLUSH. Existing `.zeroes` files must remain readable
for migration and rolling compatibility.

## Decision

Make a versioned, checksummed `<upper>.coverage` artifact authoritative for
sector-granular DATA and ZERO ownership; publish backing bytes before DATA
coverage, publish ZERO coverage before reclamation, and sync the data file
before the coverage artifact at every successful guest FLUSH.

The artifact is a sparse two-bit-per-sector map. One logical metadata page
describes 8 MiB of virtual device space and has two generation-numbered slots.
A writer clears the inactive slot's commit word, writes its complete body and
CRC, then writes the commit word last. Recovery selects the newest valid slot.
An uncommitted or checksum-invalid slot falls back to the other valid slot.
If recognized committed candidates are corrupt and no valid slot survives,
recovery fails closed. A page with no recognized commit marker uses its
implicit initial INHERIT state. Corruption of the sole first-generation commit
marker is indistinguishable from interrupted first publication; this format
does not promise detection of arbitrary marker corruption. Sparse holes mean
INHERIT.

Each modifying request writes the backing data or zero operation first where
required. Before publishing a page it allocates that page's complete runtime
index replacement; after the commit word becomes visible it performs only a
non-throwing swap and scalar updates. A crash partway through a multi-page publication may
leave a prefix of the uncompleted request visible; ublk USER_RECOVERY reissues
the whole idempotent request. A returned metadata error has the same partial
write boundary. Guest FLUSH first synchronizes the data file and compatibility
zero-mask changes, then synchronizes coverage.

The header binds coverage to a random 128-bit upper identity stored in a user
xattr. When the header requires that marker, it is the strong identity and a
copy or atomic replacement that preserves it may change device/inode. On a
filesystem that cannot store user xattrs, device and inode form the documented
weaker fallback binding; replacing the data inode still fails closed, while
inode reuse after removal is outside that fallback's guarantee.

When coverage is absent, open imports the currently observable legacy view
once: it enumerates `SEEK_DATA`/`SEEK_HOLE`, overlays `.zeroes`, then atomically
installs and directory-syncs v2 coverage before serving I/O. This migration
freezes existing behavior but cannot recover historical sub-block intent that
v1 never recorded. With v2 present, recovery loads it first and applies
`.zeroes` as a protective ZERO-only overlay; extent enumeration no longer
widens ownership.

Creation and migration use `<upper>.coverage.tmp`. Legacy import scans only the
original data-file EOF. The implementation writes, syncs, and fully validates
the temporary artifact before expanding the data file, installs the xattr marker,
syncs the data inode so the marker and file size precede publication, renames
the artifact, then syncs its parent directory. If a marker exists while the
final artifact is missing, open resumes only a fully valid temporary artifact;
without one it fails closed. A pre-existing zero-length data inode is never
classified as newly created, so its metadata is not silently removed.

The initial header has its snapshot-complete flag clear. Only after every
initial page is written does the creator set flag bit 2, refresh the header
CRC and sync the temporary. An incomplete unmarked temporary is discarded and
the legacy import restarts; absent snapshot pages cannot be mistaken for
INHERIT. Incomplete fresh creation leaves a zero-length data file, so even a
filesystem that allocates zero-filled expansion cannot fabricate guest DATA
on restart. A complete installing snapshot may declare a size beyond the
current data EOF, but all DATA sectors must remain within that EOF. Before a
matching xattr marker exists, a complete temporary must match
the recorded device/inode. A recovered complete temporary is synchronized before
marker installation. Every reopened coverage file is potentially dirty until
the replacement process successfully flushes it, including without new writes.

## Consequences

- A 512-byte DATA write and adjacent INHERIT sectors remain distinct across a
  process restart on ext4, XFS, and filesystems with different extent rules.
- Writes that change ownership update one 4160-byte inactive metadata slot per affected
  8 MiB region, plus an eight-byte commit write. This costs metadata I/O but
  avoids full-sidecar rewrites and lower-layer copy-up.
- A write wholly within DATA keeps the existing mapping and coverage bytes;
  it performs only the backing write after pending ZERO transitions settle.
- The sparse artifact's virtual size is roughly 1 GiB per TiB of device
  because every logical page reserves two slots. Untouched pages remain holes;
  recovery enumerates allocated metadata ranges and falls back to a full scan
  only where `SEEK_DATA` is unavailable for the metadata file.
- `.zeroes` remains a compatibility artifact and always wins as ZERO during
  reopen. New exact DATA recovery does not depend on it.
- An old binary that ignores `.coverage` cannot safely open a v2 upper: it can
  widen DATA to filesystem allocation extents and lose exact INHERIT intent.
  Downgrade requires exporting or recreating the upper in an old-format-aware
  workflow; merely retaining `.zeroes` is insufficient.
- On a filesystem without user xattrs, deleting every coverage sidecar while
  leaving the data inode intact is indistinguishable from a genuine v1 upper.
  The documented device+inode fallback detects mismatched surviving sidecars,
  but cannot detect loss of all sidecars and will attempt legacy import.
- Corrupt authoritative headers, identity mismatches, invalid sector states,
  and corrupt recognized committed pages without a valid rollback stop open.
- Buffered 4 KiB first writes still pay for ownership publication. Local
  controls measured about 70 versus 23 CPU microseconds per request; DATA
  rewrites and reads do not republish ownership. The dated filesystem matrix,
  recovery costs, and measurement limits are recorded in
  [format.md](../format.md#validated-filesystem-coordinates-and-costs).

## Alternatives considered

- FIEMAP plus ZERO_RANGE cannot recover guest intent: written extents are
  filesystem-block granular and UNWRITTEN extents carry no origin tag.
- The selected metadata design does not invoke FIEMAP or ZERO_RANGE. Their
  absence therefore needs no physical-zero fallback to reconstruct ownership:
  the persisted ZERO state is authoritative even when physical hole punching
  is unavailable. The actual-zero fallback proposed for the extent-discovery
  alternative is replaced by that ownership record. NOUNMAP keeps its actual
  zero-write path; logical DISCARD does not promise zeroed backing bytes.
- Copying a complete filesystem block from the merged view before every
  partial write would make extent-wide DATA truthful, but adds lower reads and
  write amplification to the latency-sensitive path and makes filesystem
  allocation units part of the format contract.
- Requiring recovery-unit-aligned guest I/O would break the existing 512-byte
  block contract.
- A single bitmap slot cannot distinguish an interrupted page rewrite from
  corruption. An append log avoids fixed virtual address space but needs
  replay bounds and compaction; dual sparse slots provide bounded recovery
  with a local rollback point.
