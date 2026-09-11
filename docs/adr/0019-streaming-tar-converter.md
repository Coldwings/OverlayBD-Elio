# ADR-0019: Build tar streams into deterministic device-free LSMT layers

- Status: accepted
- Date: 2026-09-12
- Supersedes: none
- Updates: ADR-0014 (converter backend scope)
- Binds: docs/design-assumptions.md, docs/binaries.md, docs/operations.md,
  docs/format.md, tools/obd_convert_main.cpp

## Context

ADR-0014 established the build-side requirement: converting the same rootfs tar
layer twice must produce the same layer bytes, and image builds must not depend
on a host block device, mount, or `mkfs` subprocess. It named a pinned
libe2fs/e2fsprogs fork as the upstream-proven way to build ext filesystems as a
pure library. This ADR updates that converter-backend detail for the first
repository-owned converter: the initial accepted scope is the dependency-free
built-in backend that can run in the default build and CI. Because the built-in
backend is always present, the initial converter has no separate
backend-enabled CI dimension; a pinned libe2fs backend remains a future
converter-local extension with its own build flag and CI matrix if added.

The project already has byte-compatible LSMT writers and deterministic seal
machinery. The missing piece is a rootfs-tar reader plus filesystem-image
builder that feeds those writers without entering the runtime data plane.

## Decision

Add `obd-convert` as the repository-owned converter binary. It reads a rootfs
ustar archive with the standard two-zero-block end marker from a file or stdin,
spools file payloads to disk while keeping only metadata in memory, builds a
deterministic ext2-compatible filesystem image with the built-in backend, seals
that raw image as a single LSMT-RO layer, and prints manifest metadata
(`digest`, `size`, `file`, and converter fields) on stdout.

The built-in backend is the default no-extra-dependency backend for the first
accepted converter implementation. It must not invoke host `mkfs`, open a block
device, or mount anything. All filesystem fields that would otherwise vary by
host or time are pinned: timestamps are zero, filesystem identity is
deterministic/static, directory order is stable, and the LSMT UUID is derived
from the raw filesystem digest rather than randomness. The supported filesystem
feature set is deliberately small: regular files up to 4,243,456 bytes (12
direct data blocks plus one single-indirect block), directories up to 12 data
blocks, and short inline symlinks; 4 KiB ext2 blocks; uid and gid values up to
65535; at most 32768 inodes; and images up to 128 MiB or an explicit aligned
`--size` budget.

A future pinned libe2fs backend may be added behind the same binary when it is
needed for broader ext4 features, but it remains converter-local. The data
plane, supervisor, image assembly, and normal source stack must not gain that
dependency. Any libe2fs backend must preserve the same deterministic contract
and keep the built-in backend available for default CI.

## Consequences

- Operators and tests can build a sealed local layer directly from a rootfs tar
  without a privileged device, mount namespace, or host formatting tool.
- `obd-mkimage` remains the raw-image fixture generator; `obd-convert` owns
  rootfs-tar image authoring.
- Converter-produced layers are ordinary sealed LSMT lowers and can be pasted
  into `config.json` just like committed layers.
- Output assembly happens in a private temporary workspace below `--out-dir`;
  only completed files are atomically renamed to the documented output paths.
- The first implementation is intentionally bounded. Tar entries outside the
  documented subset, and malformed archive framing, fail clearly before an LSMT
  layer is published.
- Broader ext4 feature coverage, PAX/xattrs, devices, hardlinks, sparse tar
  files, and OCI push/manifest assembly remain outside this binary's current
  contract.

## Alternatives considered

- **Call host `mkfs.ext4` or `genext2fs`.** Rejected for the same reason as
  ADR-0014: host tools introduce random UUIDs, timestamps, hash seeds, layout
  differences, availability problems, and often a privilege/device boundary.
- **Hard-require a pinned libe2fs backend before shipping any converter.**
  Rejected: it blocks the default build on a non-runtime dependency even though
  the repository can satisfy the deterministic no-device path for a useful
  bounded subset with a small built-in backend.
- **Extend `obd-mkimage` instead of adding a binary.** Rejected: `obd-mkimage`
  is a raw-disk fixture tool, while rootfs-tar conversion has a different input
  contract, failure surface, and documentation boundary.
- **Buffer the full tar archive in memory.** Rejected: the converter must keep
  memory bounded. Payload bytes are spooled to disk and then copied into the
  filesystem image.
- **Include OCI upload and manifest assembly.** Rejected: this repository owns
  local format construction. Registry UX and artifact publishing stay in the
  external CLI boundary described by ADR-0014.
