# ADR-0022: Retain failed startup owners until positive retirement

- Status: proposed
- Date: 2026-10-20
- Supersedes: ADR-0010
- Binds: src/ublk/startup_control.hpp, src/ublk/ctrl.cpp,
  src/ublk/queue.cpp, src/ublk/device.cpp, src/cmd/obd_device_main.cpp,
  src/supervisor/child.cpp, src/supervisor/daemon.cpp,
  src/supervisor/protocol.cpp, src/supervisor/device_control.cpp,
  docs/ublk.md, docs/supervisor.md, docs/operations.md, docs/binaries.md,
  docs/config.md, docs/design-assumptions.md

## Context

[Issue #136](https://github.com/Coldwings/OverlayBD-Elio/issues/136)
concerns startup control commands that can remain pending.
[Issue #137](https://github.com/Coldwings/OverlayBD-Elio/issues/137)
concerns the separate ownership of queue commands published before recovery
END. Cancelling END does not retire those FETCH commands or their borrowed
buffers. The selected failed-recovery policy preserves the old device and
the failed replacement's complete resource graph until supported recovery or
explicit operator action. It does not authorize automatic STOP/DEL.

The supervisor must participate: retaining a Device inside the child cannot
protect it from automatic process termination, replacement or daemon teardown.
Explicit destroy must still attempt release where supported and report
refusal or uncertainty without pretending that elapsed time proves cleanup.

## Decision

Retain each failed startup operation and its execution owners until original
kernel commands and native source frames have positively retired, or until a
successful startup transfers ownership to the ready Device.

The retained graph includes the startup/caller coroutine frames, source and
image objects, buffers, mappings, control ring, queue rings and original queue
threads, and the runtime needed to progress them. Observers hold facts, not
resource ownership. Exceptions while reporting failure cannot unwind this
graph.

Original START_DEV or END_USER_RECOVERY success commits ownership immediately,
even after cancellation intent. START_USER_RECOVERY success prepares recovery
but does not adopt the old device. A cancellation CQE, STOP CQE, timeout,
channel EOF or process reap is not another command's retirement evidence.

### Admit supervised startup with a generation-bound grant

Protocol batch 5 advertises `startup_owner`, version 1. A supervised child
requires a nonzero `--startup-generation`; before publishing device
obligations it offers its ownership state and waits for a matching
`startup_begin` grant. The supervisor installs registry/monitor ownership and
protection from automatic termination before attempting the grant write.
Failed or ambiguous delivery does not revoke that protection.

Supported new/old binary pairings fail before device publication: a new child
rejects the missing generation argument, and a supported old child rejects
the unknown argument. An arbitrary custom executable is not assumed to
provide that guarantee. Missing or malformed status never implies legacy
safe cleanup. Standalone/library callers retain equivalent lifetime duties.

### Report retained failure and service explicit release

Retained failure uses lifecycle state `failed` with an additive
`startup_owner` record: generation, revision, phase, reason, control/raw-debt
facts, and release sequence/result. Only the current generation and coherent
monotonic observations can change protection. Lost diagnostics cannot
authorize kill, replacement or successful destruction.

One private-channel reader handles startup and ready operation. An explicit
`startup_release` uses the current generation and an increasing nonzero
sequence below 2^63. Acceptance means pending. Ready handoff and release
admission share one atomic decision so an accepted release cannot be orphaned.

Explicit release must:

1. Close ordinary work admission and settle startup control/cancellation
   records while retaining their owners.
2. Establish the queue publication/submission boundary before authorized STOP.
   Keep queue owners and required source executors runnable; already delivered
   requests may need COMMIT to let STOP finish.
3. Observe original terminal CQEs for all published commands and native
   source-frame destruction. Ambiguous publication retains ownership.
4. Join queue owners and close their ring/mapping/cdev resources before one
   authorized DEL. Preserve the DEL outcome; never blindly retry a numeric
   device ID after an ambiguous deletion.
5. Report correlated completion only after those barriers. The supervisor
   removes the matching entry only after terminal release and actual reap.

Refusal or timeout retains the attempt ledger. A retry observes or continues
that attempt; a later request after refusal uses a fresh sequence without
replaying already completed STOP/DEL work. Safe automatic cleanup is distinct
from acknowledgement of an operator's release request.

### Preserve standalone release ingress

Standalone `obd-device` creates an exclusive Linux abstract Unix socket before
image assembly for both create and recovery. Its default name includes effective
UID and PID; an explicit `--startup-socket @NAME` selects another name. A fresh
nonzero generation identifies each invocation independently of PID reuse.
`SO_PEERCRED` limits connections to the server's effective UID or root. Using an
abstract name avoids pathname unlink/replacement races and automatically frees
the name when the listener closes; filesystem socket paths are not supported.

Independent bounded request handlers expose `startup_status` and the same
`startup_release` generation/sequence admission. A slow or disconnected client
does not abandon the owner or block the acceptor. Acceptance reports pending;
a subsequent correlated outcome reports refusal or, only after all caller
cleanup, release. Status remains available for reconnecting clients while the
owner is alive. The terminal ownership record is also printed to stderr. Socket
EOF is not retirement evidence. Once ready, this startup-only endpoint rejects
release and ordinary signal shutdown applies.

Pre-owner setup failure atomically closes release admission even if no request
was pending. A racing request either wins and is settled during final cleanup,
or observes closed admission; no command may be accepted after its service
owner has passed the finalization boundary. A startup-owner phase alone does
not prove outer image/fill cleanup: the correlated terminal socket outcome is
held until that cleanup completes. Listener and client native frames
are drained before the borrowed StartupControl or runtime is destroyed.

### Retain operational supervisor maintenance

Create failure/timeout, control EOF and shutdown do not weaken protected
startup ownership. No competing replacement may start over an unresolved
owner. Commit refuses retained/releasing generations, including an externally
reaped generation without retirement evidence.

On SIGTERM/SIGINT the supervisor closes new-work/grant admission. While
protected owners remain it keeps the listener, registry, monitor/reaper tasks
and runtime alive, reports `shutdown_pending`, and serves hello/list/status/
explicit destroy. It refuses other new work. Exceptions follow the same
ownership rule. Skipping kill in a destructor while abandoning the operational
graph is insufficient.

After validated ready handoff the ordinary ready-device shutdown policy
applies. External SIGKILL and host loss are outside this application guarantee;
they are never reported as successful retirement.

### Scope of supersession

This record replaces only ADR-0010's automatic replacement and shutdown
decisions where protected startup/recovery ownership remains unresolved.
Its recovery flags, REISSUE choice, bounded respawn policy for ordinary
ready-device crashes, capacity behavior and writable-upper durability
boundaries remain in force. Per-device isolation and bounded client replies
also remain; a bounded reply may report retained/pending/refused while the
resource owner continues running.

## Consequences

Failed recovery can retain a process and resources indefinitely. Operators
can inspect the reason and request release, but neither a client timeout nor
a daemon stop request fabricates a safe cleanup deadline. Supervisor/device
upgrades must use a compatible startup-owner pair; existing client lifecycle
fields keep their meanings.

The observer protocol adds no per-I/O handoff. This decision does not resolve
the data-plane performance scope of issue #120.

Acceptance requires deterministic generation/revision/sequence and
ready-versus-release tests; exception/EOF/timeout retention tests; complete
caller-to-supervisor maintenance and refusal/retry tests; and owned-fixture
kernel evidence on Linux 6.6 and a current kernel for original control/raw
CQEs, native-frame retirement and cdev-close-before-DEL. A single-FETCH probe,
successful STOP, thread join or helper-only test does not establish the
complete lifecycle. This proposed record makes no claim that those checks
have all passed.

## Alternatives considered

- Automatic failed-recovery STOP/DEL violates the selected preservation policy.
- Retaining only Device, or returning an owning failure exception, can lose
  caller/image/runtime owners during ordinary unwinding.
- Treating cancellation, STOP or process reap as a universal fence ignores
  independent original-command lifetimes.
- Leaking a process pointer after daemon shutdown loses operational control.
- Permanent unconditional destroy refusal omits the required supported release
  path.
- A signal-only standalone release cannot bind commands to a generation/sequence
  or return structured refusal/debt facts; it is not the management interface.
- Unconditionally rolling forward through END cannot repair failed queue
  preparation; a preserving kernel abort would require a separate verified ABI.
