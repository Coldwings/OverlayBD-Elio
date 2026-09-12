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
e2fsprogs/libe2fs fork as the upstream-proven way to build ext filesystems as a
pure library. The repository first accepted a small dependency-free built-in
backend to unblock deterministic local conversion; the converter now has both
backends. Normal builds enable the pinned libe2fs backend and use it by default,
while the built-in backend remains available for dependency-free builds and
explicit fallback runs.

The project already has byte-compatible LSMT writers and deterministic seal
machinery. The remaining converter boundary is a rootfs-tar reader plus
filesystem-image builders that feed those writers without entering the runtime
data plane.

## Decision

Add `obd-convert` as the repository-owned converter binary. It reads a rootfs
ustar archive with the standard two-zero-block end marker from a file or stdin,
spools file payloads to disk while keeping only metadata in memory, builds a
deterministic ext2-compatible filesystem image, seals that raw image as a single
LSMT-RO layer, and prints manifest metadata (`digest`, `size`, `file`, and
converter fields) on stdout.

The default backend is `libe2fs` when `OBD_ENABLE_LIBE2FS_BACKEND=ON`. That
backend builds and links against the pinned `data-accelerator/e2fsprogs`
libext2fs source used by upstream OverlayBD's standalone libext2fs build at
commit `404deb95e6b0ed0ceb0148d289977e65bee7f8d0`. The
installed binary prefers the bundled `libext2fs.so*` relative to itself via
`$ORIGIN/../lib/overlaybd-elio`; `libcom_err.so.2` remains a normal system
runtime library. An operator may intentionally rely on a system-compatible
libe2fs/libext2fs instead of the bundled library. That fallback is treated as a
performance and dependency-control tradeoff, especially for many-file images,
not as a known output-correctness incompatibility.

The built-in backend remains the no-extra-dependency backend. It must not invoke
host `mkfs`, open a block device, or mount anything. All filesystem fields that
would otherwise vary by host or time are pinned: timestamps are zero,
filesystem identity is deterministic/static, directory order is stable, and the
LSMT UUID is derived from the raw filesystem digest rather than randomness. The
built-in feature set is deliberately small: regular files up to 4,243,456 bytes
(12 direct data blocks plus one single-indirect block), directories up to 12
data blocks, and short inline symlinks; 4 KiB ext2 blocks; uid and gid values up
to 65535; at most 32768 inodes; and images up to 128 MiB or an explicit aligned
`--size` budget.

Both backends remain converter-local. The data plane, supervisor, image
assembly, and normal source stack must not gain the libe2fs dependency. CI must
cover the default libe2fs build and the `OBD_ENABLE_LIBE2FS_BACKEND=OFF`
dependency-free build so the explicit built-in backend remains usable.

## Consequences

- Operators and tests can build a sealed local layer directly from a rootfs tar
  without a privileged device, mount namespace, or host formatting tool.
- `obd-mkimage` remains the raw-image fixture generator; `obd-convert` owns
  rootfs-tar image authoring.
- Converter-produced layers are ordinary sealed LSMT lowers and can be pasted
  into `config.json` just like committed layers.
- Output assembly happens in a private temporary workspace below `--out-dir`;
  only completed files are atomically renamed to the documented output paths.
- The built-in backend is intentionally bounded, and the libe2fs backend only
  expands the filesystem writer limits for the current regular-file,
  directory, and symlink tar-entry classes. Until the converter creates
  htree-indexed ext2 directories, the libe2fs path keeps explicit guardrails of
  65536 in-memory tar nodes and 1024 data blocks in any one directory. Tar
  entries outside the documented subset, and malformed archive framing, fail
  clearly before an LSMT layer is published.
- Broader tar feature coverage, PAX/xattrs, devices, hardlinks, sparse tar
  files, and OCI push/manifest assembly remain outside this binary's current
  contract.

## Alternatives considered

- **Call host `mkfs.ext4` or `genext2fs`.** Rejected for the same reason as
  ADR-0014: host tools introduce random UUIDs, timestamps, hash seeds, layout
  differences, availability problems, and often a privilege/device boundary.
- **Remove the built-in backend after adding libe2fs.** Rejected: the built-in
  writer is still valuable for dependency-free builds, minimal CI coverage and
  isolating libe2fs packaging problems from the converter's tar and LSMT logic.
- **Extend `obd-mkimage` instead of adding a binary.** Rejected: `obd-mkimage`
  is a raw-disk fixture tool, while rootfs-tar conversion has a different input
  contract, failure surface, and documentation boundary.
- **Buffer the full tar archive in memory.** Rejected: the converter must keep
  memory bounded. Payload bytes are spooled to disk and then copied into the
  filesystem image.
- **Include OCI upload and manifest assembly.** Rejected: this repository owns
  local format construction. Registry UX and artifact publishing stay in the
  external CLI boundary described by ADR-0014.
