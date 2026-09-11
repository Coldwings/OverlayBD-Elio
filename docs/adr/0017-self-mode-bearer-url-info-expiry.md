# ADR-0017: Cap Self-mode Bearer URL-info cache by token expiry

- Status: accepted
- Date: 2026-09-11
- Supersedes: ADR-0015
- Binds: docs/source.md, src/source/registry.hpp, src/source/registry.cpp

## Context

ADR-0015 made registry Bearer tokens honor the OAuth2 `expires_in`
lifetime at a proactive 80% refresh point, but left the per-URL
resolution cache at a fixed 300 seconds. That is safe for redirect-mode
entries, where the cached value is an opaque CDN `Location` fetched
without Authorization, and for anonymous or Basic-auth entries, which do
not depend on the Bearer token cache. It is not safe for Self-mode Bearer
entries: the URL-info cache also stores the `Authorization: Bearer ...`
header that every data request reuses.

A short-lived Bearer token can therefore expire in the token cache while
the same already-open source keeps serving the same URL through a still
valid URL-info entry. The reactive 401 drop-and-re-resolve path remains a
backstop, but ADR-0015's proactive refresh rule should apply before data
requests are sent with stale cached Bearer headers.

## Decision

Self-mode Bearer URL-info entries must expire no later than the Bearer
token's proactive refresh deadline. Anonymous, Basic, and Redirect-mode
URL-info entries keep the fixed 300 second cache lifetime because those
entries do not send cached Bearer headers on data requests.

## Consequences

- Reusing an already-open RegistrySource for the same Self-mode URL now
  refreshes before the cached Bearer header passes the token cache's
  proactive expiry.
- Redirect-mode CDN `Location` entries stay cached for 300 seconds; CDN
  expiry is still handled by the existing 401/403 drop-and-re-resolve
  path because redirect responses carry no standard lifetime field.
- Basic and anonymous resolution entries remain unchanged; they are not
  coupled to the Bearer token generation or token expiry.
- ADR-0015's single-flight exchange, generation counter, and per-request
  retry budget remain in force.

## Alternatives considered

- **Keep URL-info at a fixed 300 seconds for all modes.** Rejected: it
  contradicts the proactive Bearer refresh margin by letting a cached
  Self-mode Authorization header outlive its token cache entry.
- **Cap all URL-info entries by the token lifetime.** Rejected: redirect,
  Basic, and anonymous entries either do not send Bearer credentials or
  do not have a Bearer token lifetime, so shortening them adds churn
  without improving correctness.
- **Parse CDN signed-URL expiry parameters.** Rejected for the same reason
  as ADR-0015: vendors encode lifetimes differently, and the existing
  401/403 re-resolution path already handles expired redirect targets.
