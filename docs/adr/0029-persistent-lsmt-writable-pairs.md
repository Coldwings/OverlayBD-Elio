# ADR-0029: Resume LSMT writable data/index pairs after process exit

- Status: accepted
- Date: 2026-10-27
- Supersedes: ADR-0020
- Binds: src/format/lsmt_rw.hpp, src/format/lsmt_rw.cpp,
  src/format/lsmt_rw_index.hpp, src/format/lsmt_rw_index.cpp,
  src/format/lsmt_rw_file.hpp, src/format/lsmt_rw_file.cpp,
  src/image/image_file.cpp, src/cmd/obd_device_main.cpp,
  src/supervisor/daemon.cpp, docs/format.md, docs/operations.md

## Context

Issue [#164](https://github.com/Coldwings/OverlayBD-Elio/issues/164) closes a
local capability gap. Upstream LSMT already supports reopening unsealed
data/index pairs. The local implementation retained mappings only in memory
until a terminal single-file checkpoint, so recreating the upper truncated it
and FLUSH alone could not recover its ownership. This was a deferred local
feature, not an intrinsic limitation of LSMT.

The user selected upstream-compatible paired persistence on 2026-10-27.
This record replaces only the memory-only LSMT writable contract inherited by
ADR-0020 from ADR-0008. Native TurboOCI, in-place DATA overwrite, immutable
lowers, explicit ZERO masks, process isolation, and deterministic offline
seal/commit remain in force. Graceful checkpoint still terminates the live
instance; opening the resulting unsealed pair creates a new writable instance.

## Decision

Store ordinary append-type LSMT data and index artifacts with upstream header
flags 3 and 1. The index contains a 4096-byte header followed by standard
16-byte mapping records. Do not switch the layer to Hybrid, change the default
upper type, or introduce a single-file private mapping encoding. Interoperable
format does not imply identical allocation policy: retain the local in-place
DATA overwrite policy and append newly owned or previously zeroed ranges.

Replay records in publication order. Later records replace earlier logical
ownership; absent records inherit lower contents, and ZERO records mask them.
Validate pair identity, capacity, field versions, bounds and resource limits
before modifying either artifact. Ignore upstream invalid-offset padding and
runtime tags. Require exclusive physical ownership of the final live DATA
extents before enabling in-place mutation. ZERO placeholders own no payload.

Complete new payload writes before publishing their mappings, and complete
mapping publication before reporting operation success. Ordinary writes and
discard do not require a data synchronization barrier for every record.
Completed operations survive device-process exit while the OS/filesystem
continues running. An interrupted operation may be absent or partially applied;
an in-place interrupted overwrite may contain mixed bytes. This is not a
copy-on-write snapshot or an exact rollback to the last FLUSH image.

An incomplete EOF record may be removed only after all preceding full records
have passed replay validation. Invalid full records, incompatible headers and
missing required artifacts fail without resetting existing data. A failed
append makes the current writer unusable until recovery; it must not overwrite
an uncertain suffix and then acknowledge later operations.

Explicit FLUSH synchronizes payload, index and necessary directory metadata,
in that order, and propagates failures. Host power loss before FLUSH has weaker
guarantees than process exit; do not add per-operation storage barriers to
simulate a stronger contract. Bound index growth through compact replacement:
sync payload, write and sync the replacement index, rename it, adopt the
published descriptor even if a subsequent directory sync fails, and permit
retry. A stable ownership lock survives pathname replacement; also lock data
and index inodes to exclude cooperating aliases. External programs must not
modify an owned pair concurrently.

Grow publishes a checksummed temporary intent containing the old capacity and
both complete target headers before rewriting either live header. Recovery
validates the intent, artifact identities and full mapping prefix, then rolls
forward to the target capacity, synchronizes both headers and retires the
intent. A partial capacity-field write is recoverable; unrelated header changes
are not overwritten. Until recovery succeeds, a failed grow prevents ordinary
mutations. A retry that finishes recovery but rejects a smaller requested size
still adopts the recovered capacity in memory. This transient transaction file
does not alter the upstream data/index encoding.

Complete valid sealed metadata takes precedence over local state or a stale
writable sidecar. Refuse writable reopening of a sealed artifact. Missing or
incomplete sealed metadata does not make an artifact a valid read-only lower.

Separate creation from opening existing artifacts. Recovery never silently
creates a replacement for missing data or truncates an existing upper. Migrate
only fully validated legacy checkpoints, publishing a complete paired index
before retiring any old metadata. Retaining old appended checkpoint bytes as
unreferenced payload is allowed until deterministic seal removes them. An old
memory-only upper without a checkpoint has no recoverable ownership and must
receive an explicit diagnostic rather than fabricated empty mappings.

Creation stages and synchronizes two complete empty headers, then publishes a
checksummed creation intent in their common directory. Recovery validates both
candidates against that intent before linking either final name without
replacement. Synchronize the published names before retiring the intent and
staging links. Unpublished private staging artifacts left by process exit are
not authoritative and are never adopted by unrelated retries. A data file of
only 4096 bytes is not proof of empty ownership: a completed discard can have
published ZERO mappings without appending payload. Missing indexes without an
applicable creation intent or valid legacy checkpoint must fail explicitly.

## Alternatives considered

- A private single-file journal preserves the old artifact count but adds a
  new persisted schema, migration protocol and interoperability boundary.
  The user selected the existing upstream pair format instead.
- Clearing the old checkpoint's terminal flag is insufficient: its append
  cursor points into metadata, and later discard still lacks persistence.
- Replacing LSMT with Hybrid does not fulfill LSMT writable recovery and changes
  the selected format and seal behavior.
- Appending all DATA rewrites grows payload indefinitely under overwrites and
  reverses the existing in-place allocation decision.

## Consequences

An unsealed LSMT upper now requires its index sidecar for recovery. Copying
payload alone cannot preserve writable ownership. Existing complete legacy
checkpoints need an explicit, restartable migration path. The acceptance gate
includes real upstream fixtures/readers, independently exiting child processes
without FLUSH, publication faults, repeated reopen/grow/checkpoint cycles,
nonzero lower layers, image/blank-device/supervisor recovery and deterministic
seal identity. Only small local performance comparisons are required.

This contract lands with the carrying implementation PR; its merge requires
the integration and recovery acceptance tests above.
