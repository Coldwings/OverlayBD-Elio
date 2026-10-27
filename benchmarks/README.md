# Optional ublk read controls

These developer tools compare two existing builds on one Linux host. They are
not CTest tests or a performance CI gate. Building them is optional and does not
change Device scheduling or select a locality policy.

Configure each revision in its own source/build directory, using equivalent
flags and dependency versions. For example, in each checkout:

```sh
cmake -S . -B build-bench -DOBD_BUILD_BENCHMARKS=ON \
  -DOBD_ENABLE_UBLK=ON -DOBD_BUILD_TESTS=OFF \
  -DOBD_ENABLE_LIBE2FS_BACKEND=OFF -DOBD_WARNINGS_AS_ERRORS=ON
cmake --build build-bench --target obd-bench-ublk-read obd-mkimage --parallel 1
```

`OBD_BUILD_BENCHMARKS` defaults to OFF. It requires the ublk backend. The target
is not installed, and the runner is a Python 3.8+ source script; fio is a separately
installed runtime prerequisite. The runner supports Linux CMake Unix Makefiles and single-configuration Ninja
builds. It reads Makefiles link.txt or queries CMAKE_MAKE_PROGRAM with the
read-only Ninja -t commands tool and resolves libraries from that generator's
link working directory. Other generators are rejected before device admission.

Coordinate a compiler-free measurement window, keep other owned experiments
idle, and ensure the host has a usable ublk driver and permission to create a
device. The runner never loads modules or adjusts system settings. Run with
your already granted device permissions, or explicitly request its noninteractive
sudo launcher:

```sh
python3 benchmarks/run_ublk_read.py \
  --baseline-build /path/to/baseline/build-bench \
  --candidate-build /path/to/candidate/build-bench \
  --out /path/to/new-results --workers 8 --rebuild-before-run --sudo
```

The paths and worker count are operator choices. The output directory must be
outside both source checkouts, so saving results cannot change their status.
Duplicate QDs and nonfinite observation deadlines are rejected. Omit `--sudo`
when unnecessary.
The runner itself and fixture writer remain unprivileged with that flag; only
the owned benchmark driver is launched through `sudo -n`. Both revisions need
the same tool implementation. `--rebuild-before-run` optionally invokes the
public CMake incremental build for the benchmark and `obd-mkimage`, serially
before fixture creation or any measurement. Its commands/logs are retained;
source HEAD/status/tracked hashes, CMake cache/commands and observed native Elio
headers must stay fixed across that build. Post-build executables and linked
archives are frozen and rechecked before each trial and after the matrix.
The raw fixture, generated LSMT artifact and selected fio executable hashes
are also frozen and checked before and after every completed trial. A changed
input stops the matrix; neither variant may silently receive different data.
Changing a configuration during rebuild fails before device admission; rerun
with the intended configuration already established. No build runs during the
matrix. This is not a clean rebuild or generic cryptographic build proof.
Prefer clean committed sources; `--allow-dirty`
retains tracked differences explicitly and must not be described as a clean
committed-head result. Record any untracked source/dependency overrides separately.

The runner writes deterministic dense raw bytes and invokes the baseline
build's real `obd-mkimage` without ZFile options. Both variants read the **same
plain sealed LSMT artifact**. The writer may generate a fresh UUID, so compare
retained artifact hashes rather than assuming reproducible whole-file hashes.
The source is either a minimal zero-filling BlobSource or one local LSMT layer
through the production merged reader. Backing data is prewarmed by reading the
artifact before the matrix.
Residency is not measured or guaranteed. To claim fully cached input, retain
independent residency evidence before each trial and account for pressure
between checks. Otherwise describe this as a prewarmed control. No alternate format writer or
fake Device lifetime model is
included. Gzip/TurboOCI is outside this initial tool; use separately identified
actual-reader controls before deciding policy for CPU-heavy sources.

Defaults are64MiB, one queue/depth128, fio4KiB random direct io_uring reads,
QD1/QD16, three interleaved repetitions,1s separate warm-up and5s measured run.
`--workers` is required. Worker count, blocking threads, queue geometry, QDs,
size and durations can be set explicitly; inspect `--help`. CPU binding,
frequency, idle state, background workload and temperature are unmanaged.
Active compilers are rejected before a trial and observed during it; a tainted
trial finishes its own drain and then stops the matrix. Detection is diagnostic,
not proof that an otherwise shared machine is idle. This tool never terminates
another process or device.

The privileged CI topology observation runs this same committed build under
both variant labels, once with one queue/worker and once with two pairs. It
records QD1/QD16 for zero and local-LSMT sources with one repetition, one-second
warm-up and three-second measurements. This is a diagnostic topology comparison,
with no cross-revision or performance threshold claim. Server JSON records the
actual Device queue count and scheduler worker count. Startup logs identify
each queue's owner worker and Linux TID; per-thread CPU snapshots allow those
owners to be correlated with measured process activity. CPU placement, clock
frequency and competing hosted load remain unmanaged, so these short results
do not establish multicore scaling or predict production performance.

For each trial the driver creates its own Device, starts fio with `posix_spawn`
and waits off-worker. The Device owner lives in synchronous `main`, outside
`elio::run`; the
coroutine holds a borrowed pointer. It awaits native `Device::stop_async()`,
then `main` resets the owner only after `elio::run` returns, off every Elio
worker. An exception from stop-task creation, async drain or blocking-pool
admission therefore cannot destroy the live Device during worker-frame unwind.
The runtime explicitly uses its public unbounded graceful shutdown contract.
Long-lived queue-owner tasks do not stop merely because shutdown begins: an
allocation failure before stop starts can leave `elio::run` waiting indefinitely
with the main-owned Device retained. Even exceptional runtime return can leave
main-thread stop/destruction waiting. This is a documented liveness limit, not
a bounded cleanup guarantee; no finite force-stop or private retry machinery
is substituted. Normal body failures request the same public async drain. It
never copies internal Queue, owner-pump, cancellation or recovery logic.
Recovery is disabled for this control.

The result directory retains config files, driver logs, actual-owner records,
full warm/measured fio JSON, server rusage/proc/TID snapshots, actual fio argv,
observed checkout HEAD/status/tracked hashes, actual relevant compile-command
entries, CMake configuration/link inputs, observed native Elio header hashes
and package/Git identity (including source overrides), executable/library hashes,
writer/fio identity, CPU model/affinity metadata, fixture hashes and raw input.
These observations identify what was seen at run time; without the optional
public rebuild they do not verify that an existing binary was built from that
checkout. Even with it, an incremental build delegates dependency decisions to
CMake and does not cryptographically prove every object input. Dependency or
header changes not covered by the recorded inputs remain a provenance limit.
The summary keeps CPU and latency separate:

- fio IOPS and individual mean clat/lat are separate measured fields. Full
  individual clat percentiles and achieved queue-depth distributions remain
  in every result; reciprocal IOPS is never labelled request latency.
- Server CPU is RUSAGE_SELF user+system divided by that measured invocation's
  completed IO count. It excludes fio child CPU and includes fixed snapshot,
  launch and wait overhead. Independent process ticks and context switches
  use the same count. This is not wall time.
- Inspect raw repetitions and achieved depth before drawing conclusions;
  the compact median summary is not a significance test.

A timeout is an **observation deadline**, not permission to terminate the owner.
The runner records the launcher PID, stops admitting trials and returns while
leaving the server alive in its own session. The driver's `.owner.json` records
its actual PID/start-stat and device identity. On timeout or interruption,
retain the directory and inspect/coordinate that still-live process before
continuing experiments. Do not turn a historical numeric PID/device ID into
signal/delete authority. No broad process cleanup, global device deletion,
process-group kill, or automatic retry is performed. Do not remove a retained
result directory while any owner is live.


The direct driver accepts `--config <json>` and validates an object, source
mode, absolute paths, positive integer bounds/alignment and existing trial
outputs before Device creation. Unknown JSON fields are ignored. It delegates
runtime worker/queue limits, available memory, kernel support and fio executable
compatibility to their respective public implementations; parsing success is
not a guarantee that the requested resources are feasible. Failed trials are
retained and are not included in performance summaries.

For an intentional public-drain control, a direct config may set
`"fail_after_warmup": true`. The driver runs the real warm-up, reports the
requested body failure, awaits the same public async drain, and records
`drain_complete` and `cleanup_complete` before exiting nonzero. This is a
lifecycle diagnostic, not a performance sample or a fake Device model.
`benchmark-runner-controls` exercises portable argument, output, observed-input,
generator and ordinary-child timeout handling without creating a Device. The
default build-test CI job compiles the optional driver and runs these controls;
CI does not run this privileged performance matrix.

## Writable upper comparison (issue #146)

`obd-bench-writable SETTINGS.json REPORT.json` uses owned ordinary files through
`MergedWritable`, the same merged write interface used by the device. It creates
a **new** absolute `directory` and exclusively creates its report; it does not
remove artifacts or overwrite existing directories/reports. Example settings:

```json
{
  "mode": "hybrid",
  "directory": "/absolute/new/trial-directory",
  "size_bytes": 1073741824,
  "populate_bytes": 1073741824,
  "samples": 4096,
  "seed": 146,
  "qd": 16,
  "workers": 2,
  "cycles": 2
}
```

Repeat for `lsmt`, `sparse`, and `hybrid`, and QD1/QD16, with a fresh directory
and report each time. QD means concurrent awaiting application operations, not
kernel queue depth; merged-layer serialization remains part of the measurement.
The tool records observed awaiting concurrency. Random first writes use unique
4 KiB offsets, then rewrite those exact offsets. Each discard/rewrite cycle
measures a discard plus a write as one operation. The same seeded offsets and
payload are used for every mode. All sampled reads are checked after each phase.
Flush time is reported separately from request latency and throughput. Durability
contracts differ: LSMT FLUSH does not make its volatile mapping index recoverable.

Full population follows the random phases, using 1 MiB requests, and is timed
separately including its final FLUSH. Set `populate_bytes == size_bytes` for a
fully populated trial; a larger virtual capacity alone is not a full-size write
or seal benchmark. After writable reopen (sparse/Hybrid), every populated byte
is checked. For all three modes, the sealed output is then reopened through the
ordinary LSMT reader and checked across every populated byte plus sampled writes
outside that region. This additional readback is outside seal timing and reported
separately; process CPU/RSS include verification. Uncheckpointed LSMT has no equivalent resumable writable reopen, so
that field is null, with an explanation. Sparse uses the separate export API
(ADR-0027): its `seal_ms` includes owner release, source reopen, output publication
and the API's digest. Its source remains reusable and contributes to retained
and peak space. LSMT/Hybrid instead time their live seal APIs without an external
digest pass. `seal_note` records that distinction; these are not identical APIs
or equal durability contracts. Every mode verifies the full populated prefix
and sampled tail of its sealed output.

The report includes per-file logical/allocated sizes after each random phase,
reopen time, live-seal time, and peak allocated space sampled every 10 ms during
seal. The sampled peak is a lower bound: short-lived temporary files between
samples may be missed. Publication includes the format's required synchronization;
LSMT/Hybrid do not add an external digest pass, while Sparse includes its API
digest. OS caches are warm; this tool does not
drop global caches. RSS includes the driver, seeded permutation and readback
buffers. Keep raw JSON, exact build/source provenance, CPU/filesystem details,
and repetition counts alongside any performance summary. Small smoke runs
validate the harness only and are not acceptance-size benchmark results.

The completed development matrix used fully populated 1/10/32 GiB uppers,
QD1/QD16, two workers, 4096 sampled offsets, four discard/rewrite cycles,
and three seeds per cell (54 trials). All trials passed, including complete
sealed-output readback for LSMT and Hybrid. On the shared, warm-cache host,
Hybrid cell-median first-write throughput was 51–162 operations/s, versus
20.1–25.4 thousand operations/s for already-owned rewrites. Hybrid is therefore
not a general first-write throughput improvement. Metadata-only seal medians
were 78–102 ms; the copying LSMT seal grew from approximately 6.4 seconds at
1 GiB to 396–413 seconds at 32 GiB. These are observations under different
durability contracts, not portable performance guarantees. Sparse export was
not implemented in this frozen comparison. QD16 does not establish multicore
scaling, and these ordinary-file measurements do not validate kernel or
supervisor crash recovery. This frozen matrix predates the final trailer-based seal classification; its
seal timings are historical evidence, not measurements of the final implementation.

## Small Hybrid process-crash comparison

The [numeric results](hybrid-writable-results.json) record the basic development
comparison: 32 MiB fully populated before seal, 256 sampled 4 KiB offsets per
random phase, one discard/rewrite cycle, seed 146, two workers, QD1/QD16,
and one trial per cell. All eight trials passed full sealed-output readback.
These measurements replace further large benchmark runs for this change;
the later 1/10/32 GiB rerun was stopped at the user's request and is incomplete.
The older completed matrix above remains historical evidence only.

| Build / format | QD | First write ops/s | Rewrite ops/s | Discard + rewrite ops/s | First FLUSH ms | Seal ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| baseline-hybrid-qd1 | 1 | 409 | 30574 | 409 | 6.0 | 40.0 |
| optimized-hybrid-qd1 | 1 | 7136 | 29201 | 4446 | 14.5 | 27.4 |
| baseline-hybrid-qd16 | 16 | 368 | 27831 | 386 | 6.4 | 29.0 |
| optimized-hybrid-qd16 | 16 | 7953 | 29129 | 4465 | 31.1 | 27.5 |
| optimized-lsmt-qd1 | 1 | 29016 | 28900 | 22590 | 15.9 | 246.4 |
| optimized-lsmt-qd16 | 16 | 29006 | 28731 | 23568 | 7.2 | 241.6 |
| optimized-sparse-qd1 | 1 | 8396 | 37233 | 51 | 45.3 | 225.8 |
| optimized-sparse-qd16 | 16 | 9540 | 36266 | 81 | 34.0 | 248.8 |

Baseline Hybrid synchronizes payload on every ownership transition. Optimized
Hybrid completes payload, mapping, and recovery-progress writes before returning,
without that per-transition sync; explicit FLUSH retains persistence ordering.
This comparison measures that selected process-crash contract, not a speedup
under identical power-loss guarantees. Request timings exclude the separately
reported FLUSH. Remaining first-write cost includes index/progress updates;
these measurements do not attribute the remaining gap to a specific operation.

The shared host and warm caches, small sample, and single trials do not support
a statistical format ranking or multicore scaling claim. Sparse export includes
its API digest; LSMT and Hybrid live seal do not. Identical benchmark payloads
cannot independently detect skipped rewrites; separate semantic regression tests
cover those transitions. The local LSMT writable-reopen gap is tracked in
[issue 164](https://github.com/Coldwings/OverlayBD-Elio/issues/164); it is not an
upstream LSMT format restriction. Source archive, dirty-patch, binary, and original
report hashes are recorded in the JSON for this development snapshot. It predates
the later removal of terminal-seal xattr publication and its extra sync, so seal
timings are not measurements of that final seal path.

## Hybrid log scaling and compaction cadence

`obd-bench-hybrid-log NEW_ABSOLUTE_DIRECTORY` creates controlled, valid synthetic
upstream-layout logs with 1, 4,096, 65,536, 1,048,576 and the supported maximum
record count. Every record describes the same live 512-byte DATA extent. This
isolates history length from live-map count; it is not a claim that the native
writer ordinarily retains that history despite automatic compaction. The tool
measures unmanaged read-only replay, initial writable adoption, then three
managed writable reopens, validating the mapping and bytes. Caches are warm.

A separate native Writer workload initializes 1 or 4,096 live mappings separated by logical gaps (so compaction
cannot merge them), verifies the live count stays fixed, and
performs 8,192 alternating discard/rewrite operations on an owned sector.
It records actual index-generation changes with before/after record counts,
live-map count and the expected `max(4096, 2*live+16)` trigger, and verifies
constant physical payload size and final bytes. This checks compaction cadence
directly; its heavily observed wall time is not a throughput benchmark.
Fixture construction happens outside replay timing. Output files are retained,
and existing directories are refused. Build and run serially with the other
benchmarks and preserve source/binary hashes with the resulting report.

## Native converter metadata placement

See [metadata layout measurements](metadata-layout.md) for the controlled
conversion/startup study and its [numeric results](metadata-layout-results.json).
These use production image assembly and inline structural warm-up, unlike the
local-source fio controls above. They do not change the default layout.
