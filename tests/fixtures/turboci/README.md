# Upstream TurboOCI v1 fixture

Generated with the unmodified x86-64 Ubuntu 22.04 binaries from the
[OverlayBD v1.0.18 release](https://github.com/containerd/overlaybd/releases/tag/v1.0.18),
asset `overlaybd-1.0.18-20260710.cee2186.ubuntu1.22.04.x86_64.deb`.
The release binaries were unpacked without installation. A private user/mount
namespace supplied their default service configuration; format code was not patched.

`original.tar.gz` contains one USTAR file named `payload`, mode 0644, size
3 * 1024 * 1024 + 123. Payload byte i is `(i * 17 + i / 251) % 256`
(integer division). Python tarfile USTAR and gzip mtime=0 produced the input.

Upstream generation steps:

```sh
overlaybd-create --turboOCI --mkfs upstream.data upstream.index 1
turboOCI-apply --gz_index_path gzip.meta original.tar.gz build.json
overlaybd-commit --turboOCI -z upstream.data upstream.index ext4.fs.meta
```

`build.json` has empty `lowers` and an `upper` whose `data`, `index`, `target`,
and `gzipIndex` point to those four corresponding files. The service disables
background downloads, gzip cache, metrics, and audit logging. Creation capacity
is 1 GiB; only compact metadata is retained here. Upstream UUID/time generation
can vary when regenerating; the committed hashes below identify this fixture.

The runtime test opens upstream ZFile-wrapped metadata and upstream gzip restart
indexes together, then verifies every target-mapped extent against the independently
defined payload pattern. This is additional to local writer roundtrip tests.

| File | SHA-256 |
|---|---|
| `ext4.fs.meta` | `5569a9fc498fae8c37d9a34533ab08aea3029c996afc159d7b053e9f4328aa5e` |
| `gzip.meta` | `9a491425d2c28f141655cf57baba9fdf7b23cbfb4400e527e15ece3219863036` |
| `original.tar.gz` | `60e1d9dc20a6e2b4a0241a60b7cfb3ffe17de0d7b08f2ba3756a86eed928a35b` |
