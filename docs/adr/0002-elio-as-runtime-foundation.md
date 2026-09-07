# ADR-0002: Elio is the runtime foundation

- Status: accepted
- Date: 2026-09-07
- Supersedes: none
- Binds: CMakeLists.txt, AGENTS.md

## Context

The data plane of this project is massively concurrent small IO: registry
range reads, ZFile decompression, ublk request dispatch, all latency-bound
and none compute-bound. That profile punishes thread-per-connection designs
(stack memory per in-flight IO, context-switch storms) and punishes
callback-chained designs (control flow shredded across handlers, error paths
that are impossible to audit). C++20 stackless coroutines give
sequential-looking asynchronous code with per-IO state in a heap frame,
which matches the workload exactly.

Elio is a header-only C++20 coroutine runtime providing io_uring and epoll
backends, an HTTP client and server, and coroutine-aware synchronization
primitives. It is maintained by the same maintainer as this project, so
runtime features can be shaped by real data-plane needs instead of
worked around. Upstream OverlayBD is built on callbacks over the Phototon
framework; its architecture therefore cannot be adopted wholesale, even
where its formats must be matched byte-for-byte.

## Decision

Use Elio as the sole asynchronous runtime: all IO on coroutine paths goes
through Elio primitives, and the Elio dependency is pinned by commit in the
top-level `CMakeLists.txt`.

## Consequences

- There is exactly one executor in process. No competing event loops, no
  thread-pool-vs-coroutine splits, no "which runtime owns this socket"
  questions at module boundaries.
- Dependency updates are deliberate events: the pin moves only in a dedicated
  bump commit with a stated reason, so a runtime change never smuggles itself
  into a feature PR.
- Upstream OverlayBD code informs on-disk and wire formats, not architecture:
  we read it to match byte layouts, and re-express behavior as coroutines.
- The same-maintainer relationship means runtime gaps are fixed upstream in
  Elio rather than patched locally; conversely, this project inherits Elio's
  release cadence and must absorb its breaking changes at pin-bump time.

## Alternatives considered

- **Boost.Asio.** Mature and ubiquitous, but coroutine support is layered
  over a callback-first core, and the header weight inflates every
  translation unit in the project. Rejected.
- **folly.** Powerful, but brings Meta-scale build weight and a dependency
  tree tuned to Meta's workloads and toolchain, far past this project's
  needs. Rejected.
- **libuv.** A C callback model; using it from C++20 coroutines means
  writing and owning the entire coroutine bridge ourselves, for no gain over
  a runtime that is coroutine-native. Rejected.
- **Phototon.** Upstream OverlayBD's choice. It is tied to that project's
  ecosystem and is not coroutine-native in the way the data plane needs;
  adopting it would import upstream's architectural constraints along with
  its code. Rejected.
