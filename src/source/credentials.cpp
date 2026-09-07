// CredentialStore implementation. See credentials.hpp.
#include "source/credentials.hpp"

#include "common/errors.hpp"
#include "source/base64.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace obd::source {

CredentialStore CredentialStore::from_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw_errno(errno ? errno : ENOENT, "cannot open credential file " + path);
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    CredentialStore store;
    if (text.empty()) return store;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        throw format_error("malformed credential file " + path + ": " +
                           e.what());
    }
    const auto auths = j.find("auths");
    if (auths == j.end() || !auths->is_object()) return store;

    for (const auto& [key, entry] : auths->items()) {
        Credential cred;
        if (entry.contains("auth")) {
            // Docker-style pre-encoded "user:pass".
            const std::string decoded =
                base64_decode(entry["auth"].get<std::string>());
            const auto colon = decoded.find(':');
            cred.username = decoded.substr(0, colon);
            cred.password =
                colon == std::string::npos ? "" : decoded.substr(colon + 1);
        } else {
            if (entry.contains("username"))
                cred.username = entry["username"].get<std::string>();
            if (entry.contains("password"))
                cred.password = entry["password"].get<std::string>();
        }
        store.add(key, std::move(cred));
    }
    return store;
}

void CredentialStore::add(std::string key, Credential cred) {
    entries_.emplace_back(std::move(key), std::move(cred));
    // Longest key first: the find() scan takes the first prefix match.
    std::sort(entries_.begin(), entries_.end(),
              [](const auto& a, const auto& b) {
                  return a.first.size() > b.first.size();
              });
}

std::optional<Credential> CredentialStore::find(std::string_view url) const {
    // Longest-prefix match over both the full URL and the URL without its
    // "scheme://" prefix (auth keys commonly omit the scheme).
    std::string_view bare = url;
    if (const auto p = bare.find("://"); p != std::string_view::npos) {
        bare.remove_prefix(p + 3);
    }
    for (const auto& [key, cred] : entries_) {
        if (key.empty()) continue;
        if (url.starts_with(key) || bare.starts_with(key)) {
            return cred;
        }
    }
    return std::nullopt;
}

}  // namespace obd::source
