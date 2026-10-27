# Hybrid reference fixtures

These ordinary-file fixtures were produced by the actual unmodified upstream
implementations pinned in `manifest.json`. They are inputs for issue #146, not
output from OverlayBD-Elio's writer. Do not regenerate expected output with the
implementation under test. The manifest records every fixture's size and hash.

Each producer created an independent writable pair, a pair with 4096-byte group
commit buffering, and a sealed data file. Both backing files were synced before
return; sealing additionally synced the data file. `sealed.data` is read-only;
the obsolete index sidecar is intentionally not part of its fixture.

The virtual device has 64 sectors of 512 bytes. Operations, in order:

1. Write sectors 1–2 with byte 0x11.
2. Write sectors 8–10 with byte 0x22.
3. Rewrite sector 2 with byte 0x33.
4. Discard sector 9, then write it with byte 0x44.
5. Discard sector 10.
6. Discard sectors 16–17, which were previously holes.
7. Write sector 63 with byte 0x77.

`expected.raw` is the 32768-byte top-only read oracle. `expected.states` has one
byte per sector: 0 INHERIT, 1 DATA, 2 ZERO. Both oracles are constructed from the
operation sequence independently of each library's index. Producer helpers in
`producers/` verify both logical bytes and mapping states through the actual
reference APIs before sealing. Readers verify those same oracles on reopen.

## Known reference differences

All three OverlayBD fixtures pass both reference readers. All three AgentENV
fixtures pass AgentENV's reader, but the pinned OverlayBD reader rejects the
all-ones physical-offset sentinel on ZERO records. These are intentionally
unmodified inputs; do not rewrite that sentinel to make an import test pass.

OverlayBD's writable ZERO records use an EOF position marker. In this fixture,
ZERO sector 10 and ZERO sectors 16–17 have physical offset 14, which also holds
live DATA sector 63. This is valid for its own writer, which appends when writing
ZERO. The pinned AgentENV writer instead treats non-sentinel ZERO offsets as
retained backing. Opening a copy of this pair in AgentENV and writing sector 10
with 0x55 changes sector 63 from 0x77 to 0x55. A read-only interchange check does
not establish safe writable interchange. Import tests must cover the subsequent
write and verify every unrelated logical sector remains unchanged.

AgentENV's own pair retains exclusive backing for discarded DATA, and uses its
sentinel for discarded holes. The identical rewrite preserves unrelated data.

An independent candidate encoding experiment assigned exclusive backing to each
ZERO range. Both actual readers accepted its writable/sealed artifacts and the
AgentENV rewrite preserved other data. That experiment is not an upstream golden
and is deliberately not substituted for these inputs. Production import/export,
durability and bounded-growth behavior still require implementation and tests.

## Reproduction

Use the exact repository revisions and Photon revision in the manifest. Compile
`producers/overlaybd.cpp` against the actual upstream `lsmt_lib` and
`photon_static` targets (C++17); initialize Photon with epoll and use ordinary
local files. Run `produce NEW_DIRECTORY`, `check-rw DATA INDEX`, or
`check-ro SEALED_DATA`. The directory must not already exist.

For AgentENV, create an external Cargo package named `agentenv-hybrid-fixture`,
version 0.1.0, edition 2021, with its own empty `[workspace]`. Copy
`producers/agentenv.rs` to `src/main.rs` and the supplied lockfile to Cargo.lock.
Dependencies: `overlaybd` as a path to the pinned `storage/overlaybd` crate with
`default-features = false`; `anyhow = "1"`; `uuid = "1"`; and `tokio = "1"` with
features `rt` and `macros`. Use Rust 1.99.1 and `cargo build --locked --jobs 2`.
The executable accepts the same produce/check modes as the C++ helper. No
AgentENV server, block device, elevated capability or network service is needed
for fixture generation or verification. Dependency retrieval is a build step.

Always run mutation tests on copies; retained golden hashes must remain stable.
