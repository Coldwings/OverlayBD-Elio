# Sparse extent copy-up: bounded coverage comparison

This measures the runtime implementation introduced for issue #197 against the
previous coverage implementation. It supports the narrow result that copy-up
reduces coverage work and read fragmentation in this workload, while increasing
first-write lower-read dependence and synchronized reopening cost. It is not a
storage-system ranking or a claim about real remote-registry performance.

## Inputs and method

- Coverage baseline: `18ad26b6440ab94beefbd31b8e3fc62705838f78`.
- Extent implementation: `9232be531bd01812f8af2b8108c15476d83e0eaa`.
  Later changes contain only documentation, API comments and this report.
- Identical `benchmarks/sparse_copyup.cpp` harness on both implementations;
  historical baseline changes only add that harness/target.
- GCC RelWithDebInfo, warnings as errors, tests/test hooks **off** for timing.
  Binary/core/harness hashes and every trial are in
  [sparse-copyup-results.json](./sparse-copyup-results.json).
- Host btrfs, 4 KiB allocation blocks/pages, warm ordinary files, CPU 0 affinity,
  one worker, QD1. Shared host; no global cache drop or hardware isolation claim.
- 7 MiB virtual disk with a nonzero immutable lower. Each write phase visits
  256 blocks; random small writes select 256 distinct blocks from a 1,024-block
  region. The read phase performs 1,024 reads. Seed 197.
- Three fresh-directory trials per implementation per delay, alternating
  variant order. The delay is either zero or a constant 1,000 microseconds per
  lower payload-source call, without bandwidth, HTTP, compression or retries.
- Every phase verifies the complete byte model outside its timers. This warms
  later reads. Timers include small model bookkeeping; paired writes are timed
  together. CPU/resource sampling includes sampling overhead.

## Latency

Values are the median of three per-trial sequence p50s, in microseconds. The
JSON retains each trial's p50/p99, wall time, throughput and CPU counters.

| Operation | Coverage warm | Extents warm | Coverage 1 ms lower model | Extents 1 ms lower model |
|---|---:|---:|---:|---:|
| First 512 B write | 46.89 | 15.15 | 46.87 | 1097.67 |
| Second sector in same block | 47.05 | 9.19 | 47.47 | 16.54 |
| Overwrite same sector | 15.21 | 9.16 | 16.74 | 10.02 |
| Random distinct-block 512 B write | 57.09 | 18.81 | 57.33 | 1104.84 |
| Aligned 4 KiB write | 61.48 | 19.43 | 62.46 | 34.65 |
| Two 512 B writes per block | 141.41 | 33.38 | 143.09 | 1150.34 |
| Warm 4 KiB read after paired writes | 5.75 | 2.17 | 2092.55 | 4.22 |

The first warm small write improves from about 47 to 15 microseconds, but the
same first write in the delay model regresses to about 1.10 milliseconds because
it must fetch the complete lower block. Random first writes and the first of a
paired write show the same dependency. Same-sector overwrites require no new
copy-up. In the warm read phase, coverage submits 2,048 upper and 2,048 lower
reads; extents submits 1,024 upper reads and no lower reads. The final merged
index has 2,049 versus 381 fragments, respectively; random untouched gaps
remain represented in the extent result.

For this script's selected phase mix, median process CPU totals are 109.914 to
34.297 ms without delay, and 188.858 to 114.788 ms with the delay model. These
include bookkeeping/sampling and are not a general CPU-cost guarantee.

## Submitted request accounting

Separate test-enabled, zero-delay runs count requests accepted by a delegating
backend; their latency/CPU values are excluded above. This is logical submitted
traffic, including retries, not completed byte counts, physical disk traffic,
filesystem metadata, or setup/recovery synchronous I/O. Both accounting runs
also pass the complete byte oracle. Every cell is **coverage / extents**, in
bytes across that phase.

| Operation | Upper payload write bytes requested | Coverage metadata write bytes requested | Lower payload read bytes requested |
|---|---:|---:|---:|
| First 512 B write | 131,072 / 1,048,576 | 1,067,008 / 0 | 0 / 1,048,576 |
| Second sector in same block | 131,072 / 131,072 | 1,067,008 / 0 | 0 / 0 |
| Overwrite same sector | 131,072 / 131,072 | 0 / 0 | 0 / 0 |
| Random distinct-block 512 B write | 131,072 / 1,048,576 | 1,067,008 / 0 | 0 / 1,048,576 |
| Aligned 4 KiB write | 1,048,576 / 1,048,576 | 1,067,008 / 0 | 0 / 0 |
| Two 512 B writes per block | 262,144 / 1,179,648 | 2,134,016 / 0 | 0 / 1,048,576 |
| Warm 4 KiB read after paired writes | 0 / 0 | 0 / 0 | 3,145,728 / 0 |

Coverage ownership publication submits 4,168 bytes per changed metadata page
(8-byte invalidation, 4,152-byte body, 8-byte commit); the measured counts match
that implementation. A same-sector overwrite does not publish coverage. The
extent path submits no ownership metadata. Its initial partial write submits
4 KiB of upper payload instead of 512 B, and reads 4 KiB from the lower.
`/proc/self/io` syscall counts are retained as observations, but they do not
count io_uring payload requests and are not used as this traffic evidence.

## Reopen and footprint

This is **same-process close/reopen without preceding guest FLUSH**, with the
runtime and host page cache alive. It is not fresh-process startup or a
power-loss test. Extent recovery requests FIEMAP_FLAG_SYNC, so its time includes
pending writeback. The subsequent reported `flush_after_reopen_ms` follows that
writeback and must not be compared as an equivalent dirty-FLUSH workload.

| Variant | Lower delay (us) | Median reopen (ms) | Trial range (ms) | Final allocated upper bytes |
|---|---:|---:|---:|---:|
| coverage | 0 | 0.339 | 0.270–0.376 | 4,210,688 |
| extents | 0 | 49.018 | 48.464–69.021 | 4,194,304 |
| coverage | 1000 | 0.536 | 0.400–1.030 | 4,210,688 |
| extents | 1000 | 56.051 | 53.941–62.264 | 4,194,304 |

Both variants ultimately allocate 4 MiB for upper payload in this workload;
coverage additionally allocates 16 KiB for its sidecar. File allocation is not
physical write amplification and excludes filesystem metadata overhead.
Per-phase `st_blocks` can reflect delayed allocation; the table uses final
post-FLUSH measurements. The reopening regression is retained explicitly.

## Reproduction and validation limits

Configure each revision out of source with `OBD_BUILD_BENCHMARKS=ON`,
`OBD_BUILD_TESTS=OFF`, `OBD_WARNINGS_AS_ERRORS=ON` and
`CMAKE_BUILD_TYPE=RelWithDebInfo`. Add the identical harness/target to the
historical baseline. For each variant, use a new absolute data directory and
report path:

```sh
obd-bench-sparse-copyup NEW_DIRECTORY REPORT 256 0 197
obd-bench-sparse-copyup NEW_DIRECTORY REPORT 256 1000 197
```

Repeat three times, alternating variant order, and pin to one CPU. For traffic
only, rebuild with `OBD_BUILD_TESTS=ON` and append `--traffic` to the zero-delay
command; that adapter is restricted to regular files and one worker.

Functional ext4/XFS validation is separate from these host-btrfs timings. It
covers ordinary buffered operations, synchronized reconstruction, real ENOSPC,
ZERO transitions, process exits, timed SIGKILL, full-neighbor oracles and
allocation-hint admission. Kernel ublk validation covers one/two queues and
512-byte guest requests. None of these measurements claim general scalability,
real remote performance, every instruction-level interruption point, or host
power-loss safety. See [testing](../docs/testing.md) and
[ADR-0037](../docs/adr/0037-reconstruct-sparse-ownership-from-extents.md).
