# ADR-0032: Batch ready completions on the fixed queue owner

- Status: accepted
- Date: 2026-10-28
- Supersedes: ADR-0023
- Binds: src/ublk/queue.hpp, src/ublk/queue.cpp, src/ublk/elio_bridge.cpp, docs/ublk.md, docs/design-assumptions.md

## Context

Issue [#187](https://github.com/Coldwings/OverlayBD-Elio/issues/187) evaluates
per-result raw-ring submission overhead. Several ready handlers can share a
submission without waiting for unfinished source operations. ADR-0023's fixed
worker, source lifetime, STOP and original-command retirement rules remain
requirements; only completion submission timing and owner notification change.

## Decision

Keep an owner-local count of delivered requests that have not been converted
into prepared commands. Restore the owner before preparing every result.
If no other delivered request remains, submit immediately. Otherwise, retain
prepared replies in the existing bounded SQ and notify the owner eventfd once
per pending batch. The owner flushes pending replies before its next CQ turn,
independently of outstanding source operations. A final ready handler may
submit the batch sooner. No timeout or all-source join is a flush prerequisite.

Before closing tag admission and joining handlers, disable batching and flush
prepared replies. Every subsequent completion submits directly, including
foreign-mailbox results collected during drain. Failed queues use direct
submission. Submission errors and partial progress retain the existing debt
tracking and maintenance retry path; a prepared cookie is not proof of kernel
retirement. STOP continues to suppress fresh FETCH only.

Keep the fixed worker, native eventfd wait, bounded CQ fairness, unique command
cookies, source-buffer fence and positive original-command retirement rules
from ADR-0023. Batching introduces no second raw-ring owner or unbounded queue.

## Consequences

Ready results may share a submit. Multiple outstanding requests can add an
owner eventfd wake; a fast stream beside a slow source may not benefit. A lone
request remains direct. Measure throughput, latency and server CPU separately;
fewer intended submissions alone do not prove a performance improvement.

## Alternatives considered

- Wait until every source completes: rejected because one slow source would
  hold unrelated completed requests.
- Keep batching while the owner joins source handlers: rejected because the
  owner cannot independently flush while suspended in that join.
- A separate persistent flusher coroutine: not selected because the existing
  owner wake can provide independent progress without another lifetime owner.
- Always submit each result: baseline fallback if validation or measurements
  do not justify the additional batching state.
