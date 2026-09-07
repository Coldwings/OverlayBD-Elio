// Configuration parsing — the overlaybd-compatible operator contract
// (docs/config.md; unknown fields are ignored, known fields keep their
// overlaybd meaning).
//
// Two files:
//   * GlobalConfig — the daemon-wide overlaybd.json: credentialConfig
//     (mode=file → credential file path), p2pConfig (DART proxy), download
//     defaults, logConfig.
//   * ImageConfig — the per-image config.json written by the
//     overlaybd-snapshotter: repoBlobUrl, lowers[], download overrides,
//     resultFile, and optionally `upper` — a writable layer (ADR-0008).
#pragma once

#include "source/downloader.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace obd::image {

struct GlobalConfig {
    /// credentialConfig: only mode=file is honored; inline/secret modes are
    /// ignored with a warning by the caller (docs/config.md).
    std::string credential_file;

    /// p2pConfig: DART proxy (ADR-0005).
    bool p2p_enable = false;
    std::string p2p_address;  // e.g. "localhost:19145/dart"

    /// Global download defaults (per-image download sections override).
    source::DownloadConfig download;

    /// logConfig.logLevel: 0=debug, 1=info, 2=warn, 3=error.
    int log_level = 1;

    /// Parses overlaybd.json. Throws obd::error on IO failure,
    /// obd::format_error on malformed JSON.
    static GlobalConfig from_file(const std::string& path);
    static GlobalConfig from_json_text(const std::string& text);
};

struct LowerConfig {
    std::string digest;  // "sha256:<hex>"
    uint64_t size = 0;
    std::string dir;     // per-layer directory (download cache)
    std::string file;    // local blob file when present ("" = remote)
};

/// Writable upper layer (ADR-0008). `dir` holds the layer file:
///   type "lsmt"   → <dir>/overlaybd.rw    (in-place-edit LSMT, default)
///   type "sparse" → <dir>/overlaybd.sparse (fiemap sparse file)
struct UpperConfig {
    std::string dir;
    std::string type = "lsmt";  // "lsmt" | "sparse"
};

struct ImageConfig {
    std::string repo_blob_url;
    std::vector<LowerConfig> lowers;  // bottom-up: lowers[0] = base layer
    std::string result_file;          // informational in v0.1 (docs/config.md)
    source::DownloadConfig download;  // merged over the global defaults

    /// Writable upper; engaged when `upper.dir` is non-empty (ADR-0008).
    UpperConfig upper;
    bool writable() const noexcept { return !upper.dir.empty(); }

    /// The digest's hex payload ("sha256:" prefix stripped) — the download
    /// integrity check value. Empty when the digest has another algorithm.
    static std::string digest_sha256_hex(const std::string& digest);

    /// Parses the per-image config.json; `defaults` are the global download
    /// settings overridden per-field by the image's download section.
    /// Throws obd::error on IO failure, obd::format_error on malformed
    /// JSON, obd::error(EINVAL) on an unknown `upper.type` (ADR-0008).
    static ImageConfig from_file(const std::string& path,
                                 const source::DownloadConfig& defaults);
    static ImageConfig from_json_text(const std::string& text,
                                      const source::DownloadConfig& defaults);
};

}  // namespace obd::image
