// RegistrySource — a BlobSource over an OCI registry blob URL, plus the
// shared RegistryClient implementing the overlaybd registryfs v2 request
// contract (docs/source.md §Registry, design-assumptions.md §S-3):
//
//   * GET with Range only — never HEAD; blob size comes from a Range 0-0
//     probe's Content-Range total.
//   * 401/403 → parse WWW-Authenticate Bearer {realm,service,scope}, fetch
//     a token with Basic auth from the credential store. Token cache
//     lifetime honors the OAuth2 expires_in field at 80% of the declared
//     lifetime, falling back to a fixed 30s when the field is absent or
//     unparsable (ADR-0015). Concurrent re-auths are single-flight: one
//     coroutine performs the exchange, the rest await it.
//   * 3xx redirects → cache the Location for 300s and GET it without auth
//     (registryfs_v2.cpp Redirect mode); otherwise re-send with auth per
//     request (Self mode).
//   * Retries 3x with short backoff; 416 → -ERANGE; 429 → -EBUSY;
//     401/403 after token refresh → -EPERM.
//   * When an accelerate prefix (DART proxy, ADR-0005) is configured, the
//     actual request URL is the prefix + "/" + the actual URL; in Self mode
//     the Authorization header is still sent (overlaybd semantics — DART is
//     a trusted on-node daemon and ignores it).
#pragma once

#include "source/blob_source.hpp"
#include "source/credentials.hpp"

#include <elio/coro/task.hpp>
#include <elio/http/http_client.hpp>
#include <elio/sync/event.hpp>
#include <elio/sync/mutex.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace obd::source {

namespace detail {

/// Maps an OAuth2 `expires_in` value (seconds, std::nullopt when the field
/// is absent or unparsable) to the token cache lifetime: 80% of the
/// declared lifetime (proactive refresh margin), or the fixed 30 s
/// fallback. A declared lifetime of 0 caches the token as already expired;
/// absurd declared lifetimes are capped at 7 days so a hostile endpoint
/// can neither pin a token forever nor overflow the arithmetic.
std::chrono::steady_clock::duration token_cache_lifetime(
    std::optional<int64_t> expires_in_seconds);

}  // namespace detail

struct RegistryClientConfig {
    std::string user_agent = "overlaybd-elio/0.1";
    std::chrono::seconds connect_timeout{10};
    std::chrono::seconds read_timeout{30};
    /// DART accelerate base ("http://host:port/prefix"), empty = direct.
    std::string accelerate_base;
    /// Max buffered response body (largest single Range we ever request).
    size_t max_response_size = 64 * 1024 * 1024;
};

/// Shared HTTP client + auth/redirect caches for one device process. All
/// RegistrySources of an image share one instance (same layering as
/// overlaybd's per-image registryfs). The client must outlive all in-flight
/// coroutines using it — methods borrow `this` (same lifetime contract
/// shape as the ublk bridge's documented source rule, docs/ublk.md).
class RegistryClient {
public:
    RegistryClient(CredentialStorePtr creds, RegistryClientConfig cfg);

    /// Range GET of [offset, offset+count) into `buf`. Returns `count` on
    /// success, negative -errno on failure (EPERM auth, ERANGE 416, EBUSY
    /// 429, EIO otherwise, ETIMEDOUT after retries).
    elio::coro::task<ssize_t> get_data(const std::string& url, void* buf,
                                       uint64_t offset, size_t count);

    /// Total blob size from a Range 0-0 Content-Range total; -errno on
    /// failure.
    elio::coro::task<int64_t> get_length(const std::string& url);

private:
    struct TokenEntry {
        std::string token;
        std::chrono::steady_clock::time_point expiry;
        /// Bumped on every successful exchange; a 401'd request demands a
        /// strictly newer generation before retrying (ADR-0015).
        uint64_t generation = 0;
    };
    /// One in-flight token exchange. Concurrent re-auths for the same
    /// realm|service|scope key await the same flight instead of each
    /// running the exchange (single-flight, ADR-0015).
    struct TokenFlight {
        elio::sync::event done;
        TokenEntry entry;             // valid when error is null
        std::exception_ptr error;     // set when the exchange failed
    };
    struct UrlInfo {
        std::string final_url;     // original (self) or redirect Location
        std::string auth_header;   // empty for redirect mode
        std::chrono::steady_clock::time_point expiry;
        /// Generation of the token auth_header was built from (0 = none).
        uint64_t token_generation = 0;
    };

    /// GET with Range [first,last]; returns nullopt with errno set on
    /// transport failure.
    elio::coro::task<std::optional<elio::http::response>> request_range(
        const std::string& url, uint64_t first, uint64_t last,
        const std::string& auth_header);

    /// Resolves auth + redirect state for `url`, refreshing caches as
    /// needed (registryfs_v2 get_scope_auth/get_actual_url). When
    /// `min_token_generation` is non-zero (a previous attempt took a 401),
    /// a cached token is only reused if its generation is at least that
    /// value; otherwise a new exchange is performed/awaited. Throws
    /// obd::error on failure.
    elio::coro::task<UrlInfo> resolve(const std::string& url,
                                      uint64_t min_token_generation = 0);

    /// Returns a usable token for the cache key, performing or awaiting a
    /// single-flight exchange when the cached entry is expired or older
    /// than `min_generation`. Throws obd::error on exchange failure.
    elio::coro::task<TokenEntry> get_token(const std::string& key,
                                           const std::string& realm,
                                           const std::string& service,
                                           const std::string& scope,
                                           const std::string& url_for_creds,
                                           uint64_t min_generation);

    /// Raw token endpoint response: the bearer token plus the cache
    /// lifetime derived from expires_in (see detail::token_cache_lifetime).
    struct TokenResponse {
        std::string token;
        std::chrono::steady_clock::duration lifetime;
    };

    /// Fetches a Bearer token for the challenge realm/service/scope with
    /// Basic auth from the credential store. Throws obd::error on failure.
    elio::coro::task<TokenResponse> fetch_token(const std::string& realm,
                                                const std::string& service,
                                                const std::string& scope,
                                                const std::string& url_for_creds);

    CredentialStorePtr creds_;
    RegistryClientConfig cfg_;
    elio::http::client http_;
    elio::sync::mutex mu_;
    std::map<std::string, TokenEntry> tokens_;    // key: realm|service|scope
    std::map<std::string, UrlInfo> url_infos_;    // key: original url
    // In-flight token exchanges, same key as tokens_.
    std::map<std::string, std::shared_ptr<TokenFlight>> flights_;
};

using RegistryClientPtr = std::shared_ptr<RegistryClient>;

class RegistrySource final : public BlobSource {
public:
    /// Probes the blob size (one Range 0-0 request) and binds the source to
    /// `client` for subsequent reads. Throws obd::error on failure.
    static elio::coro::task<std::unique_ptr<RegistrySource>> open(
        RegistryClientPtr client, std::string url);

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;

    uint64_t size() const noexcept override { return size_; }
    std::string_view label() const noexcept override { return label_; }

private:
    RegistrySource() = default;

    RegistryClientPtr client_;
    std::string url_;
    uint64_t size_ = 0;
    std::string label_;
};

}  // namespace obd::source
