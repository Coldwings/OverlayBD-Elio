# ADR-0020: Read TurboOCI natively and build its metadata locally

- Status: proposed
- Date: 2026-09-12
- Supersedes: ADR-0008
- Binds: docs/design-assumptions.md, docs/format.md, docs/config.md,
  docs/operations.md, docs/binaries.md, src/format/lsmt.cpp,
  src/image/config.cpp, src/image/image_file.cpp, tools/obd_convert_main.cpp

## Context

[Issue #102](https://github.com/Coldwings/OverlayBD-Elio/issues/102) requests
TurboOCI runtime support and project-owned conversion. ADR-0008 retained the
early TurboOCI exclusion. This decision removes that exclusion and preserves
ADR-0008's writable-upper contracts without change.

TurboOCI retains the original OCI layer blobs. A committed warp LSMT contains
filesystem metadata and mappings into uncompressed tar byte offsets. For gzip
layers, a separate restart index maps those offsets to compressed data and
DEFLATE dictionaries. Published metadata layers package the filesystem metadata,
a marker, and the optional gzip index in a tar.gz archive; descriptor annotations
identify the original target blob and its media type.

## Decision

Implement native TurboOCI v1 reads in the Elio source/image stack and OCI-to-
TurboOCI conversion in `obd-convert`, retaining the original tar or gzip blobs
as the file-content source.

- Accept local and registry targets through upstream `targetFile`,
  `targetDigest`, and `gzipIndex` configuration semantics. Metadata and target
  offsets occupy separate byte spaces. Runtime mapping tags must not be
  confused with the merged-layer priority tags.
- Persist remote original targets in a separate digest-keyed LayerStore under
  `<lower.dir>/targets/<sha256-hex>`, below gzip decoding. A committed target
  can reopen without registry access. The same download, admission, and
  degraded-directory policies apply as for metadata blobs. Structural warm-up
  includes both sources; the upstream trace format continues to address only
  metadata sources, keeping one trace slot per lower and no target-offset alias.
- Decode upstream warp indexes and `ddgzidx` version 1 explicitly, validating
  bounds, sizes, checksum, dictionary encoding, and restart positions before
  use. Random reads restore the DEFLATE bit position and dictionary; concurrent
  reads must not share mutable inflater state. Source reads remain asynchronous.
- Import and produce the upstream metadata package and target annotations.
  Validate required entries, target identity, media type, and archive paths;
  report unsupported formats and malformed input instead of emitting a
  runnable-looking partial configuration.
- Use libe2fs by default for device-free ext-family filesystem construction.
  Build metadata and remote mappings locally, without invoking upstream
  conversion binaries or bringing Photon into the runtime. Preserve original
  target blobs and emit their digest/size identities with generated metadata.
- Apply OCI differential layers in parent order, including whiteouts, opaque
  directories, hardlinks, symlinks, attributes, sparse content, and device-node
  metadata. Unsupported encodings must fail explicitly. A differential archive
  must never be treated silently as a flattened rootfs.
- Preserve explicit entry modification times at nanosecond precision using
  256-byte ext-family inodes and the extra-inode timestamp fields. Determinism
  fixes synthesized metadata times, not timestamps already specified by input.
  Explicit directory replacements replace the complete attribute set.
- Generated metadata, gzip indexes, and package bytes are deterministic for
  identical inputs and options. Registry publishing remains the external CLI
  boundary established by ADR-0014.
- EROFS support and TurboOCI-to-native materialization are excluded by the
  user's explicit scope decision. Ordinary conversion can use the original OCI
  input; a reverse conversion entry point is unnecessary.

## Consequences

Runtime compatibility and local production require separate coverage. Golden
upstream layouts and upstream-generated fixtures must supplement local round
trips; locally produced artifacts must also be readable upstream. Tests cover
mixed ordinary/TurboOCI layers, metadata/payload transitions, gzip restart
boundaries, parent-layer filesystem semantics, and malformed package/index data.

Filesystem-builder dependencies remain confined to conversion targets. The
existing ordinary converter interface and writable upper contracts remain valid.
This ADR and the implementation are reviewed together; a partial implementation
does not fulfill #102.

## Alternatives considered

- Offline-only import would omit the requested direct lazy runtime reads.
- An upstream CLI wrapper would omit project-owned conversion and introduce a
  second runtime framework into the deployment workflow.
- Whole-blob gzip decompression at device startup would defeat lazy access.
- Reverse materialization duplicates ordinary conversion from the retained OCI
  inputs and is not required by the user.
- An EROFS builder adds a separate filesystem dependency and is not requested.
