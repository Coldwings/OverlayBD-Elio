# Sparse coverage baseline measurements

These dated measurements describe the legacy coverage implementation, not the
extent-owned replacement in ADR-0037. They remain a comparison baseline; no
replacement performance claim follows from them.

## Validated filesystem coordinates and costs

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

