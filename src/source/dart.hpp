// DART proxy integration (docs/source.md §DART, ADR-0005).
//
// DART (data-accelerator/dart) is an external, per-node read-only P2P cache
// speaking the overlaybd prefix-passthrough convention:
//
//   GET http://<dart-host>:<port>/<prefix>/<full upstream URL incl. scheme>
//
// e.g. with address "localhost:19145/dart" the blob
// "https://registry.example.com/v2/lib/nginx/blobs/sha256:abc" is fetched as
// "http://localhost:19145/dart/https://registry.example.com/v2/lib/nginx/
// blobs/sha256:abc". DART answers Range GETs with 200/206/416, always with
// an explicit Content-Length (docs/dart.md in the DART repo).
//
// This module owns address parsing and the reachability probe; the request
// URL rewriting itself lives in RegistryClient (same layering as overlaybd
// registryfs + p2pConfig).
#pragma once

#include <elio/coro/task.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace obd::source {

/// Parsed DART proxy address. `address` in the overlaybd p2pConfig is
// "<host>:<port>/<prefix>" or "<scheme>://<host>:<port>/<prefix>"; DART's
// default prefix is "dart".
struct DartProxyAddress {
    std::string host;
    uint16_t port = 0;
    std::string prefix;  // e.g. "/dart"
    /// Absolute base used for URL rewriting: "http://host:port/prefix".
    std::string base;
};

/// Parses an overlaybd p2pConfig.address. Returns std::nullopt on malformed
// input (missing host/port, bad port). Scheme is accepted but normalized to
// http (DART listens on plain HTTP; docs/dart.md).
std::optional<DartProxyAddress> parse_dart_address(std::string_view address);

/// Builds the prefix-passthrough request URL for an actual blob URL:
/// address.base + "/" + actual_url. The double slashes of the embedded
/// upstream scheme are preserved verbatim (DART parses RequestURI, not
/// normalized URL.Path — see DART docs/dart.md §prefix).
std::string dart_prefixed_url(const DartProxyAddress& address,
                              const std::string& actual_url);

/// Reachability probe: resolves the proxy host and attempts a TCP connect.
/// Mirrors overlaybd's check_accelerate_url(): when the probe fails, the
/// caller falls back to fetching directly from the registry (acceleration
/// disabled) instead of failing the device.
elio::coro::task<bool> dart_proxy_reachable(const DartProxyAddress& address);

}  // namespace obd::source
