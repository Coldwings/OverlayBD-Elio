# Architecture Decision Records

This directory holds overlaybd-elio's **Architecture Decision Records
(ADRs)** — the project's legislative history. Each ADR captures one
contract-level decision: the forces behind it, the rule it establishes, and
the alternatives that were considered and rejected.

## Two layers, one source of truth each

- **Current law** lives in `docs/design-assumptions.md`, the module documents
  (`docs/<module>.md`), and `AGENTS.md`. Those files always describe the rule
  as it stands *today*; reading them is sufficient to work on the code.
- **Case law** lives here. An ADR is a frozen snapshot of *why* a rule exists
  and what was rejected. Once accepted, an ADR's body never changes (status
  lines, links, and typos excepted).

When the current-law wording is ambiguous, the **newest accepted ADR wins**.

## When an ADR is required (trigger list)

An ADR file must ride in the same PR as the change it governs when the change:

- **T1** — modifies a wire or on-disk format: the ZFile/LSMT headers, index
  encodings, or compression framing; the supervisor↔obdctl or
  supervisor↔obd-device protocols; the `config.json` semantics consumed from
  overlaybd-snapshotter; or the ublk uapi compatibility layer's view of the
  kernel ABI;
- **T2** — adds, drops, or weakens an entry in `docs/design-assumptions.md`;
- **T3** — changes the Stability Contract section of any module document;
- **T4** — establishes a cross-module convention: a rule that **two or more
  modules depend on** and that is not written in any one module's contract
  sections (e.g. "the Elio scheduler never submits to a ublk queue ring",
  "sources never cache mutable state across a reopen");
- **T5** — rejects a significant alternative without adopting it
  (decision-by-exclusion, e.g. "no in-process P2P engine; DART stays an
  external proxy"); the ADR exists so the alternative is not re-proposed.

An ADR is **not** required for: bug fixes that restore documented behavior,
test additions, documentation clarifications, or internal refactors that
preserve contracts. Bureaucracy resistance is a design goal: a one-paragraph
ADR is a good ADR.

## Format

Files are `NNNN-kebab-title.md`; numbers are zero-padded, sequential, and
**never reused** — including rejected records. Reserve the number in the index
table below inside the same PR that adds the file.

```markdown
# ADR-NNNN: <imperative, decision-shaped title>

- Status: proposed | accepted | superseded by ADR-XXXX | rejected
- Date: YYYY-MM-DD (of the status)
- Supersedes: <ADR-XXXX or "none"> (required: the lifecycle's supersession
  audit keys off this line; it must pair with the old record's status change)
- Binds: <files/sections this decision constrains>

## Context
<forces and constraints; link issues/PRs>

## Decision
<one enforceable, imperative sentence — the load-bearing line>

## Consequences
<what becomes easier/harder; which code paths are bound>

## Alternatives considered
<rejected options and why — the anti-relitigation record>
```

## Lifecycle

```
proposed ──(PR merges)──> accepted ──(a new ADR overrides it)──> superseded
    └──(review declines)──> rejected (kept on file)
```

- **proposed → accepted** happens when the PR carrying the ADR merges. The
  decision and its implementation land atomically and are reviewed together.
- **Weakening or reversing** an accepted ADR requires a **new ADR** whose
  `Supersedes` line names the old one. The superseding PR must update every
  document in the old ADR's `Binds` list — that list is the audit surface.
  The old file stays, its status becomes `superseded by ADR-XXXX`.
- **rejected** records are kept permanently: they are the strongest
  anti-re-proposal material the project has.
- If two PRs race for the same number, the merge conflict in the index below
  forces a renumber. At this repository's change rate that is sufficient.

## References and anchoring

**Back-references are mandatory (the push channel).** A reader navigates this
repository by entry file + current file + whatever is linked from them; an ADR
nothing references is effectively invisible. Every non-superseded ADR must
therefore be cited by number (`ADR-NNNN`) from at least one current-law
document (`docs/*.md` or `AGENTS.md`) — `scripts/check-docs.sh` reports
missing references as an advisory (non-blocking) note; a PR may merge with an
open note only once the PR thread records a disposition (fixed, or waived with
a reason). Superseded records are exempt: they stay reachable through the
`Supersedes` chain. Anchor where a reader of the current rule needs the
rationale. For **rejected** records the anchor belongs at the *temptation
site* — the current-law entry whose rule the rejected alternative would
violate — never in "see also" link piles.

**ADR bodies carry no live code anchors.** An accepted ADR's body is frozen;
a live anchor must stay valid. Those rules are mutually exclusive — one
forces edits to frozen text, the other a permanently red CI — so the body
must be **self-contained**: describe behavior semantically, in prose that
needs no code reference to be understood. `Context` may optionally link an
"as of this PR" commit snapshot: the ADR and the code it discusses land in
the same PR, and a frozen reference is a *feature* for historical claims.
Live, drift-checked anchors are for current-law documents, which are few and
read every session — the cost-benefit closes only there.

**Current law vs case law, editorially.** Current-law documents state what
the rule is *today*; ADRs state *why*, and what was rejected. Sentences built
around "used to / originally / we considered / rejected" belong in ADRs, not
in `docs/*.md`. One explicit exception: a single-clause issue/ADR reference
kept inline as a reader signpost (e.g. "the ADR-0003 rule: …") stays
acceptable in current law — it is a pointer, not a narrative.

## Index

| ADR | Title | Status |
|---|---|---|
| [0001](./0001-minimalist-assumptions-as-contract.md) | Minimalist assumptions are contract | accepted |
| [0002](./0002-elio-as-runtime-foundation.md) | Elio is the runtime foundation | accepted |
| [0003](./0003-ublk-over-tcmu.md) | ublk, not tcmu, is the block-device backend | accepted |
| [0004](./0004-per-device-process-isolation.md) | One isolated process per device | accepted |
| [0005](./0005-dart-as-external-prefix-proxy.md) | DART integrates as an external prefix proxy | accepted |
| [0006](./0006-ublk-ring-ownership-and-elio-bridge.md) | Queue rings belong to queue threads; Elio bridges by completion | accepted |
| [0007](./0007-readonly-first-scope.md) | Read-only first: writable layers and TurboOCI are out of scope | superseded by ADR-0008 |
| [0008](./0008-writable-upper-layers.md) | Writable upper layers: sparse file and in-place-edit LSMT-RW | superseded by ADR-0020 |
| [0009](./0009-discard-punch-hole.md) | Discard/punch-hole with mask-with-zeroes semantics | accepted |
| [0010](./0010-ublk-user-recovery.md) | ublk USER_RECOVERY crash recovery with bounded respawn | accepted |
| [0011](./0011-sparse-layer-store.md) | Unify layer persistence into a sparse-file LayerStore with sidecar bitmap and per-extent CRC | accepted |
| [0012](./0012-read-admission-funnel.md) | Admit all remote reads through one priority funnel with scavenger-class prefetch | accepted |
| [0013](./0013-trace-prefetch.md) | Record and replay prefetch traces in upstream-compatible format without a photon dependency | accepted |
| [0014](./0014-offline-commit-and-cli-boundary.md) | Commit offline with deterministic seal, grow-only resize, and an external CLI boundary | accepted |
| [0015](./0015-registry-token-singleflight-and-expiry.md) | Single-flight registry token re-auth with expires_in-honoring cache lifetimes | superseded by ADR-0017 |
| [0016](./0016-persistence-never-gates-boot.md) | Layer persistence is best-effort — an unusable layer dir degrades to remote-only, never fails assembly | accepted |
| [0017](./0017-self-mode-bearer-url-info-expiry.md) | Cap Self-mode Bearer URL-info cache by token expiry | accepted |
| [0018](./0018-download-trycnt-structural-boundary.md) | Treat invalid download tryCnt as a structural configuration error | proposed |
| [0019](./0019-streaming-tar-converter.md) | Build tar streams into deterministic device-free LSMT layers | accepted |
| [0020](./0020-native-turbooci.md) | Read TurboOCI natively and build its metadata locally | accepted |
