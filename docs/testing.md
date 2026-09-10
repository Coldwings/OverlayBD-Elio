# Test Strategy

How overlaybd-elio is tested: layout, fixtures, naming, golden-value
policy, how to run, and the full inventory of tests with the property
each guards.

## Layout

| Location | Contents | Built into |
|---|---|---|
| `tests/unit/` | Catch2 unit tests, one file per module area | `obd_unit_tests` |
| `tests/integration/` | End-to-end tests over a mock registry; the ublk E2E | `obd_integration_tests` |
| `tests/support.hpp` | Shared fixtures (below) | header-only |
| `tests/fixtures/` | (reserved for on-disk fixtures) | — |

The ublk tests (`tests/unit/test_ublk.cpp`,
`tests/integration/test_ublk_e2e.cpp`) compile only when
`OBD_ENABLE_UBLK` is on; everything else builds everywhere.

### Fixtures (`tests/support.hpp`)

- **`run_coro`** — runs a coroutine to completion on a fresh Elio
  scheduler and returns its value; every async test body goes through it.
- **`VectorSource`** — an in-memory `BlobSource` with a read counter and
  failure injection (`fail_with(errno)`); the workhorse stub for the
  whole source/format stack.
- **`TempDir`** — RAII temporary directory (pid + counter namespaced),
  removed on destruction.
- **`pattern_bytes(n, seed)`** — deterministic pseudo-random content
  (LCG), so fixtures are reproducible across runs and machines.
- **`write_file`** — durable little helper for fixture files.
- **`make_tar_header(payload_size, typeflag)`** — builds a minimal valid
  512-byte ustar header (magic/version/checksum correct) for tar-adapter
  tests.

A **mock registry server with HTTP Range support** (`MockRegistry` in
`tests/unit/test_registry.cpp`) serves plain and redirecting blob
endpoints plus a bearer-token endpoint; the token endpoint counts
exchanges and can issue `expires_in`, per-exchange serial tokens, and
selective rejections, so single-flight and cache-lifetime behavior is
observable. The integration tests reuse the
same style of in-process server, so no test touches the network; the
multi-blob variant (`BlobMapServer` in
`tests/integration/test_integration.cpp`) adds per-blob GET/extent
counters, per-request latency injection, and an optional serialized
service mode (source capacity 1) for the ADR-0012 admission-funnel
tests.

## Naming convention

Every `TEST_CASE` name is `"<area>: <behavior>"`, with area in the fixed
set `{common, format, source, image, ublk, supervisor, cli,
integration}`. This is **enforced**: `scripts/check-docs.sh` (check 2,
hard gate) extracts backticked tokens matching the convention from
`docs/` and fails CI if any is not the exact name of a real `TEST_CASE`
in `tests/`. Cite tests in docs verbatim, in backticks, and keep the
area prefixes spelled exactly.

## Test writing rules

Async test bodies are coroutines driven to completion by `test::run_coro`.
Never pass a `co_await` expression directly to a Catch2 assertion macro:

```cpp
// WRONG — REQUIRE/CHECK decompose and can evaluate their argument more
// than once; a co_await inside one is not a single evaluation. (Kept
// commented out below so the anti-pattern never appears as live code:
// a repo-wide grep for it stays clean, and no test copies it by accident.)
//   REQUIRE(co_await src->pread(buf.data(), buf.size(), 0) == 1024);

// RIGHT — await once into a named local, then assert on the value.
const ssize_t got = co_await src->pread(buf.data(), buf.size(), 0);
REQUIRE(got == 1024);
```

Awaiting the same coroutine a second time corrupts coroutine semantics
(re-suspension on an already-driven awaitable), so `REQUIRE`,
`CHECK`, `REQUIRE_FALSE` and `CHECK_FALSE` must never wrap `co_await`
directly — in any form of the pattern, including negations
(`REQUIRE(!co_await ...)`) and multi-line arguments. Assign the awaited
result to a named local first, then assert on the local.

## Golden values and cross-validation

Format-level behavior is pinned against **upstream OverlayBD format
semantics, cross-validated independently** — golden header bytes and
encoding expectations are taken from the OverlayBD specification
documents, and round-trip fixtures are generated with the independent
`obd-mkimage` writer path. A golden value must **never** be sourced
from this project's own writers alone: a writer/reader pair that shares
a bug would round-trip cleanly and prove nothing. When adding a
format test, state in the test comment where the golden value comes
from.

## How to run

```bash
cmake -S . -B build && cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
bash scripts/check-docs.sh   # documentation governance floor
```

Tests are registered with CTest via `catch_discover_tests`, so each
`TEST_CASE` is its own CTest entry (`ctest -R '<name>'`). For Catch2
filters directly:

```bash
./build/tests/obd_unit_tests "format: *"        # one area
./build/tests/obd_unit_tests "[integration]"    # by tag
./build/tests/obd_integration_tests             # e2e suite
```

**Privileged ublk E2E.** The tests tagged `[ublk]` (currently
`integration: ublk device serves sector reads from a blob`,
`integration: ublk writable device serves writes and discard`,
`integration: ublk device survives server death via USER_RECOVERY`)
need `/dev/ublk-control` and root (or `CAP_SYS_ADMIN` in a suitable
user namespace) with `ublk_drv` loaded. They **self-skip** via Catch2
`SKIP()` when the kernel facility is absent, and
`obd_integration_tests` is registered with `SKIP_RETURN_CODE 4` so an
all-skipped run is not a failure. **Never make the default test run
depend on privileged kernel state** — new privileged tests must
self-skip the same way. The privileged CI job
(`build-test-ublk-privileged` in `.github/workflows/ci.yml`) loads
`ublk_drv` and runs exactly these:
`sudo -E ./build/tests/obd_integration_tests "[ublk]"`.

## Test inventory

Every test, grouped by area, with the property it guards.

### common

- `common: crc32c matches the OverlayBD raw running CRC` — the CRC32C
  variant matches the exact running-CRC semantics OverlayBD segment
  digests use (raw, non-inverted), cross-validated against upstream.
- `common: little-endian load/store round-trips` — LE encode/decode
  helpers are inverse across boundary values (0, unaligned sizes, max).
- `common: segment_mapping encodes OverlayBD bit layout` — the LSMT
  segment mapping packs/unpacks offset/length/flag bits exactly as the
  upstream wire layout requires.
- `common: base64 round-trips and decodes cred.json form` — base64
  encode/decode is inverse and decodes the Docker-style `auth` field
  used in credential files.

### format

- `format: zfile header bytes match the OverlayBD wire format` — golden
  bytes: the emitted ZFile header equals the upstream-specified layout
  byte for byte.
- `format: zfile round-trip reads back the original content` — a
  compressed fixture decompresses to the exact original bytes at
  arbitrary offsets.
- `format: lsmt header bytes match the OverlayBD wire format` — golden
  bytes for the LSMT header and index encoding.
- `format: lsmt round-trip and multi-layer merge semantics` — written
  layers read back exactly, and merging N layers resolves each virtual
  offset to the topmost covering layer.
- `format: merge falls through holes to lower layers` — a range not
  covered by an upper layer reads from the layer below, down to the
  base.
- `format: zfile reader rejects a corrupted block digest` — a bit-flipped
  compressed block fails verification instead of returning bad data.
- `format: sparse layer writes, reads and recovers extents` — the
  `SparseRwLayer` persists writes, reads them back, and rebuilds its
  extent map from the fiemap across a reopen (ADR-0008).
- `format: lsmt rw overwrites covered data in place` — rewriting a
  range already covered by the RW layer edits it in place; uncovering
  writes append.
- `format: lsmt rw seal compacts into a standard sealed layer` —
  `seal()` turns the unsealed RW file into a byte-standard sealed LSMT
  RO layer that the normal reader opens.
- `format: lsmt rw seal is deterministic for identical content` —
  identical upper content seals to byte-identical files with equal
  content-derived uuids (ADR-0014 seal determinism); a different write
  sequence yields a different digest, as does identical data with a
  different packed index (zeroed segments — pinning that the index, not
  only the data, feeds the digest).
- `format: lsmt rw checkpoint persists the index for offline seal` —
  `checkpoint()` persists the RW index as an unsealed trailer (terminal:
  later writes get `-EROFS`), `seal_file()` seals it offline with
  sha256/size reported, missing/sealed/uncheckpointed files map to
  `-ENOENT`/`-EALREADY`/`-EINVAL`, and a checkpointed index entry with an
  out-of-range moffset is rejected (`-EINVAL`) rather than sealed
  (ADR-0014).
- `format: lsmt rw offline seal rejects a torn checkpoint trailer` — a
  trailer torn mid-write (magic/flags/virtual_size present, uuid and
  index fields still zero) parses as a valid but empty checkpoint; the
  header/trailer uuid cross-check rejects it with `-EINVAL` and the file
  is left unsealed. Also: an out-of-range `index_offset` with
  `index_size == 0` (uuid intact, cross-check would pass) is rejected by
  the index bounds guard instead of underflowing into an empty seal
  (ADR-0014).
- `format: merged writable falls through and copy-on-writes` —
  `MergedWritable` reads fall through the upper to sealed lowers, and
  writes shadow lowers copy-on-write without mutating them (ADR-0008).
- `format: lsmt rw discard masks coverage with zeroed segments` —
  discard inserts zeroed segments that read back as zeroes and survive
  `seal()` into a valid standard LSMT file (ADR-0009).
- `format: sparse layer discard punches holes and recovers` — discard
  is a real punch-hole; reads stay correct across a reopen even though
  fiemap recovery is filesystem-block granular (ADR-0009).
- `format: merged writable discard masks the lower layer` — discarding
  a range covered only by the lower reads back zeroes, not the lower's
  data (mask semantics, ADR-0009).
- `format: trace crc32c golden vectors match the spec` — the trace blob's
  raw-chaining CRC-32C matches the trace-format.md §4 golden vectors,
  including the chaining property (ADR-0013).
- `format: trace decodes the spec worked example byte-for-byte` — the
  golden 72-byte blob from the trace-format.md §13 appendix decodes to
  the documented header fields and records, and the conforming writer
  reproduces it byte-for-byte.
- `format: trace writer round-trips through the parser` — written records
  parse back to an identical list; the empty trace is a valid blob.
- `format: trace parser rejects corrupt headers and checksums` — the four
  precise failure kinds (Truncated, BadMagic, BadSize, ChecksumMismatch)
  fire per the trace-format.md §7 acceptance order.
- `format: trace parser accepts and ignores a non-multiple tail` — a
  non-multiple `data_size` tail is accepted, ignored, and unchecksummed
  (trace-format.md §7 rule 4).
- `format: trace parser exposes unknown op bytes to the caller` — no op
  validation at parse time; nonzero padding is checksummed, not
  interpreted (trace-format.md §7/§8).
- `format: trace writer enforces the conforming-writer contract` — count 0
  and > 1 MiB, op 'W', and negative offsets are rejected
  (trace-format.md §10 rule 4).

### source

- `source: tar adapter detects ustar wrapper and skips the header` — a
  tar-wrapped blob is exposed at its payload offset, header invisible.
- `source: tar adapter passes plain files through unwrapped` — a
  non-tar blob is served byte-identically, offset zero.
- `source: credential store longest-prefix matching` — the
  `CredentialStore` picks the longest matching key, with and without
  URL scheme.
- `source: DART address parsing and prefixed URL` — `host:port[/prefix]`
  parsing, scheme stripping, `/dart` default prefix, and the
  prefix-passthrough URL shape (ADR-0005).
- `source: registry range reads and size probe` — HTTP `Range` reads
  return exactly the requested window against the mock registry.
- `source: registry bearer auth flow via token endpoint` — a 401 with
  `WWW-Authenticate` drives a token fetch and an authorized retry.
- `source: registry redirect mode drops auth on the CDN URL` —
  redirected blob GETs to the CDN URL do not carry the registry
  credentials.
- `source: DART prefix passthrough preserves the embedded URL` — the
  full upstream URL survives verbatim inside the DART path, including
  query and encoding edge cases.
- `source: layer store cold read persists and reopen serves locally` —
  remote-fetched extents persist to the staging pair and are served
  locally (zero remote reads) after a destroy + reopen (ADR-0011).
- `source: layer store detects corrupted staging via crc and refetches` —
  on-disk staging corruption is caught by the per-extent CRC32, demoted,
  re-fetched correctly, and counted.
- `source: layer store deletes stale sidecar and restarts fresh` — wrong
  nonce names and bad sidecar magic both invalidate the pair: stale files
  are deleted and the layer restarts with a fresh nonce.
- `source: layer store drops writes when the queue is full` — the bounded
  write-behind queue drops (never back-pressures) under a stalled writer;
  drops are counted and reads stay correct.
- `source: layer store enters bypass on write failure` — `ENOSPC` and
  `EIO` from the writer thread both flip the store to `Bypass`; reads
  continue remotely, `populate` no-ops, and no further extent is persisted
  even after the injected failure stops.
- `source: layer store stays filling after a non-fatal write error` — a
  non-ENOSPC/EIO write failure drops only that entry: the store stays
  `Filling`, other extents persist, and the failed extent re-fetches.
- `source: layer store completes to overlaybd.commit and reopens read-only` —
  a fully-filled layer is sha256-verified, atomically renamed to
  `overlaybd.commit`, and a reopen binds it with zero remote reads.
- `source: layer store restarts on sha mismatch within try count` —
  completion verification failure discards the pair and restarts fresh,
  bounded by `try_count`, then degrades to remote-serving `Bypass`.
- `source: layer store populate warms extents without serving data` —
  `populate` delivers no data, persists the warmed extents, and they
  survive a reopen.
- `source: layer store coalesces concurrent fetches of one extent` — N
  concurrent preads of a missing extent join one in-flight remote fetch
  (exactly 1 fetch, N-1 joins).
- `source: layer store handles a tail extent at eof` — the short tail
  extent of a non-extent-aligned blob reads, persists, and CRC-verifies
  over its actual length.
- `source: layer store completes a fully-filled pair on reopen` — a
  staging pair whose records are all present (death between the last
  record write and the rename) is sha256-verified and renamed to
  `overlaybd.commit` immediately at reopen.
- `source: layer store accepts digest forms and rejects malformed` — the
  `sha256:` prefix and uppercase hex are accepted (normalized before
  comparison); malformed digests fail `open` with `EINVAL`.
- `source: layer store completes without verification when digest is empty` —
  an empty expected digest zero-fills the sidecar header (resume still
  matches) and completes to `overlaybd.commit` without sha256
  verification.
- `source: layer store completes an empty layer` — a zero-length remote
  creates an empty `overlaybd.commit` instead of remaining in `Filling`.
- `source: layer store fill completes a partially warmed store` — one
  read-through extent plus the background fill warm the whole layer to
  `overlaybd.commit` (`kDone`); a reopen serves everything locally.
- `source: layer store fill resumes from the sidecar across a restart` —
  a fill stopped mid-walk leaves persisted extents in the sidecar; a
  reopen resumes them without refetching and the restarted fill
  completes the layer.
- `source: layer store fill honors the throughput throttle` — a 1 MiB/s
  budget makes a 3 MiB fill take measurable seconds instead of
  milliseconds.
- `source: layer store fill stays off in bypass` — an injected `ENOSPC`
  mid-fill flips the store to `Bypass`: fill stops (`kStopped`), reads
  continue remotely, nothing persists, no commit appears.
- `source: layer store fill is disabled without download.enable` — with
  fill not configured there is no background traffic: `kDisabled`, zero
  extents present, zero remote reads.
- `source: layer store sweeps stale pairs when the commit binds` — a dir
  holding `overlaybd.commit` plus leftover `.download.*`/`.bitmap.*`
  files binds the commit and removes the pair files.
- `source: layer store fill does not starve readers` — with fill active
  and back-pressured (tiny queue, slow disk), cold-reader preads each
  complete within a bounded 1 s budget, byte-exactly.
- `source: registry concurrent 401s share one token refresh` — N
  concurrent reads on a server-side-expired token trigger exactly one
  coalesced token exchange (mock counts token endpoint hits); all reads
  succeed (single-flight, ADR-0015).
- `source: registry failed token refresh reaches all concurrent waiters` —
  when the coalesced exchange itself fails, every waiter receives the
  error (no hang, no wrong success) with exactly one exchange attempted,
  and the next request after the endpoint recovers starts a fresh flight
  (the key is not poisoned, ADR-0015).
- `source: registry 401 retry budget is bounded when re-auth keeps failing` —
  when every fresh token is still rejected on data GETs, the request fails
  with `-EPERM` after its retry budget with one exchange per attempt —
  no livelock (ADR-0015).
- `source: registry re-auths after expires_in lifetime elapses` — with
  `expires_in=1` (800 ms cache lifetime) the token is reused inside the
  lifetime and re-fetched after it.
- `source: registry keeps the cached token within expires_in lifetime` —
  with `expires_in=100` no re-auth happens across repeated resolutions
  and reads far inside the 80 s cache lifetime.
- `source: registry survives hostile token endpoint fields` — a float
  `expires_in` far outside int64 range (`1e100`) is ignored rather than
  converted (no UB), a non-string `token` field maps to `-EINVAL`
  through the `-errno` discipline instead of escaping as a raw exception,
  and a partially-numeric string `expires_in` (`"1junk"`) takes the
  fallback while a fully-numeric string (`"1"`) is honored (ADR-0015).
- `source: registry token cache lifetime derives from expires_in` — the
  pure mapping: 80% of the declared lifetime, 0 for `expires_in=0`, 30 s
  fallback for absent/negative values, and the 7-day cap for absurd ones
  (2^62, int64 max, and around the ceiling) (ADR-0015).
- `source: admission funnel admits on-demand unconditionally under a full window` —
  with the window fully occupied by scavengers, on-demand acquires
  complete immediately and in-flight exceeds the window (ADR-0012).
- `source: admission funnel blocks scavengers while on-demand is in flight` —
  queued Prefetch/Fill requests stay parked while an on-demand request
  is in flight, even with window room, and enter once it completes.
- `source: admission funnel bounded scavenger acquire times out and dequeues` —
  issue #35: with the gate held closed, the bounded acquire returns
  `std::nullopt` after its timeout and leaves no ghost reservation
  (counters and a subsequent acquire prove the dequeue); admitted
  immediately when the gate is open, admitted mid-wait when the gate
  opens before the timeout, and refused outright (`std::nullopt`, no
  slot taken) when mis-called with OnDemand.
- `source: admission funnel grows additively on flat latency and halves on rise` —
  flat samples at the EMA baseline raise the window by one each; a
  sample above 150% of the baseline halves it; the drifted baseline
  reclassifies subsequent samples (AIMD, ADR-0012).
- `source: admission funnel respects window floor and ceiling` — sustained
  flat samples stop at `window_max`; samples that keep outrunning the
  EMA baseline drive the window to `window_min`, never below.
- `source: admission funnel admits prefetch before fill` — with room for
  exactly one scavenger, a queued Prefetch wins the freed slot over a
  queued Fill (two-level scavenger queue, ADR-0012).
- `source: admission funnel caps scavenger request size` — scavenger
  requests clamp to the ~1 MiB cap (custom caps honored); OnDemand is
  uncapped.
- `source: admission source splits populate at the scavenger size cap` —
  `AdmissionSource::populate` forwards successive ≤ 1 MiB chunks at
  successive offsets as Prefetch admissions; `pread` is OnDemand.
- `source: layer store populate waits at the admission funnel while reads pass` —
  the LayerStore class wiring end to end: with the window held full, a
  miss `pread` (OnDemand) completes anyway while a `populate`
  (Prefetch) queues until a slot frees (ADR-0012).
- `source: layer store populate skips the extent when the funnel gate stays closed` —
  issue #35: with `populate_admit_timeout` set and the gate held closed
  by an on-demand permit, `populate` fails the extent with `-EAGAIN`
  within the bound (nothing reaches the remote) and succeeds once the
  gate opens.
- `source: admission funnel re-checks the gate when queueing a scavenger` —
  lost-wakeup regression: with the check-then-queue gap injected by the
  test hook (a slot freed against empty queues inside it), the
  push+re-admit critical section still admits the scavenger — without
  the re-check the acquire never wakes (ADR-0012).
- `source: layer store fill frees the funnel window before throttling` —
  fill's Fill-class permit covers the remote fetch alone: a Prefetch
  queued behind the full window is admitted immediately after fill's
  fetch completes, not after fill's 1 s `maxMBps` throttle sleep
  (prefetch outranks fill; throttle and funnel stay separate).
- `source: populate joining an in-flight fetch bypasses the funnel` —
  dedup ordering (ADR-0012): a `populate` for an extent already being
  fetched joins the in-flight fetch (coalesced join) and consumes no
  scavenger admission and no queue event at the funnel.
- `source: layer store fetch frees the funnel slot before retiring the fetch` —
  the starter's funnel permit covers exactly the remote fetch: the
  fetch-done hook observes the slot already released before the
  completion bookkeeping (in-flight map retire) runs (ADR-0012).

### image

- `image: global config parses overlaybd.json fields` — credential,
  p2p, download, prefetch (`enable` honored, ADR-0012; `head_kb`/
  `tail_kb` pinned separately), and log
  sections parse with the documented defaults.
- `image: per-image download overrides merge over global defaults` —
  only fields present in the image's `download` section override.
- `image: upper config parses; unknown type rejected` — `lsmt`/`sparse`
  parse; any other `upper.type` fails with `EINVAL` (ADR-0008).
- `image: assembly from local layer files reads merged content` —
  `open_image` over local lowers yields the correctly merged block view.
- `image: assembly picks the ZFile view for compressed layers` —
  compressed blobs are detected and transparently decompressed during
  assembly.
- `image: writable upper assembles and serves writes` — a config with
  `upper.dir` opens a `MergedWritable` root that accepts and serves
  back writes (ADR-0008).
- `image: trace replay populates traced extents in recorded order` —
  records interleaving two lowers issue `populate` on the right target in
  the trace's exact order (ADR-0013).
- `image: trace replay skips unknown ops, layers and bad records` —
  non-READ ops, unknown/null layer indexes, zero and > 1 MiB counts, and
  negative offsets skip silently; a failing populate and a malformed blob
  degrade to "no prefetch", never an error.
- `image: trace replay enforces record, byte and time budgets` — the
  `max_records` / `max_bytes` / `max_wall_time` bounds each stop replay
  early with `budget_exhausted` set.
- `image: trace recording round-trips through the codec reader` — a
  recorded blob parses with the C2 reader (header checksum rewritten on
  finalize) and the finalize stats match the file (ADR-0013).
- `image: trace recording coalesces adjacent records and preserves order`
  — same-layer continuations merge within the 1 MiB cap; disjoint reads
  keep their exact order.
- `image: trace recording splits reads beyond the conforming count cap` —
  an oversized read lands as consecutive ≤ 1 MiB records.
- `image: trace recording drops and counts records when the buffer fills`
  — overflow sheds whole chunks, `dropped` is surfaced, and the blob
  stays valid and replayable.
- `image: trace recording skips partial and failed reads` — short reads
  and read errors record nothing.
- `image: trace recording is pass-through and error-clean when idle` — an
  idle tap reads and reports errors exactly like the wrapped source.
- `image: trace recording stop is idempotent and reports expiry stats` —
  the device-side timer finalizes with no client call; a late stop
  returns the same stats.
- `image: trace recording captures only remote fetches through the layer store`
  — local hits record nothing; misses record exactly the fetched
  extents.
- `image: trace recording translates offsets out of the tar wrapper` —
  records address payload space; header-spanning fetches clamp to their
  payload overlap.
- `image: trace recording finalizes an empty window to a valid header-only blob` —
  a stop before any record still runs the header checksum rewrite; the
  24-byte blob parses with the C2 reader.
- `image: trace recording restarts cleanly after a stop` —
  start→stop→start opens a fresh window (queue/drop counter reset) and
  both blobs stay valid.
- `image: trace recording rejects start while a finalize is in flight` —
  a start meeting an in-flight finalize (held open by the test hook) is
  rejected with "already in progress" and the finalize completes with
  its records and stats intact.
- `image: trace recording rejected start never truncates existing files` —
  the state gate runs BEFORE the output open: a rejected start leaves
  a previous recording's valid blob byte-identical and the active
  window's own finalize complete.
- `image: trace recording start race truncates the output exactly once` —
  two concurrent starts on the same path (made deterministic by the
  test-only start hook): the loser is rejected without touching the
  file, only the winner truncates (under the state lock), and the
  winner's finalize produces a complete valid blob.
- `image: trace recording drops out-of-range offsets instead of corrupting` —
  an offset past INT64_MAX (or one whose count overflows int64) is
  dropped + counted, never cast into a negative blob offset.
- `image: local trace layer is set aside and replayed at open` — an
  `accelerationLayer: true` image with a local `<dir>/trace` blob opens
  with the trace layer excluded from the merged view and the trace fully
  replayed (ADR-0013).
- `image: prefetch enable false skips trace replay but keeps recognition` —
  with `prefetch.enable = false` the trace blob is neither loaded nor
  replayed (stats zero), yet the acceleration layer is still excluded
  from the merge — recognition is structural, not gated (ADR-0012).
- `image: garbage trace layer never fails assembly` — a garbage trace
  blob still yields a working, byte-exact device (opportunistic replay).
- `image: writable image with a trace layer assembles and replays` — a
  writable (`upper`) image with `accelerationLayer` still sets the trace
  layer aside, replays it, and serves copy-on-write reads/writes.
- `image: structural warm-up windows clamp and merge on small blobs` —
  the pure window computation: disjoint head/tail windows at the exact
  edges, per-side clamping, a single merged full window for any blob
  smaller than head+tail (no double-population), 0 disabling a side,
  and no windows for an empty blob.
- `image: structural warm-up populates head and tail windows opportunistically` —
  the driver issues head-before-tail per layer in layer order (each
  window in 64 KiB slices), merges a small blob into one window, skips
  nullptr targets, counts failing and throwing populates without
  propagating them, issues nothing with both window sizes 0, and stops
  early on the wall-time budget (abandoning the in-flight window).
- `image: structural warm-up budget interrupts a slow window` — the
  wall budget is real mid-window, not just between windows: with a
  source sleeping 250 ms per extent, a 300 ms budget interrupts the
  first window after one or two 64 KiB slices (slice granularity and
  the elapsed bound both fail red against whole-window population),
  counted `windows_skipped` with `budget_exhausted` set and the
  remaining windows/layers never started.
- `image: structural warm-up skips windows the funnel will not admit` —
  issue #35: a populate reporting `-EAGAIN` (gate closed past the admit
  timeout) skips that window — `windows_skipped`, not
  `windows_failed` — and warm-up moves on without waiting; both windows
  of the layer are skipped at their first slice, nothing counted
  warmed, and the pass is not budget-exhausted.
- `image: prefetch config parses structural window knobs` — the
  `prefetch` section's honored subset (`enable`, `head_kb`, `tail_kb`)
  parses with the documented defaults; 0 window sizes are kept; partial
  sections default field by field; out-of-range window sizes (negative,
  or above the uint32 range) are rejected with `EINVAL` at parse time.
- `image: prefetch enable false skips structural warm-up` — with
  `prefetch.enable = false` no structural warm-up runs (stats zero)
  while the device assembles and reads byte-exactly; enabled, the local
  lower's merged window is populated (ADR-0012).

### ublk

- `ublk: command buffer geometry matches the driver layout` — the
  command-buffer stride and descriptor layout match `<linux/ublk_cmd.h>`
  exactly (uapi compatibility).
- `ublk: IoRequest byte math is sector based` — sector↔byte conversion
  in `IoRequest` is exact, including boundary sector counts.
- `ublk: recovery feature flags follow device params` — `dev_info_flags`
  adds `UBLK_F_USER_RECOVERY | _REISSUE` exactly when
  `DeviceParams::enable_recovery` is set (ADR-0010).

### supervisor

- `supervisor: protocol commands parse and reject garbage` — the
  control-protocol parser accepts the four commands and rejects
  malformed JSON, missing fields, and unknown commands.
- `supervisor: hello handshake replies with protocol version and features` —
  the `hello` reply carries an integer `protocol` ≥ 1, a non-empty
  `version` string, and a `features` array; `hello` requires no fields and
  ignores extras; unknown cmds and malformed JSON stay answered errors
  (ADR-0014).
- `supervisor: child spawn execs and reports through the channel` — a
  spawned child's fd-3 status lines reach the parent and drive the
  ready event.
- `supervisor: exec failure surfaces as exit 127` — a missing device
  binary yields EOF on the status channel plus exit code 127.
- `supervisor: bdev path parses to device id` —
  `dev_id_from_bdev_path` extracts the id from `/dev/ublkbN` and
  rejects non-bdev paths (ADR-0010).
- `supervisor: recover spec adds the recover flag to child argv` — a
  recovery respawn passes `--recover` and `--dev-id` to obd-device
  (ADR-0010).
- `supervisor: commit command parses and validates its fields` — `commit`
  requires a string `id`, accepts an optional string `user_tag` (wrong
  field types are parse-time protocol errors, not handler exceptions),
  ignores unknown fields, and the `hello` reply pins the `protocol` field
  plus the `commit` feature advertisement (ADR-0014).
- `supervisor: device trace control answers malformed-typed fields with clean errors` —
  the device-side trace command loop (`src/supervisor/device_control.hpp`)
  over a real socketpair: a `trace_start` with a wrong-typed `path` or
  `duration_sec` (including a float, a negative, and a huge integer)
  gets a clean error reply (with the `seq` correlation echoed) instead
  of an escaping `type_error` killing the loop, and a valid start/stop
  cycle afterwards proves the loop stayed alive (ADR-0013).
- `supervisor: device trace control skips an oversized line and stays alive` —
  the same loop over a real socketpair: a command line larger than the
  64 KiB cap (no newline inside) is discarded rather than mistaken for
  channel EOF, and a valid start/stop cycle afterwards proves the loop
  stayed alive (ADR-0013).
- `supervisor: control channel writer loops short writes and never throws` —
  the serialized channel writer loops ::write until the whole line is
  out (a short write would truncate/fuse protocol lines); a line larger
  than half the capacity of a nonblocking pipe (filled to capacity,
  drained halfway — no fixed pipe size assumed) deterministically
  short-writes then EAGAINs and is reported (false), never thrown,
  while a normal line over a socketpair lands intact (ADR-0013).
- `supervisor: control channel writer survives a closed peer without SIGPIPE` —
  an EPIPE (the supervisor vanishing mid-write) is reported as a dropped
  line (false) instead of SIGPIPE-terminating the process: sockets are
  written via `send(MSG_NOSIGNAL)` (ADR-0013).

### integration

- `integration: layered stack stages over a mock registry` — the full
  registry→layer store→tar→zfile→lsmt→merge chain serves correct bytes
  for a multi-layer image.
- `integration: cancelled connect probe does not break later io` — a
  cancelled reachability/connect probe leaves the HTTP stack usable for
  subsequent reads.
- `integration: enabled-but-unreachable DART falls back to direct reads` —
  with `p2pConfig.enable` on and the proxy down, image open falls back
  to direct registry reads within the probe budget (ADR-0005).
- `integration: registry pipeline serves a zfile-compressed image` —
  end-to-end correctness for a compressed image fetched over HTTP.
- `integration: image assembly serves remote reads through the layer store` —
  a remote lower with a configured `dir` assembles through
  RegistrySource → LayerStore and reads back byte-exactly, leaving
  read-through persistence state in the layer dir (ADR-0011).
- `integration: trace layer replays warm-up through the layer store` — an
  `accelerationLayer: true` image with a tar-wrapped trace layer on a
  multi-blob mock registry: the trace is recognized, set aside from the
  merge, and its records warm the data layer through
  TarOffsetSource::populate → LayerStore::populate — the tar-header
  translation is pinned by attributing fetched extents only the replay can
  reach — and the device serves the data layer byte-exactly (ADR-0013,
  proposed).
- `integration: trace replay warms the lower addressed by layer index` —
  two remote dir-configured data layers: a `layer_index` 1 record warms
  an otherwise-untouched extent of layer 1's blob only, pinning the
  warm-target ordering end to end (ADR-0013).
- `integration: layer store restart serves warmed extents without remote reads` —
  a partially-warmed staging pair is resumed by a second `open_image`:
  repeated reads are served locally and the mock registry's remote-read
  counter does not increase.
- `integration: completed layer store commit binds read-only without remote reads` —
  a LayerStore driven to completion renames its staging file to
  `overlaybd.commit`; a reopen binds it via the local probe and serves
  byte-exact reads with zero remote data reads.
- `integration: background fill completes a layer through image assembly` —
  with `download.enable` set, the first open reads a prefix while the
  background fill warms every remaining extent to `overlaybd.commit`; the
  second open binds the commit with zero additional remote reads.
- `integration: admission funnel bounds on-demand latency under scavenger load` —
  a serialized, latency-injected mock registry (capacity 1, 25 ms
  service) under a six-coroutine populate storm plus the background
  fill: twelve sequential on-demand image reads each complete within a
  bounded 2 s, byte-exactly, and the funnel's `scavenger_waits` counter
  proves the storm was really throttled (ADR-0012 acceptance).
- `integration: admission funnel collapses scavenger traffic under on-demand contention` —
  two lowers sharing the one per-device funnel: while eight concurrent
  on-demand readers stream cold extents of the top layer, the shadowed
  bottom layer's fill makes essentially no progress, and resumes once
  the contention stops (ADR-0012 acceptance).
- `integration: structural warm-up fetches head and tail extents at bring-up` —
  the ADR-0012 cold-start floor end to end: with warm-up enabled
  (256 KiB windows), `open_image` alone — no device read — fetches a
  head extent and a tail extent only the warm-up can reach (per-extent
  attribution on the multi-blob mock), while a middle extent stays cold;
  extent 4, reachable only through the +512 tar-base translation of the
  head window, pins the windows to the tar-VIEW byte space; with
  `prefetch.enable = false` the same extents stay cold and the
  device still reads byte-exactly.
- `integration: structural warm-up runs before the trace blob load` —
  ADR-0012 "floor first": via the mock's ordered cross-blob request log,
  BOTH warm-up windows of the data blob (a warm-up-only head extent and
  the tail window's first extent) are served BEFORE the trace blob's
  first data GET — a head→trace→tail regression order fails red on the
  tail check — while replay of a traced middle extent still completes
  and the device reads byte-exactly.
- `image: malformed remote lower digest fails assembly` — a malformed
  `sha256:` lower digest fails assembly with `EINVAL` before any registry
  I/O (ADR-0016 boundary: structural config errors fail loud).
- `integration: unwritable layer dir degrades to remote-only reads` — a
  `lower.dir` that can never be created does not fail `open_image`: the
  image boots, serves byte-exact remote-only reads, and writes no
  persistence state (ADR-0016).
- `supervisor: crashed device child is recovered with bounded respawns` —
  a real daemon with a fake obd-device: crash → respawn with
  `--recover`, bounded at `max_recovery_attempts`, `recoveries` reported
  in status, destroy still works (ADR-0010). Runs without privileges.
- `supervisor: daemon answers hello and never drops bad input` — a real
  daemon over its control socket: `hello` returns the documented
  handshake shape end to end, an unknown cmd is answered with an error,
  and malformed JSON is answered with an error rather than dropped
  (ADR-0014). Runs without privileges.
- `supervisor: commit stops the device and seals its upper offline` — a
  real daemon with a signalfd-based fake obd-device
  (`tests/fake_device_main.cpp`) that, like the real device, checkpoints
  its LSMT-RW upper only on SIGTERM — so a commit that sealed without
  stopping the device first could not succeed (stop-then-seal is pinned,
  not just narrated): commit on unknown id, sparse upper, and upper-less
  devices fails with precise errors; malformed field types (non-string
  `id`/`user_tag`) are answered as protocol errors and the daemon keeps
  serving; editing the config file after create
  does not redirect commit (upper path provenance); commit on a live
  LSMT-upper device stops it (bounded reap) and seals its checkpointed
  upper, replying `path`/`sha256`/`size`; a second commit fails with
  "already sealed"; the sealed file re-opens as a valid LSMT RO layer
  with the fake's payload (ADR-0014). Runs without privileges.
- `supervisor: concurrent commits are serialized and reject the loser` —
  two barrier-synchronized commits of the same device: exactly one
  succeeds, the loser gets a precise error ("commit already in progress"
  or "already sealed"), and the sealed file is intact (no interleaved
  tmp-file writes) (ADR-0014). Runs without privileges.
- `integration: trace recording captures remote reads end to end` — a
  real daemon with the extended fake obd-device (opens a REAL image
  against the mock registry, speaks the real device-side trace protocol,
  and runs a scripted read workload when recording starts): start →
  workload → stop returns `{path,sha256,size,records,dropped}`, the
  additive `trace` status field reports recording then stopped, and the
  blob parses with the codec reader to exactly the workload's coalesced
  record (ADR-0013). Runs without privileges.
- `integration: trace recording duration expiry finalizes without a client call` —
  the device-side timer finalizes on its own; `status` reports
  `"state":"stopped","reason":"expired"` and a late stop returns the
  same stats (ADR-0013). Runs without privileges.
- `integration: trace recording survives client disconnect mid-record` —
  a client that sends `trace_start` and vanishes without reading the
  reply cannot leak a recording device: the duration bound still
  finalizes and the supervisor keeps serving (ADR-0013). Runs without
  privileges.
- `integration: trace recording crash mid-record marks the trace lost` —
  a SIGKILLed device's `trace` status flips to
  `"state":"lost","reason":"device_exit"`, the never-finalized output
  file stays a 0-byte non-blob, and the daemon keeps serving
  (ADR-0013). Runs without privileges.
- `integration: trace recording rejects bad requests cleanly` — missing
  or wrong-typed fields (protocol parse), unknown ids, idle stops,
  out-of-bounds durations, and double starts are precise errors that
  leave the daemon and the active recording unaffected (ADR-0013). Runs
  without privileges.
- `integration: ublk device serves sector reads from a blob` — the
  privileged E2E: a real ublk device backed by an in-memory blob
  returns correct sectors through `/dev/ublkb<N>` (self-skips without
  `/dev/ublk-control`).
- `integration: ublk writable device serves writes and discard` — a
  writable device (in-memory writable root) answers block-device
  writes, and a `BLKDISCARD` ioctl reaches `discard()` with
  mask-with-zeroes read-back (ADR-0009; self-skips without ublk).
- `integration: ublk device survives server death via USER_RECOVERY` —
  a forked server is SIGKILLed; the block device survives QUIESCED and
  a replacement server completes the real
  START/END_USER_RECOVERY handshake and keeps serving reads
  (ADR-0010; self-skips without ublk or on kernels without the
  feature).
