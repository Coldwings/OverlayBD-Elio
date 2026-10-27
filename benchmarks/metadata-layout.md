# Native LSMT metadata placement measurements

These measurements evaluate `--lsmt-layout metadata-tail` against the unchanged
`logical` layout. **Keep logical placement as the default.** Physical clustering
does not establish a startup benefit, and inline structural warm-up can cost more
than the later command saves. This report records regressions as well as gains.

## Method and scope

The 2026-10-27 controlled matrix contains 144 independent guest trials: two
8 MiB filesystem trees, single and two-layer views, both layouts, raw/LZ4/ZSTD
outer encodings, structural warm-up off/on, and three repetitions per cell.
The metadata tree has 128 directories; the ordinary tree has eight directories
and eight 53,371-byte regular files. Both contain a slow symlink. Layered tests
leave the live root-directory block and ordinary payload inherited, poison the
parent underneath upper DATA/ZERO, and verify complete merged bytes and ownership
with an independent upstream LSMT reader before mounting. These small controlled
trees are not substitutes for an application workload.

A separate 256 MiB image contains the installed CPython interpreter, standard
library, extension modules and dynamic libraries: 743 files in 54 directories,
44,132,307 content bytes. Its logical/tail/repeat conversions pass full-view
upstream readback, filesystem checks, deterministic output and native ZFile
byte verification. Its 36-trial startup matrix also completed: two layouts,
three encodings, prefetch off/on and three repetitions, each in a fresh guest.
All 743 file hashes and the exact directory set pass after the timed starts.

Every timed trial starts a fresh no-NIC Linux 6.18.18 guest with two vCPUs,
3 GiB RAM, two Elio workers and two ublk queues. A guest-loopback origin applies
2 ms response delay and a shared nominal 16 MiB/s response-body cap. Background
fill is disabled. Structural prefetch uses 256 KiB head and tail windows.
ZFile uses 4 KiB blocks; LayerStore uses 64 KiB extents. Conditions are identical
within each comparison. The origin blob is hashed before timing, so origin files
are warm; host caches are neither flushed nor controlled. The shared Intel N305
host is not an isolated performance laboratory.

The probe calls production `open_image`, including inline structural warm-up,
creates the real ublk device and mounts its filesystem. Controlled-tree commands
enumerate directories and hash files. The application command starts a fresh
CPython process inside the mounted root, imports JSON/SSL/SQLite modules and
checks a deterministic result. A second process is the warm control; full
application file/directory validation follows both timed starts. All timestamps
use CLOCK_MONOTONIC. HTTP counts are assigned by request start time; root read
counters measure post-assembly device demand.

## Controlled raw-image results

Values are medians of three trials. Start-to-command includes image assembly,
device readiness, mount and the first complete command. It does not hide inline
warm-up in an untimed preparation step. Warm repeat includes guest filesystem
caching. See [the numeric results](metadata-layout-results.json) for all codecs,
min/max, command and assembly times, CPU/RSS, HTTP bytes and read amplification.

| View | Tree | Layout | Prefetch off: start ms | Prefetch on: start ms | Off: remote MiB | On: remote MiB |
|---|---|---|---:|---:|---:|---:|
| single | metadata | logical | 147.96 | 202.16 | 0.83 | 1.20 |
| single | metadata | tail | 149.11 | 218.00 | 0.83 | 1.08 |
| single | ordinary | logical | 189.81 | 234.59 | 0.70 | 1.14 |
| single | ordinary | tail | 180.44 | 224.45 | 0.77 | 1.02 |
| layered | metadata | logical | 176.94 | 316.17 | 0.95 | 1.76 |
| layered | metadata | tail | 178.48 | 315.41 | 0.95 | 1.70 |
| layered | ordinary | logical | 212.31 | 263.03 | 0.86 | 1.30 |
| layered | ordinary | tail | 214.56 | 262.81 | 0.86 | 1.24 |

Prefetch reduces later raw-image command work in several cells but increases
overall start-to-command time in these controls. Small compressed blobs already
fit in very few cache extents, so layout effects can be negligible. Three
repetitions describe observations; they do not establish statistical significance.

## Application startup results

The CPython command imports JSON, SSL, SQLite and argument-parsing modules,
checks an input-file digest and a deterministic calculation, then exits. The
warm control starts a second interpreter in the same guest. Full-tree validation
happens afterwards and is excluded from all startup timings and traffic below.

| Codec | Layout | Prefetch off: start ms | Prefetch on: start ms | Off: remote MiB | On: remote MiB |
|---|---|---:|---:|---:|---:|
| raw | logical | 2846.27 | 2855.55 | 18.58 | 18.89 |
| raw | tail | 2811.83 | 2831.55 | 18.21 | 18.40 |
| lz4 | logical | 2040.57 | 2019.74 | 12.42 | 12.30 |
| lz4 | tail | 1954.86 | 1943.67 | 11.92 | 11.92 |
| zstd | logical | 1762.23 | 1812.75 | 10.13 | 10.19 |
| zstd | tail | 1708.18 | 1708.98 | 9.26 | 9.32 |

With prefetch disabled, observed total startup medians change from 2,846 to
2,812 ms for raw, 2,041 to 1,955 ms for LZ4, and 1,762 to 1,708 ms for ZSTD.
These are modest workload-specific observations, not a universal speedup or
a significance claim. Prefetch does not uniformly improve total startup.
Warm-command medians span approximately 127–134 ms; filesystem/page caching is
part of that control. Keep both knobs explicit and compare total startup under
representative input and transport conditions before enabling them.

## Conversion and index costs

The cost series has 18 randomized controlled converter runs plus six application
runs: four fixtures, two layouts and three repetitions. Both variants retain
their raw filesystem for verification.
GNU time measures converter CPU and peak RSS. Output-directory space is sampled
every 5 ms: reported maxima are lower bounds, not exact transient peaks. Exact
retained logical/allocated bytes and raw/LSMT hashes are recorded separately.

| Fixture | Layout | Wall ms | User + system CPU s | Peak RSS KiB | Index entries | Index bytes |
|---|---|---:|---:|---:|---:|---:|
| metadata | logical | 165.35 | 0.03 | 10168 | 2 | 32 |
| metadata | tail | 165.31 | 0.03 | 9308 | 2 | 32 |
| multigroup | logical | 1268.58 | 0.41 | 10116 | 21 | 336 |
| multigroup | tail | 1118.21 | 0.38 | 10160 | 28 | 448 |
| ordinary | logical | 118.98 | 0.04 | 10272 | 2 | 32 |
| ordinary | tail | 165.57 | 0.04 | 10412 | 20 | 320 |
| application | logical | 2872.41 | 0.80 | 10804 | 33 | 528 |
| application | tail | 2871.08 | 0.78 | 10772 | 309 | 4944 |

The metadata and ordinary fixtures are 8 MiB; the multigroup fixture is 160 MiB.
The application filesystem is 256 MiB.
The ordinary fixture grows from 2 to 20 index entries. The application image
grows from 33 to 309 entries, adding 4,416 index bytes; its LZ4 output grows from
27,836,400 to 27,839,442 bytes and ZSTD from 20,647,993 to 20,649,366 bytes.
Identical logical bytes do not imply identical compressed size or conversion cost.

## Stored-window coverage

The application filesystem has 184 classified metadata blocks (753,664 bytes).
Raw tail placement covers 33.58% in a 256 KiB tail window and 100% in 1 MiB.
For its LZ4 wrapper, the exact 256 KiB stored window covers no complete metadata
records because of trailing format/index bytes; expansion to 64 KiB cache extents
covers 78.26%. A 1 MiB window covers all metadata. This is a geometry model, not
a standalone prediction of startup latency: format-open reads, head warm-up,
eviction and budget exhaustion are separate effects. Full native decompression
checks precede the model. Do not apply raw offsets to compressed byte windows.

## Validation and limits

The reordered converter output preserves every logical sector, including free
blocks, and emits only ordinary LSMT mappings. Independent raw classification,
upstream readback, filesystem checks and repeated conversions supplement unit
coverage of hardlinks, external xattrs, slow symlinks, indirect blocks and block
reuse. Unsupported classification falls back to logical packing. Differential
conversion is not added: layered benchmark uppers select mappings from actual
converter outputs and test existing DATA/ZERO/INHERIT composition.

The controlled matrices and controlled conversion costs use the packing implementation
before integration of the independent sparse-export change. The application
probe and application conversion costs use the rebuilt integrated implementation; source revisions and controls
are in the numeric results. Helpers and runtime inputs are hash-pinned within
each matrix. These are local experiments, not CI performance requirements.

CPU snapshots distinguish helper and waited command children; helper RSS comes
from its own process status. Registry and guest/kernel memory are excluded.
Remote/device-requested byte ratios include compression, omit assembly warm-up
from the denominator, and are not internal cache-hit statistics. Concurrent
counter snapshots are individually atomic; kernel readahead can cross phase
boundaries. The bandwidth setting is an upper bound, not achieved throughput.
