// RegistrySource / RegistryClient implementation. See registry.hpp for the
// contract and upstream (overlaybd registryfs_v2.cpp) mapping.
#include "source/registry.hpp"

#include "common/errors.hpp"
#include "source/base64.hpp"

#include <elio/time/timer.hpp>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <optional>

namespace obd::source {

namespace {

/// Parses "Bearer realm=\"...\",service=\"...\",scope=\"...\"" or "Basic
/// realm=\"...\"" from a WWW-Authenticate header.
struct Challenge {
    bool bearer = false;
    bool basic = false;
    std::string realm, service, scope;
};

std::optional<Challenge> parse_challenge(std::string_view header) {
    Challenge ch;
    if (header.starts_with("Bearer") || header.starts_with("bearer")) {
        ch.bearer = true;
        header.remove_prefix(6);
    } else if (header.starts_with("Basic") || header.starts_with("basic")) {
        ch.basic = true;
        header.remove_prefix(5);
    } else {
        return std::nullopt;
    }
    // Parse key="value" pairs.
    std::string_view rest = header;
    while (!rest.empty()) {
        const auto eq = rest.find('=');
        if (eq == std::string_view::npos) break;
        std::string_view key = rest.substr(0, eq);
        // Trim spaces/commas.
        while (!key.empty() && (key.front() == ' ' || key.front() == ','))
            key.remove_prefix(1);
        if (eq + 1 >= rest.size()) break;
        if (rest[eq + 1] != '"') break;
        const auto value_start = eq + 2;
        const auto end = rest.find('"', value_start);
        if (end == std::string_view::npos) break;
        const std::string_view value = rest.substr(value_start, end - value_start);
        if (key == "realm") ch.realm = value;
        else if (key == "service") ch.service = value;
        else if (key == "scope") ch.scope = value;
        rest = rest.substr(end + 1);
    }
    if (ch.bearer && ch.realm.empty()) return std::nullopt;
    return ch;
}

/// Parses "bytes A-B/TOTAL" (or "bytes A-B/*") from a Content-Range header.
/// Returns {first, total} with total == UINT64_MAX for '*'.
struct ContentRange {
    uint64_t first = 0;
    uint64_t total = 0;
};

std::optional<ContentRange> parse_content_range(std::string_view v) {
    if (!v.starts_with("bytes ")) return std::nullopt;
    v.remove_prefix(6);
    ContentRange out;
    const auto dash = v.find('-');
    const auto slash = v.find('/');
    if (dash == std::string_view::npos || slash == std::string_view::npos ||
        slash < dash) {
        return std::nullopt;
    }
    try {
        out.first = std::stoull(std::string(v.substr(0, dash)));
        if (v.substr(slash + 1) == "*") {
            out.total = UINT64_MAX;
        } else {
            out.total = std::stoull(std::string(v.substr(slash + 1)));
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return out;
}

int status_to_errno(int status) {
    switch (status) {
    case 401:
    case 403: return EPERM;
    case 404: return ENOENT;
    case 416: return ERANGE;
    case 429: return EBUSY;
    default: return EIO;
    }
}

std::string make_range_header(uint64_t first, uint64_t last) {
    return "bytes=" + std::to_string(first) + "-" + std::to_string(last);
}

}  // namespace

namespace {

elio::http::client_config make_http_config(const RegistryClientConfig& cfg) {
    elio::http::client_config cc;
    cc.follow_redirects = false;  // resolved manually (redirect caching)
    cc.max_response_size = cfg.max_response_size;
    cc.connect_timeout = cfg.connect_timeout;
    cc.read_timeout = cfg.read_timeout;
    cc.user_agent = cfg.user_agent;
    return cc;
}

}  // namespace

RegistryClient::RegistryClient(CredentialStorePtr creds,
                               RegistryClientConfig cfg)
    : creds_(std::move(creds)),
      cfg_(std::move(cfg)),
      http_(make_http_config(cfg_)) {}

elio::coro::task<std::optional<elio::http::response>>
RegistryClient::request_range(const std::string& url, uint64_t first,
                              uint64_t last, const std::string& auth_header) {
    auto parsed = elio::http::url::parse(url);
    if (!parsed) {
        errno = EINVAL;
        co_return std::nullopt;
    }
    elio::http::request req(elio::http::method::GET, parsed->path_with_query());
    req.set_host(parsed->host_authority());
    req.set_header("Accept", "*/*");
    req.set_header("Range", make_range_header(first, last));
    if (!auth_header.empty()) {
        req.set_header("Authorization", auth_header);
    }
    errno = 0;
    co_return co_await http_.send(req, *parsed);
}

elio::coro::task<std::string> RegistryClient::fetch_token(
    const std::string& realm, const std::string& service,
    const std::string& scope, const std::string& url_for_creds) {
    std::string token_url = realm;
    token_url += token_url.find('?') == std::string::npos ? '?' : '&';
    if (!service.empty()) token_url += "service=" + service;
    if (!scope.empty()) {
        if (!service.empty()) token_url += "&";
        token_url += "scope=" + scope;
    }

    std::string auth;
    if (creds_) {
        if (const auto cred = creds_->find(url_for_creds)) {
            auth = "Basic " + base64_encode(cred->username + ":" +
                                            cred->password);
        }
    }
    errno = 0;
    auto resp = co_await request_range(token_url, 0, 0, auth);
    if (!resp) {
        throw_errno(errno ? errno : EIO, "token request failed: " + realm);
    }
    if (resp->status_code() != 200) {
        // Token endpoints ignore Range; a 200 with full body is expected.
        throw_errno(status_to_errno(resp->status_code()),
                    "token request rejected (HTTP " +
                        std::to_string(resp->status_code()) + "): " + realm);
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(resp->body());
    } catch (const nlohmann::json::exception& e) {
        throw format_error("malformed token response: " +
                           std::string(e.what()));
    }
    std::string token;
    if (j.contains("token")) token = j["token"].get<std::string>();
    else if (j.contains("access_token"))
        token = j["access_token"].get<std::string>();
    if (token.empty()) {
        throw format_error("token response has no token field: " + realm);
    }
    co_return token;
}

elio::coro::task<RegistryClient::UrlInfo> RegistryClient::resolve(
    const std::string& url) {
    {
        co_await mu_.lock();
        auto it = url_infos_.find(url);
        const bool hit = it != url_infos_.end() &&
                         it->second.expiry > std::chrono::steady_clock::now();
        UrlInfo cached;
        if (hit) cached = it->second;
        mu_.unlock();
        if (hit) co_return cached;
    }

    // Probe: Range 0-0, no auth (registryfs_v2 get_scope_auth semantics).
    errno = 0;
    auto probe = co_await request_range(url, 0, 0, "");
    if (!probe) {
        throw_errno(errno ? errno : EIO, "registry probe failed: " + url);
    }
    const int status = probe->status_code();
    const auto location = probe->get_headers().get("Location");

    UrlInfo info;
    if (status >= 300 && status < 400 && !location.empty()) {
        // Anonymous CDN redirect: no auth needed at all.
        info.final_url = std::string(location);
        info.expiry = std::chrono::steady_clock::now() +
                      std::chrono::seconds(300);
    } else if (status == 200 || status == 206) {
        info.final_url = url;
        info.expiry = std::chrono::steady_clock::now() +
                      std::chrono::seconds(300);
    } else if (status == 401 || status == 403) {
        const auto challenge_hdr =
            probe->get_headers().get("WWW-Authenticate");
        if (challenge_hdr.empty()) {
            throw_errno(EPERM, "registry denied without challenge: " + url);
        }
        const auto challenge = parse_challenge(challenge_hdr);
        if (!challenge) {
            throw_errno(EPERM, "unsupported auth challenge " +
                                   std::string(challenge_hdr));
        }
        std::string auth;
        if (challenge->bearer) {
            const std::string key = challenge->realm + "|" +
                                    challenge->service + "|" +
                                    challenge->scope;
            std::string token;
            {
                co_await mu_.lock();
                auto it = tokens_.find(key);
                const bool hit =
                    it != tokens_.end() &&
                    it->second.expiry > std::chrono::steady_clock::now();
                if (hit) token = it->second.token;
                mu_.unlock();
            }
            if (token.empty()) {
                token = co_await fetch_token(challenge->realm,
                                             challenge->service,
                                             challenge->scope, url);
                co_await mu_.lock();
                tokens_[key] = {token, std::chrono::steady_clock::now() +
                                           std::chrono::seconds(30)};
                mu_.unlock();
            }
            auth = "Bearer " + token;
        } else {
            // Basic auth: credentials from the store for the blob URL.
            if (creds_) {
                if (const auto cred = creds_->find(url)) {
                    auth = "Basic " + base64_encode(cred->username + ":" +
                                                    cred->password);
                }
            }
            if (auth.empty()) {
                throw_errno(EPERM, "no credentials for " + url);
            }
        }
        // Re-issue the probe with auth to discover Self vs Redirect mode
        // (registryfs_v2 get_actual_url).
        errno = 0;
        auto probe2 = co_await request_range(url, 0, 0, auth);
        if (!probe2) {
            throw_errno(errno ? errno : EIO,
                        "registry probe with auth failed: " + url);
        }
        const auto location2 = probe2->get_headers().get("Location");
        const int status2 = probe2->status_code();
        if (status2 >= 300 && status2 < 400 && !location2.empty()) {
            info.final_url = std::string(location2);
            // Redirect mode: CDN URL needs no auth.
        } else if (status2 == 200 || status2 == 206) {
            info.final_url = url;
            info.auth_header = auth;  // Self mode: auth on every request.
        } else {
            throw_errno(status_to_errno(status2),
                        "registry probe with auth rejected (HTTP " +
                            std::to_string(status2) + "): " + url);
        }
        info.expiry = std::chrono::steady_clock::now() +
                      std::chrono::seconds(300);
    } else {
        throw_errno(status_to_errno(status),
                    "registry probe rejected (HTTP " + std::to_string(status) +
                        "): " + url);
    }

    co_await mu_.lock();
    url_infos_[url] = info;
    mu_.unlock();
    co_return info;
}

elio::coro::task<ssize_t> RegistryClient::get_data(const std::string& url,
                                                   void* buf, uint64_t offset,
                                                   size_t count) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        UrlInfo info;
        try {
            info = co_await resolve(url);
        } catch (const std::system_error& e) {
            co_return -e.code().value();
        }
        std::string request_url = info.final_url;
        if (!cfg_.accelerate_base.empty()) {
            // DART prefix passthrough (ADR-0005): prefix + "/" + actual URL.
            request_url = cfg_.accelerate_base + "/" + info.final_url;
        }
        errno = 0;
        auto resp = co_await request_range(request_url, offset,
                                           offset + count - 1,
                                           info.auth_header);
        if (!resp) {
            const int e = errno ? errno : EIO;
            if (attempt == 2) co_return -e;
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const int status = resp->status_code();
        if (status == 206) {
            const auto cr_hdr = resp->get_headers().get("Content-Range");
            const auto cr = parse_content_range(cr_hdr);
            if (!cr || cr->first != offset) {
                if (attempt == 2) co_return -EIO;
                co_await mu_.lock();
                url_infos_.erase(url);
                mu_.unlock();
                continue;
            }
            const auto& body = resp->body();
            if (body.size() < count) co_return -EIO;
            std::memcpy(buf, body.data(), count);
            co_return static_cast<ssize_t>(count);
        }
        if (status == 200) {
            // Server ignored Range: only valid for a full-blob read at 0.
            const auto& body = resp->body();
            if (offset == 0 && body.size() >= count) {
                std::memcpy(buf, body.data(), count);
                co_return static_cast<ssize_t>(count);
            }
            if (attempt == 2) co_return -EIO;
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (status == 401 || status == 403) {
            // Stale redirect/token: drop cached info and re-resolve.
            co_await mu_.lock();
            url_infos_.erase(url);
            mu_.unlock();
            if (attempt == 2) co_return -EPERM;
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (status == 416) co_return -ERANGE;
        if (status == 429) {
            if (attempt == 2) co_return -EBUSY;
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        co_return -status_to_errno(status);
    }
    co_return -ETIMEDOUT;
}

elio::coro::task<int64_t> RegistryClient::get_length(const std::string& url) {
    UrlInfo info;
    try {
        info = co_await resolve(url);
    } catch (const std::system_error& e) {
        co_return -e.code().value();
    }
    std::string request_url = info.final_url;
    if (!cfg_.accelerate_base.empty()) {
        request_url = cfg_.accelerate_base + "/" + info.final_url;
    }
    errno = 0;
    auto resp = co_await request_range(request_url, 0, 0, info.auth_header);
    if (!resp) co_return -(errno ? errno : EIO);
    const auto cr_hdr = resp->get_headers().get("Content-Range");
    const auto cr = parse_content_range(cr_hdr);
    if (resp->status_code() == 206 && cr && cr->total != UINT64_MAX) {
        co_return static_cast<int64_t>(cr->total);
    }
    if (resp->status_code() == 200) {
        // Server ignored Range and answered with the full blob.
        co_return static_cast<int64_t>(resp->body().size());
    }
    co_return -status_to_errno(resp->status_code());
}

elio::coro::task<std::unique_ptr<RegistrySource>> RegistrySource::open(
    RegistryClientPtr client, std::string url) {
    if (!client) throw error(EINVAL, "registry source with null client");
    const int64_t size = co_await client->get_length(url);
    if (size < 0) {
        throw_errno(static_cast<int>(-size),
                    "cannot determine blob size: " + url);
    }
    auto src = std::unique_ptr<RegistrySource>(new RegistrySource());
    src->client_ = std::move(client);
    src->url_ = url;
    src->size_ = static_cast<uint64_t>(size);
    src->label_ = url;
    co_return src;
}

elio::coro::task<ssize_t> RegistrySource::pread(void* buf, size_t count,
                                                uint64_t offset) {
    if (offset >= size_) co_return 0;
    if (count > size_ - offset) count = static_cast<size_t>(size_ - offset);
    if (count == 0) co_return 0;
    co_return co_await client_->get_data(url_, buf, offset, count);
}

}  // namespace obd::source
