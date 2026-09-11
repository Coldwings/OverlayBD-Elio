# Test Strategy

How overlaybd-elio is tested: layout, fixtures, naming, golden-value
policy, how to run, and the full inventory of tests with the property
each guards.

## Layout

| Location | Contents | Built into |
|---|---|---|
| `tests/unit/` | Catch2 unit tests, one file per module area | `obd_unit_tests` |
| `tests/integration/` | End-to-end tests over a mock registry; the ublk E2E | `obd_integration_tests` |
| `tests/scripts/` | Source-guard regression tests | Python standard library, via CTest |
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
// WRONG — keep suspension points outside assertion macro arguments.
// The bad form below is an example only:
//   REQUIRE(  co_await  src->pread(buf.data(), buf.size(), 0)  == 1024 );

// RIGHT — await once into a named local, then assert on the value.
const ssize_t got = co_await src->pread(buf.data(), buf.size(), 0);
REQUIRE(got == 1024);
```

Catch2 expands the assertion argument into expression-decomposition and
compiler-checking code. Repeated appearances in that expansion do not
by themselves imply repeated runtime evaluation: some appear in branches
designed never to execute. Historical issue #14 reported repeated token
exchanges, but that observation alone does not establish a general Catch2
multiple-evaluation mechanism or identify a compiler/runtime cause.

The project keeps a conservative rule: the `REQUIRE` and `CHECK` runtime
assertion families listed below must never wrap `co_await` directly, whether
the awaited expression is compared, negated, or spans several lines. Assign
the awaited result to a named local first, then assert on the local. This makes the
suspension and operation order explicit, independent of assertion macro
expansion. The source guard below enforces that rule; it does not prove a
runtime failure mechanism for the prohibited spelling.

CTest enforces this existing rule with `test-await-assertions`, which runs
`scripts/check_test_awaits.py` over all C++ sources and headers in `tests/`
(including ublk sources even when that backend is disabled).
`test-await-assertions-selftest` checks rejection and harmless-text cases.
Both have a 30-second timeout. Python 3.8+ is an explicit configure-time
requirement when `OBD_BUILD_TESTS=ON`; the guard is never silently skipped.

The guard recognizes the `REQUIRE` and `CHECK` runtime assertion families:
the base macros and `_FALSE`, `_THROWS`, `_THROWS_AS`, `_THROWS_WITH`,
`_THROWS_MATCHES`, `_NOTHROW`, and `_THAT`. It balances parentheses across
lines, including nested calls and lambda bodies, while ignoring comments,
quoted/character literals, and raw strings. Any `co_await` token inside an
assertion's arguments is rejected, including inside a nested lambda. This
also means assigning a synchronous `test::run_coro(...)` result to a local
before asserting when its coroutine lambda contains awaits. Such a
synchronous wrapper does not itself directly await in the assertion; the
same spelling rule keeps the guard simple and consistent.
This is a conservative lexical check, not a C++ parser: it does not expand
macro aliases, process line splices, or evaluate conditional compilation
(disabled branches are checked too). Keep coroutine work outside assertion
arguments instead of wrapping it in another macro.

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

The `build-warning-coverage` CTest entry (Makefiles/Ninja generators) checks
the generated compile command for every project translation unit, including
CLI binaries and test helpers, for `-Wall -Wextra` and, when requested,
`-Werror`. This catches a target omitted from the warning policy even when
its current sources happen to compile cleanly. CI's ordinary build opts in
with `-DOBD_WARNINGS_AS_ERRORS=ON`; local builds retain the default `OFF`.
For strict local verification, configure with that option and build all targets.

Catch2 tests are registered with CTest via `catch_discover_tests`, so each
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
`integration: ublk device survives server death via USER_RECOVERY`,
`integration: ublk device grows online and serves the new capacity`)
need `/dev/ublk-control` and root (or `CAP_SYS_ADMIN` in a suitable
user namespace) with `ublk_drv` loaded. They **self-skip** via Catch2
`SKIP()` when the kernel facility is absent, and
`obd_integration_tests` is registered with `SKIP_RETURN_CODE 4` so an
all-skipped run is not a failure. **Never make the default test run
depend on privileged kernel state** — new privileged tests must
self-skip the same way. The privileged CI job
(`build-test-ublk-privileged` in `.github/workflows/ci.yml`) loads
`ublk_drv` and runs these tests individually by exact name under `sudo`,
with a separate bounded step and log for each. The online-grow step has a
120-second process timeout and records the commit, kernel, command exit status
and the test-output `tee` exit status in `build/ublk-e2e-grow.log`, included
in the `ublk-e2e-logs` artifact. On a supported driver it must verify capacity,
grown-region write/read and original content. Only exit 4 accompanied by
the test's explicit `kernel driver lacks UBLK_U_CMD_UPDATE_SIZE` reason is
accepted as an unsupported-driver skip, reported as a workflow warning;
this is **not supported-grow coverage**. Missing control access, unexplained
all-skipped results, other test failures, signals, timeouts and log-write
failures fail the step. Failure diagnostics include available kernel and
core information. Local unprivileged skips do not prove the privileged path.

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
- `format: lsmt rw seal rebinds live reads to the compacted inode` —
  successful live `seal()` keeps the original layer, a retained
  `data_source()` reference, current `segments()` and a fresh RO reopen
  coherent with the compacted published inode.
- `format: lsmt rw failed live seal keeps original backing` — a live
  seal serialization exception removes the unpublished output, preserves
  the original backing descriptor and UUID, and still allows a later
  checkpoint/offline seal.
- `format: lsmt rw direct terminal and grow gates reject overlaps` —
  direct overlapping `grow()`/`checkpoint()`/`seal()` calls return
  `-EBUSY` while the in-flight operation retains its gate, and the
  rejected operation can be retried after release.
- `format: lsmt rw seal is deterministic for identical content` —
  identical upper content seals to byte-identical files with equal
  content-derived uuids (ADR-0014 seal determinism); a different write
  sequence yields a different digest, as does identical data with a
  different packed index (zeroed segments — pinning that the index, not
  only the data, feeds the digest).
- `format: lsmt rw destruction releases its backing descriptor` — repeated
  create/read/reset cycles leave no descriptor for the backing device/inode.
- `format: lsmt rw offline seal releases replaced inode descriptors` —
  checkpoint/offline seal releases its reopened old inode and digest descriptors;
  destroying the creator releases its remaining reference, and data round-trips.
- `format: lsmt rw rejected seal releases its reopened descriptor` — unaligned
  and shrinking overrides plus output-open failure preserve the checkpoint and
  release the reopened owner.
- `format: lsmt rw seal exceptions release temporary descriptors` — a header
  serialization exception closes both temporary descriptors and removes the
  compaction file while preserving the original checkpoint.
- `format: lsmt rw setup failures release their descriptors` — short files,
  invalid trailers/index entries, and `/dev/full` header-write errors release
  their descriptors while a separate live layer remains readable.
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
- `format: empty sealed lsmt layer is a zero base of its virtual size` —
  the ADR-0014 blank-device zero base: a sealed LSMT layer with an empty
  index opens with the requested `virtual_size` and reads the whole
  range as zeroes through the normal merge path.
- `format: empty lsmt layer bytes are deterministic per virtual size` —
  identical sizes produce byte-identical sealed empty layers (content-
  derived uuid pinned against an independent digest), different sizes
  differ (ADR-0014 determinism).
- `format: merged writable falls through and copy-on-writes` —
  `MergedWritable` reads fall through the upper to sealed lowers, and
  writes shadow lowers copy-on-write without mutating them (ADR-0008).
- `format: lsmt rw discard masks coverage with zeroed segments` —
  discard inserts zeroed segments that read back as zeroes and survive
  `seal()` into a valid standard LSMT file (ADR-0009).
- `format: lsmt rw pwrite over a discarded range appends fresh data` —
  rewriting a discarded range appends at the data end instead of
  in-place-reusing the zeroed segment's placeholder `moffset` (sector 8),
  so the surviving live neighbor's data is never clobbered and no two live
  segments share one physical span; the round trip survives `seal()` and
  seals byte-identically to a plain overwrite of the same content (issue
  #13).
- `format: lsmt rw straddling pwrite keeps live and discarded parts apart` —
  a pwrite whose head lands on live data (in-place edit) and whose tail
  lands on discarded coverage appends exactly the discarded part, without
  writing through the placeholder `moffset` into the live head's physical
  blocks (issue #13).
- `format: lsmt rw partial zeroed rewrites survive offline sealing` —
  partial rewrites and repeated discards keep zeroed placeholder offsets
  valid through checkpoint/offline seal; reopened virtual reads preserve
  the patch, an unrelated live range and all remaining zeroes. Also covers
  a rewrite crossing the maximum segment/write-piece length (issue #13).
- `format: sparse layer discard punches holes and recovers` — discard
  is a real punch-hole; reads stay correct across a reopen even though
  fiemap recovery is filesystem-block granular (ADR-0009).
- `format: merged writable discard masks the lower layer` — with an
  LSMT-RW top, discarding a range covered only by the lower reads back
  zeroes, not the lower's data (mask semantics, ADR-0009). Sparse top
  lower-mask coverage is tracked in #85.
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
  validation at parse time; nonzero record padding is checksummed,
  not interpreted (trace-format.md §7/§8).
- `format: trace writer enforces the conforming-writer contract` — count 0
  and > 1 MiB, op 'W', and negative offsets are rejected
  (trace-format.md §10 rule 4).
- `format: offline seal rejects a virtual_size below the declared size` —
  the D3 commit re-baseline rejection side: `seal_file` with a
  `virtual_size` override smaller than the layer's declared size (or
  misaligned) returns `-EINVAL` with a precise reason BEFORE any
  compaction, and the upper stays committable at its declared size
  (ADR-0014).
- `format: offline seal re-baselines the sealed virtual size grow-only` —
  the D3 commit re-baseline acceptance side: an override at least the
  declared size (and content extent) is written into the sealed
  header/trailer and content digest, and the sealed layer re-opens with
  the larger declared size and byte-exact content (ADR-0014).
- `format: lsmt rw grow extends the write window and persists the size` —
  the D3 data-plane grow for the LSMT upper: after `grow()`, pwrite
  into the region past the original declared size succeeds and reads
  back; shrink/misaligned grow requests are rejected (equal is an
  idempotent no-op); the on-disk declared-size header is rewritten, so
  a checkpoint and a plain offline seal stay consistent and the sealed
  layer re-opens at the grown size with both content regions intact
  (ADR-0014).
- `format: sparse layer grow extends the write window` — the D3 grow for
  the sparse upper (ftruncate): pwrite/pread accept the new range and
  shrink is rejected (ADR-0014).
- `format: merged writable grows with its writable top` — the D3 merged-
  view grow: `MergedWritable::grow` extends the writable top first and
  then the merged view; pwrite/discard accept the grown range, an
  unwritten headroom gap reads as zeroes, and shrink is rejected
  (ADR-0014).
- `image: writable assembly grows to the virtual_size headroom override` —
  `open_image(..., override)` on the real writable assembly path sizes
  the writable top — and hence the merged data plane — at the override:
  writes past the lowers' content into the headroom land in the upper
  and read back through the merge (D3; ADR-0014).
- `format: offline seal rejects a virtual_size below the content extent` —
  the D3 re-baseline content-extent guard on a synthetic fixture whose
  checkpointed declared size was patched below its real content extent:
  the seal rejects with the precise "content extent" grow-only reason
  (ADR-0014).
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
- `source: registry self-mode URL cache follows bearer expiry` — an
  already-open Self-mode source on the same URL refreshes before data GETs
  once the bearer token passes its proactive expiry.
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
- `image: invalid download tryCnt is rejected at config boundaries` —
  zero, negative, and out-of-range JSON values fail before conversion, and
  programmatic zero fails at assembly entry before persistence fallback
  can swallow it.
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
- `image: blank device rejects an unusable workspace path` — the
  ADR-0014 workspace preflight: a regular FILE sitting where the
  per-device workspace must be is reported as a workspace error naming
  the path (create_directories sets an error_code while `exists()` stays
  true, so an `ec && !exists` check let it through and the failure
  surfaced later as a confusing `overlaybd.zero` error), before any layer
  work happens.
- `image: blank device assembles a zeroed writable upper` — no config:
  `open_blank_device` opens a writable root of the requested size over a
  sealed empty LSMT zero base; fresh reads are zero, writes land in the
  upper and read back, untouched ranges stay zero (ADR-0014 mode 2).
- `image: trace replay populates traced extents in recorded order` —
  records interleaving two lowers issue `populate` on the right target in
  the trace's exact order (ADR-0013).
- `image: trace replay skips unknown ops, layers and bad records` —
  non-READ ops, unknown/null layer indexes, zero and > 1 MiB counts, and
  negative offsets skip silently; a failing populate and a malformed blob
  degrade to "no prefetch", never an error, while failed populate requests
  still count toward `bytes_requested`.
- `image: trace replay enforces record, byte and time budgets` — the
  `max_records` / `max_bytes` / `max_wall_time` bounds each stop replay
  early with `budget_exhausted` set; `max_bytes` charges issued populate
  requests even on failure and stops before a nondivisible final record
  would exceed the remaining allowance.
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
- `image: trace recording stop drains an awakened duration timer` —
  explicit shutdown stop waits for a timer that already woke before it
  touched recorder state.
- `image: trace recording expiry callback is skipped when explicit stop wins` —
  a timer that already captured the expiry callback but loses finalization
  to an explicit shutdown stop returns that shutdown result without
  emitting an expiry callback.
- `image: trace recording shutdown joins expiry finalization` —
  shutdown stop joins an expiry-owned finalize while `recording()` is
  already false and returns the expiry result.
- `image: trace recording late stop waits for expiry callback completion` —
  a cached late stop waits for the timer's expiry callback tail to
  finish before returning.
- `image: trace recording timer losing stop race skips expiry callback` —
  if an external stop wins after the timer copied its callback but before
  it owns finalization, no expiry callback is emitted for the external
  stop result.
- `image: trace recording expiry callback stop never joins itself` —
  a stop task created re-entrantly from the expiry callback returns the
  captured expiry result without joining its own timer or stopping a
  restarted recording, and a callback-created start is rejected.
- `image: trace recording external stop during expiry callback drains timer` —
  a stop task created by another thread while the expiry callback is
  running still waits for the timer frame to finish before returning.
- `image: trace recording external start during expiry callback drains timer` —
  a start task created by another thread while the expiry callback is
  running waits for the stale timer drain instead of being rejected as
  callback-reentrant.
- `image: trace recording stale stop never drains a restarted timer` —
  a late stop waiting on an old timer drain stays bound to that old
  timer after a new recording starts.
- `image: concurrent trace stops join the winning finalize` — an
  expiry stop and explicit stop are held after both observe Recording;
  the losing stop waits for and returns the winner's finalized path,
  digest, reason, and counts instead of reporting no recording.
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
- `image: device capacity honors the virtual_size headroom override grow-only` —
  `device_capacity_bytes` (D3 create-time headroom): no override = the
  image's declared size; an override >= the image size is sanctioned
  headroom (equal is a no-op); a smaller override is rejected with a
  grow-only reason (ADR-0014).
### ublk

- `ublk: control task releases its device owner before final destruction` —
  actual control-loop EOF and frame/capture teardown precede final off-worker
  Device destruction, with one worker and one blocking thread.
- `ublk: failed setup drains partial state before rethrowing its error` —
  the create/attach cleanup routine preserves the original exception and
  destroys partial state off-worker, before and after bridge registration.
- `ublk: blocking stop waits for a slow source to finish` — a normal read
  completes after eight seconds while a dedicated off-worker caller drains;
  stop must not return before completion, and owners remain valid throughout.
- `ublk: async stop drains an idle bridge before source destruction` — real
  Device/bridge/eventfd shutdown on one worker, both parked and newly scheduled
  bridges, sequential repeated stop, and final off-worker source destruction.
- `ublk: async stop lets a source finish on the sole blocking thread` — normal
  pending read completion after shutdown starts, with one worker and one
  blocking thread; verifies source retention and completion before release.
- `ublk: async stop handles a partially initialized device` — the same drain
  and destruction path before queue setup. These nonprivileged tests bypass
  kernel registration, command mappings, queue rings and queue threads;
  each has a 30-second CTest timeout.
- `ublk: command buffer geometry matches the driver layout` — the
  command-buffer stride and descriptor layout match `<linux/ublk_cmd.h>`
  exactly (uapi compatibility).
- `ublk: IoRequest byte math is sector based` — sector↔byte conversion
  in `IoRequest` is exact, including boundary sector counts.
- `ublk: recovery feature flags follow device params` — `dev_info_flags`
  adds `UBLK_F_USER_RECOVERY | _REISSUE` exactly when
  `DeviceParams::enable_recovery` is set (ADR-0010).
- `integration: ublk device grows online and serves the new capacity` —
  the D3 privileged E2E: `Device::resize_blocking` issues
  `UBLK_U_CMD_UPDATE_SIZE`, the kernel gendisk reports the new
  capacity, writes in the grown region read back, and the original content
  still reads back; shrink attempts
  are rejected device-side before any kernel IO. Self-skips without
  `/dev/ublk-control` or on kernels whose driver lacks the command (it
  landed in the 6.16 cycle).
### supervisor

- `supervisor: protocol commands parse and reject garbage` — the
  control-protocol parser accepts the four commands and rejects
  malformed JSON, missing fields, unknown commands, and a `create`
  `dev_id` outside `[-1, INT32_MAX]` (in range: accepted; `4294967296`
  and `-2`: clean parse errors naming `dev_id`, never a thrown
  `get<int>()`).
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
- `supervisor: mkfs runner maps exit codes and bounds the timeout` — the
  real default mkfs runner against throwaway PATH shims: exit 0 → ok,
  exit 1 → code+message, absent `mkfs.<type>` → the 127 "not found"
  mapping, an unsafe type refused before argv, and a shim sleeping past
  the bound reported as `-ETIMEDOUT` (helper SIGKILLed) — the runner owns
  its child (ADR-0014 mode 3).
- `supervisor: blank spawn argv carries the blank flags and global` — a
  blank child's real argv is `--blank-size N --blank-dir D --global G
  --control-fd 3` with no `--config`, pinning the spawn contract for both
  creation modes.
- `supervisor: commit is refused while a mode-3 create is still in mkfs` —
  the ADR-0014 unsealable-upper rule under concurrency: a mode-3 create is
  parked inside its mkfs step (the mock runner's release gate) while the
  device is already created and reachable, then a `commit` for that id
  must be refused with the "host mkfs" error and must NOT stop the device
  being formatted — a rule keyed on mkfs having finished would seal the
  supervisor-formatted upper here; the same create is also refused after
  mkfs completes.
- `supervisor: the reaper collects helpers a runner had to abandon` — a
  mode-3 mkfs helper that outlives its bounded post-SIGKILL reap is handed
  to the reaper (`MkfsRunner::take_orphan_pids()`) instead of parking a
  detached task on it: the test registers a real forked helper as
  abandoned plus an already-reaped pid (whose wait answers `ECHILD`, which
  must be terminal for that entry) and requires the daemon to reap the
  live helper (the test's own `waitpid` then answers `ECHILD`) — a
  returned pid means the zombie survived.
- `supervisor: stale blank-create failure leaves a newer device alone` —
  F1/F3-scale concurrency: a create parked inside its (slow) mkfs step,
  the id destroyed and re-created underneath it, then the stale mkfs
  failure released: the stale cleanup must leave the NEWER device
  untouched (same pid, still served) and the daemon healthy.
- `supervisor: obd-device rejects malformed blank flags` — the real
  obd-device binary's own `--blank-size` validation (negative, zero,
  unaligned, junk, overflowing, above the 16 TiB bound, missing
  `--blank-dir`, and `--config` combined with blank) is a usage error
  (exit 2) before any device work — obd-device is a standalone entry
  point, not only a supervisor child.
- `supervisor: fake device rejects malformed blank flags like obd-device` —
  the same matrix against the test-only fake device binary: its
  `--blank-size` parsing must accept and reject exactly what the
  production binary does (a `std::stoull` parse used to turn `-512` into
  1.8e19 and drive blank-mode integration tests with a size production
  refuses); a well-formed size paired with an unwritable workspace exits 1
  (failed device), proving the validator is not merely rejecting
  everything. The wait is bounded, so a validator regression fails the
  assertion instead of hanging the job (#11).
- `supervisor: create blank spec parses and validates size and mkfs` —
  the ADR-0014 blank create grammar end to end: `create` parses with a
  `blank` object (mode 2, and mode 3 with `mkfs`); `config` and `blank`
  are mutually exclusive with one mandatory; wrong-typed
  `blank`/`size`/`mkfs` are parse-time errors; `parse_blank_spec`
  rejects zero, unaligned, and oversized sizes and unsafe `mkfs` types
  (`valid_mkfs_type`), accepting `ext4`/`xfs` (ADR-0014).
- `supervisor: device trace control answers malformed-typed fields with clean errors` —
  the device-side trace command loop (`src/supervisor/device_control.hpp`)
  over a real socketpair: a `trace_start` with a wrong-typed `path` or
  `duration_sec` (including a float, a negative, and a huge integer)
  gets a clean error reply (with the `seq` correlation echoed) instead
  of an escaping `type_error` killing the loop, and a valid start/stop
  cycle afterwards proves the loop stayed alive (ADR-0013).
- `supervisor: trace start is rejected after shutdown admission closes` —
  a queued `trace_start` behind the device shutdown admission gate is
  rejected with a clean shutting-down error and never arms a recorder
  timer after shutdown has begun (ADR-0013).
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
- `supervisor: resize command parses and validates its fields` — the D3
  resize command requires a string `id` and a non-negative integer
  `size` (bytes); wrong-typed, negative, or float `size` are clean
  parse-time protocol errors, and the `hello` reply advertises the
  `resize` feature with the protocol version bumped by the D3 batch
  (ADR-0014).
- `supervisor: create virtual_size override parses and validates` — the
  D3 headroom override is an optional non-negative integer on `create`;
  other types are clean parse-time errors (grow-only/alignment live in
  the handlers) (ADR-0014).
- `supervisor: commit virtual_size override parses and validates` — the
  D3 re-baseline override is an optional non-negative integer on
  `commit`; other types are clean parse-time errors (the grow-only
  rules need the checkpoint, so they live in the seal path)
  (ADR-0014).
- `supervisor: device resize executor grows and rejects shrink or no-op` —
  the device-side `run_device_control` "resize" branch over a real
  socketpair: a grow is applied once (seq echoed, new size reported),
  an equal (no-op) or smaller (shrink) request is a clean ok:false
  "grow-only" reply that never reaches the apply seam, misaligned
  sizes are clean errors, and a recorder-less device still serves
  resize while trace commands answer "unavailable" (D3; ADR-0014).
- `supervisor: device resize answers unsupported without a seam and survives malformed sizes` —
  the same loop: a device without a resize executor seam answers resize
  with a clean "unsupported" error; wrong-typed `size` (float, negative,
  missing) are clean error replies — never an exception escaping the
  loop — and a valid grow afterwards proves the loop stayed alive (D3).
- `supervisor: resize executor grows the data plane first and rejects during shutdown` —
  `make_resize_apply` (the exact closure obd-device installs as its
  resize seam), tested without a device or kernel: it grows the writable
  data plane BEFORE the kernel gendisk, passes a read-only image
  straight to the kernel grow, surfaces a data-plane failure without
  touching the kernel and a kernel failure without swallowing it, and —
  once the shutdown flag is set — rejects with
  `ECANCELED`/"device is shutting down" WITHOUT calling either grow
  (the guard that keeps a post-checkpoint header rewrite from making the
  upper uncommittable), the gate serializes concurrent applies so the
  shutdown path can drain an in-flight grow before checkpointing, and a
  malformed construction is refused (D3).

### cli

- `cli: obdctl create-blank sends a create command with the blank object` —
  the REAL obdctl binary executed against a test-owned UDS server:
  `create-blank --size --mkfs --global --dev-id` becomes the wire line
  `{"cmd":"create","blank":{"size":...,"mkfs":...},...}` (the supervisor
  has no `create-blank` command), mode 2 omits `mkfs`, image-mode create
  still sends `config`, an `ok:false` reply exits 1, and malformed input
  (`--size -512` / `100` / missing / above the 16 TiB bound / bad
  `--mkfs` / unknown flag / junk, overflowing or below `-1` `--dev-id` in
  either create form — `std::stoi` would abort the process instead of
  exiting 2) exits 2 without connecting, while `--dev-id -1` (the
  documented auto-assign spelling) and a `--mkfs` type using `_` are
  forwarded, not refused. The test server's accept is bounded, so a CLI
  that wrongly rejects an argument fails the assertion instead of hanging
  the job (#11).

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
  accepted).
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
- `supervisor: successful commit disables recovery respawn` — with
  `max_recovery_attempts` positive, a successful commit leaves the stopped
  child exited with the original PID and zero recoveries instead of
  publishing a replacement, while still producing a valid sealed layer.
  Runs without privileges.
- `supervisor: rejected commits preserve crash recovery` — pre-stop
  commit refusals for sparse uppers, upper-less/read-only configs, and
  mode-3 host-mkfs uppers leave the fake child running, keep the
  recovery counter unchanged, and still allow a later controlled child
  crash to consume the remaining ADR-0010 recovery attempt. Runs without
  privileges.
- `supervisor: destroy is rejected while commit is admitted` — a
  test-gated admitted commit rejects a concurrent `destroy` of the same
  device with the precise busy error, then the commit is released to
  finish and a later destroy can clean up the entry. Runs without
  privileges.
- `supervisor: commit is rejected while destroy is admitted` — a
  test-gated admitted destroy rejects a concurrent `commit` of the same
  device with the precise destroy-in-progress error, then the destroy is
  released to finish and the entry disappears. Runs without privileges.
- `supervisor: concurrent commits are serialized and reject the loser` —
  two barrier-synchronized commits of the same device: exactly one
  succeeds, the loser gets a precise error ("commit already in progress"
  or "already sealed"), and the sealed file is intact (no interleaved
  tmp-file writes) (ADR-0014). Runs without privileges.
- `supervisor: blank create serves a writable zero base and commit seals its upper` — ADR-0014 mode 2 with the fake device: `create` with
  `blank` (no config) assembles the workspace (`overlaybd.zero` +
  `overlaybd.rw`) via `open_blank_device`; the fake round-trips a
  payload through the merged stack (unwritten regions read zero); commit
  stops it and seals the blank-born upper (path/sha256/size); a second
  commit is "already sealed"; the sealed file re-opens as a valid LSMT
  RO layer carrying the payload. A recording mock mkfs runner asserts
  mode 2 never invokes host mkfs (ADR-0014). Runs without privileges.
- `supervisor: mode-3 mkfs runs only when the blank spec requests it` —
  ADR-0014 mode 3 with a mock mkfs runner (host mkfs is never executed
  by the suite): a plain blank create never invokes the runner; a
  `blank.mkfs` create invokes it exactly once with the requested type
  and the reported device path and replies ok with the `mkfs` field;
  commit of a mode-3 (supervisor-formatted) upper is refused with the
  "host mkfs ... cannot be sealed" boundary error; a failing mkfs is a
  clean create error and the half-created device entry is removed. Runs
  without privileges.
- `integration: trace mock range parsing retains numeric storage` — directly
  exercises the registry fixture handler with long numeric tokens, closed and
  open-ended ranges, malformed/overflowing values, empty blobs and bounds.
  No background server tasks are active when assertions run.
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
  mask-with-zeroes read-back through that test root (ADR-0009;
  self-skips without ublk).
- `integration: ublk device survives server death via USER_RECOVERY` —
  a forked server is SIGKILLed; the block device survives QUIESCED and
  a replacement server completes the real
  START/END_USER_RECOVERY handshake and keeps serving reads
  (ADR-0010; self-skips without ublk or on kernels without the
  feature).
- `supervisor: resize grows a device and rejects shrink or no-op cleanly` —
  a real daemon with the fake obd-device (which serves the device
  command channel and executes resize grow-only without a kernel):
  unknown ids, wrong-typed `size`, and misaligned sizes are clean
  errors; a grow replies with the new size and id; a resize to the
  current size or smaller is rejected grow-only (D3; ADR-0014). Runs
  without privileges.
- `supervisor: create virtual_size headroom override is validated grow-only` —
  the same daemon/fake pair: a create override smaller than the image's
  declared size fails create with the grow-only message (the device
  validates, where the assembled size is known), an override larger
  than the image creates fine and that device stays grow-only
  (D3; ADR-0014). Runs without privileges.
- `supervisor: commit virtual_size re-baselines the sealed layer grow-only` —
  the same daemon/fake pair: a commit override below the layer's
  declared size is rejected with a precise grow-only reason, and an
  override at least the declared size seals with the override in the
  header (verified by re-opening the sealed layer) (D3; ADR-0014).
  Runs without privileges.
- `supervisor: resize of a writable device grows its data plane and persists it` —
  a real daemon with the fake obd-device: a resize grows the fake's
  writable layer through the REAL format grow path, and a subsequent
  plain commit seals the grown declared size — the data plane grew with
  the device and the growth is durable at commit (D3; ADR-0014). Runs
  without privileges.
- `supervisor: create virtual_size headroom sizes the writable upper` —
  the same daemon/fake pair: a create with `--virtual-size` assembles
  the writable upper at the override (like the real device's writable
  assembly), so a plain commit seals that declared size — the headroom
  reaches the data plane, not just the device size (D3; ADR-0014). Runs
  without privileges.

The `[daemon-lifetime]` supervisor cases hold real EOF, command-admission,
and accept boundaries with coroutine-friendly gates. They also leave idle
and partial-command client sockets open during shutdown. Assertions run only
after gates are released, fixture peers are closed, and fixture threads have
joined; the daemon must drain handlers and erased-entry monitors before
returning. These tests use unprivileged fake device processes.

The `[command-rejection]` cases use real resize RPCs and an unprivileged
fake device whose reply or exit is controlled through a fixture FIFO:

- `supervisor: rejected command keeps busy reason after pending reply` —
  pause the second handler after rejection unlock, complete the first
  request through the real monitor with its matching sequence, then check
  that the second response retains the admission-time busy reason.
- `supervisor: rejected command keeps busy reason after channel EOF` —
  close the device channel while the second handler is paused; the first
  request receives the channel-closed error and the second remains busy.

Both cases verify that handlers and monitors overlap on distinct workers
and only the first command reaches the fake. Gates are released, RPC peers
joined, the independently owned fake child terminated and reaped, daemon
tasks drained, and the signal mask restored before Catch assertions.

The `[command-ownership]` cases use the same bounded unprivileged process
and FIFO discipline. The first handler can pause after its real event wait
and before result collection while another handler and the monitor execute.
Each operation has distinct reply fields; a third command checks continued
admission and notification after the competing cleanup. The internal test
gate uses a one-second command deadline; normal daemon commands retain the
30-second deadline. Timeout cases require the daemon's timeout error, not a
fixture socket receive timeout. Routing observations prove the stale records
were processed before the correct reply is sent.

- `supervisor: old command cleanup preserves the next waiter` — A consumes its response while B awaits its own device reply; B and C complete.
- `supervisor: completed commands retain their distinct payloads` — B completes while A is parked; both retain their own sizes.
- `supervisor: earlier device error survives later success` — A retains its device error after B succeeds.
- `supervisor: earlier success survives later device error` — A retains its success after B receives a device error.
- `supervisor: terminal command reply survives later channel EOF` — EOF fails only active B while accepted A keeps its response.
- `supervisor: stale and malformed replies cannot complete a newer command` — Real routing processes old, mismatched, malformed and missing sequence replies before the valid B reply.
- `supervisor: late reply after timeout cannot complete the next command` — A really times out; its later reply cannot complete B.
- `supervisor: timed-out handler cleanup preserves a newer command` — A times out and parks; a late reply releases admission, and A cleanup cannot retire B.
- `supervisor: closed peer fails an admitted command write` — The fake exits after admission and before the actual write; the handler reports its write error.
- `supervisor: shutdown drains an active device command reply` — The real fake reply completes an active handler after shutdown begins, before daemon task drain returns.

The closed-peer case executes a real failed channel write after admission.
It does not simulate partial writes or a newer operation during old write
cleanup; that ownership is also checked in the implementation's common
identity-checked retirement path. Shutdown may cancel client response
delivery: its case observes a real command event completion during handler
drain and requires the daemon to return, without requiring a reply to reach
the canceled client. Peer threads and the independently owned child are
cleaned up before assertions, including exceptions during C's setup.
