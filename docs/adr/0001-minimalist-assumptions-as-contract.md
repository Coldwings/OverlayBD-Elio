# ADR-0001: Minimalist assumptions are contract

- Status: accepted
- Date: 2026-09-07
- Supersedes: none
- Binds: docs/design-assumptions.md, AGENTS.md

## Context

This project deliberately implements a minimal subset of upstream OverlayBD
semantics: read-only images, a bounded set of blob sources, and only the
behaviors needed to serve containers lazily through a block device. Minimalism
is what keeps the codebase reviewable and the test suite exhaustive, but it
has a failure mode: every module author carries their own idea of what the
rest of the system guarantees. An assumption that lives only in one
implementer's head diverges silently — a source assumes images never mutate
under it, a merger assumes layers arrive in a fixed order, a device process
assumes the supervisor always drains status — and the mismatch surfaces as a
heisenbug at the boundary, months later, in code neither party remembers
agreeing on.

The project also values reviewable change over exhaustive design: small
modules, small PRs, few documents. Any assumption-tracking mechanism must
therefore be a single, short, authoritative file, not a paperwork tax on every
change.

## Decision

Write every cross-module design assumption in `docs/design-assumptions.md`
and treat it as contract: code that violates a written assumption is a bug,
and adding, dropping, or weakening an assumption requires an ADR in the same
PR.

## Consequences

- There is exactly one top-precedence "current law" document for assumptions.
  Reviewers and agents check it first; when documents disagree, it wins (the
  precedence order is stated in `AGENTS.md`).
- Reviewers are empowered — and expected — to reject changes that rest on an
  undocumented cross-module assumption. "It was obvious" is not a defense;
  obvious assumptions are the cheapest to write down.
- Assumption changes become deliberately slow: they pass through the ADR
  process (trigger T2), so their ripple effects across modules are examined
  before the rule moves, not after.
- Modules may still hold purely internal invariants in their own documents;
  only assumptions another module relies on belong in the contract.

## Alternatives considered

- **Assumptions in code comments.** Invisible at review time, scattered
  across files, and they rot as the code around them changes. There is no
  moment at which anyone reads them all together, so divergence is
  undetectable. Rejected.
- **Per-module documents only.** Each module restating the assumptions it
  depends on guarantees duplication, and duplication guarantees drift: two
  copies of the same rule will eventually disagree about it. Rejected — module
  documents may reference the contract, never re-legislate it.
- **Full OverlayBD feature parity instead.** If we implemented every upstream
  behavior, assumptions would be "whatever upstream does" and the divergence
  problem would shrink. But parity explodes scope beyond what this project
  can test and maintain; the minimal subset is the point. Rejected.
