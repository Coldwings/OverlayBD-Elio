# ADR-0021: Reuse tag handlers on their bridge worker

- Status: accepted
- Date: 2026-10-20
- Supersedes: none
- Binds: src/ublk/elio_bridge.cpp, docs/ublk.md

## Context

The ublk bridge already retains a TaskGroup to own concurrent source work.
Admitting a fresh child for every request makes scheduler placement a repeated
part of the data path. Default round-robin admission frequently puts a handler
on a different worker from its bridge, even for a single queue at depth one.
The controlled placement experiment in issue
[#120](https://github.com/Coldwings/OverlayBD-Elio/issues/120) identifies this
handoff as a material part of the cached local-read latency.

LSMT read latency, throughput and CPU cost are the primary performance
objectives. Controlled local zero/LSMT reads improve with queue-local reusable
handlers. Actual gzip-reader controls also expose a trade-off: CPU-intensive
decompression for one queue concentrates on one worker and can lose substantial
high-depth throughput. Favor the LSMT path while retaining asynchronous source
semantics and documenting this cost; gzip throughput parity is not a condition
for choosing this placement policy.

A kernel tag permits one outstanding request at a time. This provides a natural
reuse boundary without serializing requests across different tags. Native
TaskGroup admission and joining must continue to own handler lifetime; an
optimization cannot lose a kernel completion when a child is rejected or
cancelled before invocation. Sources remain asynchronous and may deliberately
change affinity while executing.

## Decision

Retain one lazily admitted native TaskGroup handler per active tag, deliver its
successive requests through a native event, and restore the bridge's worker
before accessing tag state or publishing completion.

## Consequences

- A tag's initial child admission may migrate once to its bridge worker. A
  reused tag avoids a fresh round-robin admission; unused tags do not allocate
  handler frames or incur startup scheduling.
- Independent tags retain overlapping asynchronous source operations. CPU work
  for one queue runs on its bridge worker. The default favors LSMT performance;
  CPU-intensive gzip work can use fewer cores and lose high-depth throughput.
  This is a per-queue policy, with the runtime's worker count unchanged.
  Multi-queue and remote/slow-source behavior require separate validation;
  cached LSMT gains do not establish improvement for every source.
- The bridge worker owns the tag's pending request, busy state and event.
  Foreign child destruction publishes atomic failure facts and wakes the
  bridge. The bridge consumes those facts before tag reuse. An active handler
  publishes its failure fact before fallback completion can allow reuse.
- Cancellation is checked before every request's source invocation. Ordinary
  stop instead closes admission, signals parked handlers and joins all accepted
  source operations before buffers or state are released. Closing is a sticky
  predicate checked before every wait: a request wake and close wake may
  coalesce, but accepted work finishes and the handler then leaves without
  requiring another notification.
- Failed event dispatch allocation keeps the same request and bridge owner
  until notification succeeds. Unset event waits and source task frames can
  still allocate; handler reuse is not an allocation-free execution contract.
- ADR-0006's queue-thread ring ownership is unchanged. Both queue/worker
  handoffs remain. This decision covers a first part of issue #120; the issue
  remains responsible for evaluating and addressing or explicitly disposing
  of those residual costs.

## Alternatives considered

- Fresh per-request TaskGroup admission repeats the measured placement hop.
  Binding the newly admitted body still pays that initial hop on every I/O.
- Keeping fresh round-robin admission distributes CPU-heavy source work across
  workers, but retains the higher cached LSMT floor. The selected default accepts
  the measured gzip trade-off in favor of the primary LSMT objective.
- Eagerly starting every tag handler multiplies startup work at large queue
  geometries even when only a small number of tags are used.
- Directly awaiting each source read in the bridge serializes independent
  tags and harms remote/slow-source concurrency.
- A custom lifecycle owner or private coroutine manipulation would duplicate
  TaskGroup responsibilities. The selected design uses supported placement,
  event, cancellation and joining APIs.
- Integrating the command ring and source execution under one serving thread
  may remove the remaining handoffs. That requires a separate ownership,
  task-work and shutdown design; this refinement does not select or reject it.
