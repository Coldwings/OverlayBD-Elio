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
