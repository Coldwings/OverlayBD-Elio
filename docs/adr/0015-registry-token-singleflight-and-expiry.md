# ADR-0015: Single-flight registry token re-auth with expires_in-honoring cache lifetimes

- Status: proposed
- Date: 2026-09-08
- Supersedes: none
- Binds: docs/source.md, src/source/registry.hpp, src/source/registry.cpp

## Context

The registry client's token cache had a fixed 30 s lifetime and no
coordination between concurrent re-authentications (issue #4). Two
problems followed:

1. **Thundering-herd re-auth.** When a cached token expired (or was
   rejected server-side) under concurrent load, every in-flight request
   took a 401 and each ran its own token exchange against the registry's
   token endpoint. Against rate-limited endpoints (Docker Hub, GHCR) this
   multiplies exactly the load the rate limiter is throttling.
2. **Lifetimes blind to the server.** OAuth2 token endpoints declare the
   token's validity via `expires_in`; a fixed 30 s cache both under-uses
   long-lived tokens (needless re-auths) and over-trusts short-lived ones
   (extra 401 round trips).

The redirect/URL-info cache has no analogous server-declared lifetime in
the registryfs v2 contract: 3xx `Location` responses and probe 200/206
responses carry no expiry field, and CDN signed-URL lifetimes live inside
opaque query parameters.

## Decision

- **Single-flight re-auth.** Concurrent token exchanges for the same
  `realm|service|scope` key are coalesced: the first requester performs
  the exchange; the rest await the same flight (shared_ptr + coroutine
  event) and share its result or its error.
- **Generation counter.** Every successful exchange bumps a per-key token
  generation. A request that took a 401 retries only with a strictly
  newer generation: if the cache still holds the generation that was just
  rejected, the request performs/awaits a refresh even when that entry is
  time-valid. The per-request 401 retry budget is unchanged — single-flight
  changes *who* performs the exchange, not how many retries a request gets.
- **`expires_in` honored at 80%.** The token cache lifetime is 80% of the
  declared `expires_in` (proactive refresh margin). The fixed 30 s fallback
  covers absent, unparsable, and negative values; `expires_in=0` caches the
  token as already expired; absurd declared lifetimes are capped at 7 days
  so a hostile or buggy endpoint can neither pin a token in the cache
  forever nor overflow the lifetime arithmetic. The redirect/URL-info
  cache keeps its fixed 300 s lifetime.

## Consequences

- A burst of N concurrent 401s costs one token exchange, not N; token
  endpoint pressure under expiry events becomes constant.
- Registries issuing long-lived tokens see proportionally fewer exchanges;
  registries issuing short-lived tokens stop being asked against stale
  credentials (the 401 drop-and-re-resolve path remains as the backstop).
- `docs/source.md`'s Stability Contract wording for the registry wire
  behavior is updated: the wire-visible request shapes still mirror
  overlaybd `registryfs_v2.cpp`, but cache lifetimes are now derived from
  `expires_in` rather than being fixed constants.

## Alternatives considered

- **Keep fixed lifetimes, add only single-flight.** Rejected: it fixes
  the herd but keeps the needless re-auth churn for long-lived tokens and
  the guaranteed-stale window for short-lived ones; parsing `expires_in`
  is a few lines against the already-required JSON parse.
- **Serialize whole resolve() calls per URL.** Rejected: it would coalesce
  re-auth as a side effect but also serializes the independent probe
  traffic of unrelated requests; coalescing only the token exchange keeps
  the concurrency the current design relies on.
- **Parse CDN signed-URL expiry parameters for the redirect cache.**
  Rejected: the parameter names are vendor-specific (X-Amz-Expires,
  Google signature params, Azure se=), so this would be a heuristic
  masquerading as a contract; the 401 drop-and-re-resolve path already
  bounds the staleness damage.
