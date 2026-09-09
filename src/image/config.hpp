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

#include <cstdint>
#include <string>
#include <vector>

namespace obd::image {

/// The overlaybd `download` section (schema contract, docs/config.md):
/// global defaults in overlaybd.json, per-field overrides per image.
/// Under ADR-0011 these knobs drive the LayerStore background fill:
/// enable starts the fill; delay/delayExtra the start delay (+ jitter);
/// maxMBps the throughput throttle; blockSize the range-read coalescing
/// cap; tryCnt bounds completion-verify restarts.
struct DownloadConfig {
    bool enable = false;
    uint32_t delay_sec = 300;        // start delay after device open
    uint32_t delay_extra_sec = 30;   // + random(0, extra)
    uint32_t max_mbps = 100;         // throttle, MiB/s
    uint32_t try_count = 5;
    uint32_t block_size = 256 * 1024;
};

struct GlobalConfig {
    /// credentialConfig: only mode=file is honored; inline/secret modes are
    /// ignored with a warning by the caller (docs/config.md).
    std::string credential_file;

    /// p2pConfig: DART proxy (ADR-0005).
    bool p2p_enable = false;
    std::string p2p_address;  // e.g. "localhost:19145/dart"

    /// Global download defaults (per-image download sections override).
    DownloadConfig download;

    /// prefetch: trace-replay warm-up master switch (ADR-0012/0013).
    /// When false, an acceleration layer's trace blob is neither loaded
    /// nor replayed. The funnel's AIMD window is NOT operator-configured
    /// (ADR-0012); this is the only honored field of the section.
    bool prefetch_enable = true;

    /// logConfig.logLevel: 0=debug, 1=info, 2=warn, 3=error.
    int log_level = 1;

    /// ublkConfig.enableRecovery (ADR-0010): create devices with
    /// UBLK_F_USER_RECOVERY so a crashed device process can be replaced
    /// without failing the block device. Default on.
    bool ublk_recovery = true;

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
    DownloadConfig download;  // merged over the global defaults

    /// accelerationLayer (ADR-0013): the snapshotter's signal
    /// that the UPPERMOST lower is the acceleration (trace) layer, not a
    /// data layer (trace-format.md §6 — upstream backstore config.v1.json
    /// carries the same field). Assembly sets that lower aside from the
    /// merge and replays its trace blob.
    bool acceleration_layer = false;

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
                                 const DownloadConfig& defaults);
    static ImageConfig from_json_text(const std::string& text,
                                      const DownloadConfig& defaults);
};

}  // namespace obd::image
