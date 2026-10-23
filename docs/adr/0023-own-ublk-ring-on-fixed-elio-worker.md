# ADR-0023: Own each ublk ring on one fixed Elio worker

- Status: accepted
- Date: 2026-10-23
- Supersedes: ADR-0006
- Binds: docs/design-assumptions.md, docs/ublk.md, AGENTS.md, src/ublk/

## Context

The dedicated queue-thread bridge preserved io_uring single-issuer safety, but
made every request cross from that thread to an Elio worker and every result
cross back. At QD1 those wakes, scheduler handoffs, and cache-line transfers
cost more CPU than the LSMT lookup itself. Reusable queue-local tag handlers
removed their additional cross-worker dispatch, but the two queue/worker
handoffs remained.

The ublk driver pins each accepted FETCH to its issuing Linux task and requires
the corresponding COMMIT_AND_FETCH from that task. Elio source operations may
suspend and resume elsewhere, while the raw ring uses SINGLE_ISSUER and
DEFER_TASKRUN. The design must therefore preserve an exact task identity,
overlapping tags, scheduler progress, and the failed-startup retirement
contract while removing the steady-state bridge.

## Decision

Run each queue's raw ublk ring, bounded fetch/completion pump, and reusable tag handlers on one fixed Elio worker; only that worker submits raw-ring commands, it awaits raw CQ progress through a separately registered eventfd on Elio native I/O, and its owner task remains alive through STOP and proven command retirement.

## Consequences

- A normal handler restores its queue worker after a source await and submits
  COMMIT_AND_FETCH directly. It uses no cross-thread request/completion deque
  and no application eventfd write.
- The registered eventfd lets the owner coroutine park without blocking the
  worker. With DEFER_TASKRUN, Linux signals that eventfd on the local-work
  list's empty-to-nonempty transition before CQ publication. The owner treats
  the wake as a hint and enters the raw ring with GETEVENTS to execute the
  deferred work on the issuing task.
- Each pump consumes a bounded CQ batch before taking a native-timer turn.
  Multiple queues can occupy different workers; overlapping tags remain
  independent TaskGroup children on their queue owner.
- Source drain and raw-ring lifetime are separate states. STOP is allowed only
  after tag handlers have joined; the owner continues consuming original CQEs
  until its ledger is complete, and ring/mapping/cdev closure additionally
  waits for final owner-frame destruction.
- An exceptional handler that fails on another worker publishes only through
  the generation mailbox and wakes the owner. Foreign continuations never
  inspect or mutate raw-ring state.
- A live Device assumes its worker set remains stable. The current device
  process does not resize the scheduler; future dynamic shrink requires an
  Elio external-worker lease or an equivalent lifetime API.

## State transitions and race closure

The owner is the only writer of raw slot state. For one tag the normal cycle
is `FETCH pending -> delivered -> handler busy -> COMMIT_AND_FETCH pending ->
FETCH pending`; an exact raw cookie and an exact handler generation must both
match before reuse. Stop changes admission, not ownership:

1. A stop/failure request publishes its sticky predicate, then increments the
   owner eventfd. If the counter is saturated, an unread wake already exists.
2. The owner closes admission and joins every accepted handler. A successful
   FETCH CQE observed after closure becomes a cancellation reply; it is never
   loaned to a new source handler.
3. Device waits for both source-handler drain and synchronous issuance of all
   prepared initial FETCH commands before sending STOP. The test-only
   pre-submit gate is asynchronous, so holding this fence cannot park the
   owner worker.
4. STOP prevents preparation of a new bare FETCH. It does not discard a
   delivered request: if a successful FETCH CQE becomes visible after STOP,
   the owner still submits that slot's COMMIT_AND_FETCH and consumes its
   terminal CQE.
5. Only explicit owner-exit permission plus a known-complete raw ledger ends
   the pump. Device additionally waits for destruction of the joinable owner
   frame before closing the ring, mapping, cdev or source graph.

The eventfd wait is level-preserving: a raw CQ or control wake before the Elio
read makes the descriptor readable, one after arming completes that read, and
one racing the check is covered by one of those cases. For a raw completion,
`UBLK_F_URING_CMD_COMP_IN_TASK` adds deferred local work to the issuing ring.
Linux `io_req_local_work_add()` first links that work and, on the list's
empty-to-nonempty transition, calls `io_eventfd_signal(ctx, false, ...)`;
`cqe_event=false` deliberately bypasses the CQ-tail check. This pre-CQE signal
wakes the Elio read, after which the owner restores its recorded worker and
calls GETEVENTS on the raw ring. A foreign rejected handler follows the
separate atomic mailbox sequence `armed -> writing -> ready`; only the owner
can consume the matching generation and submit its fallback completion.

## Alternatives considered

- Keep the dedicated queue thread and optimize its notifications. Rejected:
  batching can reduce QD16 notifications but leaves both QD1 handoffs, the
  dominant measured LSMT latency cost.
- Permit whichever worker finishes a source read to submit the raw ring.
  Rejected: worker migration would violate both io_uring single-issuer and
  ublk's FETCH/COMMIT task-identity requirement.
- Block the fixed Elio worker in `io_uring_submit_and_wait`. Rejected: an idle
  queue would prevent source, control, and other queue coroutines assigned to
  that worker from progressing.
- Wait for a generic Elio URING_CMD abstraction. Rejected: Elio's existing
  worker placement and native eventfd I/O are sufficient, while the ublk
  command ledger remains clearer as project-owned kernel-protocol code.
