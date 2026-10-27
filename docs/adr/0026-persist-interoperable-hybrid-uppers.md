# ADR-0026: Persist interoperable Hybrid uppers without payload-copy sealing

- Status: accepted
- Date: 2026-10-27
- Supersedes: none
- Binds: src/format/hybrid_format.hpp, src/format/hybrid_format.cpp,
  src/format/lsmt.cpp, src/format/writable.hpp,
  src/format/merged_writable.cpp, src/image/config.cpp,
  src/image/image_file.cpp, src/supervisor/daemon.cpp,
  docs/config.md, docs/format.md, docs/image.md, docs/operations.md,
  docs/supervisor.md, docs/testing.md

## Context

Issue [#146](https://github.com/Coldwings/OverlayBD-Elio/issues/146) requires a
restart-recoverable writable upper that seals into a compatible LSMT layer
without copying its live payload. Existing `lsmt` already reuses live upper
mappings, but persists its index only at terminal checkpoint and copies live
payload when sealing. Existing `sparse` has exact persistent ownership and
exports a separate sealed LSMT delta while retaining its reusable source
(ADR-0027). Neither mode changes its default or contract under this decision.

Actual fixtures from pinned current OverlayBD and AgentENV writers show a
compatibility difference that source-level flag matching cannot resolve.
Both use bit 5 for Hybrid and the standard 16-byte mapping record. OverlayBD
stores a data-EOF position in ZERO records; that position can later also hold
live DATA. AgentENV treats a non-sentinel ZERO position as reusable backing and
uses the all-ones 55-bit physical-offset sentinel for unbacked ZERO. OverlayBD's
reader rejects that sentinel, while an AgentENV rewrite through an imported
OverlayBD ZERO marker can overwrite unrelated live DATA. The reference fixtures
and their independent state/byte oracles are retained with producer provenance.

## Decision

Provide opt-in `upper.type = "hybrid"` using a paired upstream-compatible data
file and mapping log, validate exclusive physical ownership before reuse, and
seal by appending the compact live index and trailer to the existing payload.

This proposed record describes the intended implementation contract. It is not
an assertion that the implementation or acceptance gates are complete.

### Import, ownership and export

Match data/index UUID, parent identity and virtual size before replay. Require
supported writable flags and versions, aligned file lengths, and bounded log
and live-index sizes. Stream records in bounded chunks and apply them in append
order; later logical ranges replace earlier ones. Runtime tags do not transfer
ownership of lower layers into the writable upper.

Only live current-upper DATA may be overwritten in place. Imported ZERO is
logically zero regardless of physical offset. Preserve reusable ZERO backing
only if the complete range lies inside the data file and is disjoint from every
other live physical range. Normalize other valid ZERO markers to unbacked ZERO
in memory. Reject aliases between live DATA extents, invalid logical ranges,
invalid DATA physical ranges and invalid artifact identities.

Outgoing writable ZERO records use exclusive backing, including ranges first
covered by discard. Reserve backing for such holes before publishing the ZERO
mapping. Repeated discard/rewrite reuses those allocations; discard does not
promise physical reclamation. This bounds ordinary repeated-rewrite payload
growth by allocated upper coverage, rather than write count. Import repair,
interrupted unpublished allocations and extension are accounted for separately.
Sealed ZERO records remain in-range markers accepted by both reference readers;
sealing never copies live payload solely to change ZERO encoding.

New owned pairs live in a dedicated directory with `data` and `index` files.
Image assembly first creates and syncs the requested workspace and its actual
directory ancestors through the namespace root on the blocking pool. Retry
syncs existing names too, since an earlier failed attempt may have created
them without completing durability. Directory open/sync errors abort assembly
before pair publication. Standalone pair-creation callers must supply an
already durable parent namespace, stable throughout setup.
Prepare both artifacts in a same-parent staging directory, sync the artifacts
and staging directory, publish with a no-replace directory rename, then sync
the parent. Existing destinations are never replaced by initialization. This
avoids exposing half of a new pair as a usable writable upper. A crash can
leave an unpublished staging directory; its presence is not authority to adopt
or remove another pair. Recovery must distinguish that state from the fully
published directory.

### Ordering, durability and failures

Serialize each direct upper operation with payload I/O and mapping publication.
Merged reads must also serialize the complete index-snapshot-plus-payload-read
operation with upper mutation and merged-index replacement. A raw file-view
lock alone cannot repair an already stale merged snapshot. Lower files are
immutable throughout these operations.

For new mappings, complete ordinary payload writes before appending the mapping
record; do not synchronize the data file for every mapping transition. A returned request must not publish a
mapping whose required payload write failed. A successful guest FLUSH covers
preceding payload changes, all pending mapping records, and required namespace
publication, in that order. In-place rewrites do not add mapping records when
logical ownership and physical location stay unchanged.

Completed writes can still tear after host crash or power loss before their
successful FLUSH: the only upper copy is overwritten in place. No copy-on-write
atomicity is promised. Errors and interrupted multi-fragment operations may
expose a completed prefix and must preserve explicit ZERO masks and lower
isolation. Process-exit recovery, partial-operation recovery and power-loss
recovery are distinct test cases.

Completed writes and discards, including their mapping transitions, survive a
process crash without FLUSH when the OS and filesystem continue running. Each
operation completes payload I/O, complete index-record writes and progress
publication before returning. This is not a guarantee that those bytes have
reached persistent storage. A process interrupted during an operation may leave
an absent or partial operation; in-place writes may leave mixed bytes.

After a host crash or power loss, operations not covered by successful FLUSH may
be absent, partial, or recovered with incomplete payload. A valid mapping CRC
checks index integrity, not payload durability. Recovery does not promise an
exact last-FLUSH image because subsequent in-place writes may already have
modified its physical blocks. Lower files remain immutable; a flushed ZERO mask
remains protective unless a later operation modifies that range. An unflushed
ZERO-to-DATA rewrite can lose that zero guarantee after host failure.


Reject partial mapping records and invalid complete records before using them.
The upstream log has no per-record checksum: arbitrary corruption or a torn
record that happens to encode another structurally valid mapping cannot be
proven absent from structural checks. Document this wire-format limitation;
do not describe alignment validation as a checksum or blanket tear detection.
Any additional publication metadata must remain outside the exchanged upstream
artifact format and must never make stale sidecars silently authoritative.

### Log growth and terminal publication

Bound replay independently of payload reuse: at most four million physical log
records and one million live mappings. Before a modifying fragment, compact
the mapping log at `max(4096, 2 * live_mappings + 16)` records, independently
of payload reuse and before the hard replay ceiling. Persist
replacement contents, atomically publish the replacement index, then sync its
parent directory. A failed publication must leave the active descriptor and
recovery-visible generation consistent. Compaction must not depend on sealing.

Checkpoint is a terminal durability operation for this process, while the
writable pair remains reopenable. Seal appends the compact index and sealed
trailer without payload-copy compaction. Grow and seal publish a local
`<index>.txn` intent before changing the pair. Its fixed 16 KiB version-1 record
binds both device/inode identities, original lengths and headers, the desired
header/trailer, and index-record CRC32C, with a checksum over the complete
record. Sync the temporary intent, rename without replacement, and sync its
directory before any header or appended metadata changes. Grow writes and syncs
both headers in order; seal appends index/padding/trailer and syncs data. Retire
the intent only after the operation is durable, then sync the directory.

Exclusive reopen validates and rolls forward a pending intent before parsing
the normal pair. Reject damaged records, changed identities and mismatched
index contents without modifying the pair. A pending intent is local to the
original inodes, so recover before copying an interrupted pair. Retry must
recognize already published output; it must not append a second ambiguous
trailer or interpret an unfinished seal as payload ownership. Test both returned
I/O errors and independent process exits at each publication boundary.

The intent only protects pending grow/seal operations. Use local
`user.overlaybd.hybrid.log` attributes on both files for ordinary-log integrity.
Index progress stores the generation and checksum/length of complete appended
records before their operations return. The data anchor stores the FLUSH prefix
only after payload, index metadata and directory sync. Reopen must validate that
committed prefix; it can accept a newer verified progress prefix or recover an
unconfirmed tail to a verified earlier prefix, but cannot guess missing committed
records. Apply any truncation only after bounded semantic replay succeeds.

A new compact index has a fresh generation. Sync both logs, publish a two-generation
data anchor, then rename, sync the directory and commit the new generation. Keep
runtime ownership consistent even on post-rename exceptions. Imported upstream
pairs without local attributes receive an installing anchor before index progress,
so interrupted adoption is recoverable. Managed writes require user-xattr support;
read-only unmanaged imports do not. Exchange with upstream writers uses a stopped,
ordinary-byte copy without these local attributes, since upstream does not update
them. Preserving managed backup protection requires preserving both attributes;
removing both is indistinguishable from a new unmanaged import. No upstream header,
flag, mapping or reader protocol is changed by this local protection.

The existing commit path stops and reaps the device before offline sealing.
Hybrid uses that lifecycle and does not introduce a new live restack API.
Configuration, blank-device selection, grow, status and precise error reporting
must carry the selected upper type through the existing control paths.

### Seal identity

Hybrid sealed bytes retain physical allocation and write history. Their digest
identifies the actual artifact, and equal live logical bytes need not produce
equal sealed bytes or UUIDs. Existing `lsmt` deterministic compacting seal and
empty-layer generation keep their current guarantees. Hybrid is opt-in and
must not change the default upper type through this decision.

## Consequences

Interoperable ownership is more restrictive than accepting every numeric ZERO
marker as reusable storage. First discard into a hole reserves space; it does
not create a free reusable offset shared with unrelated data. Tests must verify
both subsequent writes and reads against the pinned reference implementations.

Additional metadata ordering has a measurable latency cost. Compare current
`lsmt`, `sparse` and Hybrid first-write/rewrite workloads at QD1/QD16, log growth
and reopen time, and seal latency/peak temporary storage. The selected local
validation scope is a basic 32 MiB comparison at QD1/QD16, with one trial per
cell, explicit build provenance, and full sealed-output readback. Further large
matrix runs were withdrawn to limit load on the development node. Retain older
large-run evidence as historical measurements, not current implementation
results. Small performance samples establish neither a statistical ranking nor
large-image scaling and do not replace the deterministic fault matrix.

Before acceptance, finish all integrations and fault/performance validation, update current module
contracts, and pass deterministic fault/concurrency tests, bidirectional fixture
checks, repository gates and independent final-head reviews.

## Blank device integration

The additive `blank.upper_type` field selects `lsmt` (default) or `hybrid`.
`hello.features` advertises `hybrid_upper`. Before forwarding an explicit
Hybrid selection, `obdctl create-blank --upper-type hybrid` requires this
capability; missing or malformed support fails before creation so an older
supervisor cannot silently substitute LSMT. Default and explicit LSMT creation
retain their existing compatibility. Child launch uses
`--blank-upper-type`. Hybrid keeps its pair under the per-device workspace,
records both paths for commit, and uses the existing `--recover` intent to
require that pair without creation. Recovery keeps persisted grown size even
when the original create size is smaller. The empty lower is regenerated at
that recovered capacity. Host mkfs restrictions remain unchanged.

### Upper provenance in status and list

`status` and each `list.devices` entry include an additive `upper` object for
known writable uppers: `type`, `path`, and, for Hybrid, `index_path`. These are
the creation-time paths used by commit, copied with the observed child under
the registry lock. Editing an image config afterwards does not redirect them.
Read-only or unknown uppers omit the object. This reports configured artifact
provenance, not a fresh filesystem existence or integrity check; failures remain
in the existing `error` field and operation replies.

### Trailer-based seal state

A complete sealed trailer and its self-contained compact index determine sealed
state for both native and imported files. Validate header identity and trailer/index geometry, logical
ordering and payload bounds, using authoritative trailer fields as the sealed reader does; accept OverlayBD's 512-byte compact-index padding
and AgentENV's 4096-byte trailer alignment, as well as extra padding accepted
by the sealed reader. A valid sealed artifact is refused
by writable-pair open even when local log attributes remain present or the
sidecar describes older writable history. Attributes cannot override that state.

Do not create or require a terminal-seal xattr. Seal appends the index/trailer,
synchronizes data, then retires the recoverable intent and synchronizes its
directory. Existing local terminal attributes are ignored for classification.
Local log attributes still protect writable recovery progress; they do not make
a missing or incomplete trailer into a sealed lower. Read-only inspection of a
writable pair is separate from loading a sealed lower, which requires complete
trailer/index validation.

An arbitrary payload deliberately constructed to be a complete sealed artifact
is classified by that structure, regardless of whether the caller invoked
seal(). Merely matching trailer magic or flags is insufficient. No verified
external xattr convention currently authorizes removing a trailer to resume
writing, so this implementation never performs that conversion implicitly.
