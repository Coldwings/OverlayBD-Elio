# ADR-0006: Queue rings belong to queue threads; Elio bridges by completion

- Status: accepted
- Date: 2026-09-07
- Supersedes: none
- Binds: src/ublk/, AGENTS.md

## Context

The ublk data plane serves each device queue through a per-queue command
ring, and each such ring is an io_uring instance. An io_uring submission
queue follows a single-submitter discipline: exactly one thread may push
submissions into a ring, and concurrent submission from foreign threads is
a correctness hazard, not merely a performance concern.

The Elio scheduler, which runs the image stack (format readers, blob
sources, caches) as coroutines on its own threads, would naturally want to
submit IO completions itself — the completion is produced on an Elio
thread, and handing it back to the queue thread first looks like pure
overhead. But letting Elio threads write to the rings directly would put
two submitters on every ring and void the single-submitter guarantee.

What is needed is a boundary: one side runs the ublk protocol on the queue
thread, the other runs the image stack on Elio threads, and requests and
completions cross that boundary through a narrow, auditable handoff.

## Decision

Own each ublk queue ring exclusively by its queue thread: the Elio
scheduler must never submit to a queue ring; requests cross the boundary by
eventfd notification into the Elio scheduler, and completions cross back
through a per-queue completion queue that only the owning queue thread
turns into ring submissions.

## Consequences

- There is a single, auditable handoff point — the bridge — between the
  ublk protocol world and the Elio coroutine world. Concurrency review of
  the data plane reduces to reviewing that one boundary.
- Queue threads do no format or source work: they fetch requests from the
  kernel, notify the Elio side, and translate finished completions into
  ring submissions. All image-stack work happens inside Elio coroutines.
- Elio coroutines never touch ring memory beyond the documented IO
  buffers; the ring structures, tags, and submission protocol remain the
  queue thread's private concern, preserving the single-submitter
  discipline by construction rather than by locking.
- Per-queue completion queues keep devices with many queues parallel:
  there is no global lock on the completion path.

## Alternatives considered

- **Let Elio submit to the rings directly.** Rejected: a second submitter
  violates the io_uring single-submitter discipline, a correctness hazard
  regardless of how rarely the races would manifest.
- **One global lock around all rings.** Rejected: it would serialize every
  queue of every device behind a single critical section, destroying the
  per-queue parallelism that is the point of the ublk multi-queue design.
- **Lock-free SPSC queues in both directions without eventfd.** Rejected:
  the Elio side must sleep on its own scheduler when no work is pending;
  the eventfd is precisely the integration point that lets the Elio
  reactor park and wake the bridge coroutine, so removing it would force
  spinning or a foreign wakeup mechanism.
