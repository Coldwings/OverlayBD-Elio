// CredentialStore — registry credentials from the overlaybd-compatible
// credential file (docs/config.md §credentialConfig).
//
// File shape (overlaybd credentialConfig mode=file):
//
//   { "auths": {
//       "registry.example.com": { "username": "u", "password": "p" },
//       "auth.example.com/limited": { "auth": "<base64 user:pass>" } } }
//
// Lookup is a longest-prefix match over the full blob URL (overlaybd
// image_service.cpp credential matching semantics).
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace obd::source {

struct Credential {
    std::string username;
    std::string password;
};

class CredentialStore {
public:
    /// Parses a credential file; empty content yields an empty store.
    /// Throws obd::error on IO failures, obd::format_error on malformed
    /// JSON.
    static CredentialStore from_file(const std::string& path);

    /// Empty store (no credentials).
    CredentialStore() = default;

    /// Longest-prefix match: returns the credential whose auths key is a
    /// prefix of `url`, preferring the longest key. Keys without a scheme
    /// also match "<scheme>://" prefixed URLs by their host part (overlaybd
    /// accepts both forms).
    std::optional<Credential> find(std::string_view url) const;

    bool empty() const noexcept { return entries_.empty(); }
    size_t size() const noexcept { return entries_.size(); }

    /// Adds an entry directly (tests).
    void add(std::string key, Credential cred);

private:
    // Kept sorted by key length descending for longest-prefix-first scans.
    std::vector<std::pair<std::string, Credential>> entries_;
};

using CredentialStorePtr = std::shared_ptr<const CredentialStore>;

}  // namespace obd::source
