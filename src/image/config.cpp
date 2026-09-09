// Configuration parsing. See config.hpp.
#include "image/config.hpp"

#include "common/errors.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <limits>
#include <sstream>

namespace obd::image {

namespace {

std::string read_text_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw_errno(errno ? errno : ENOENT, "cannot open config file " + path);
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

nlohmann::json parse_json(const std::string& text, const std::string& what) {
    try {
        return nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        throw format_error("malformed " + what + ": " + e.what());
    }
}

/// Applies a JSON "download" object over `base`; only present fields
/// override. overlaybd field names: enable, delay, delayExtra, maxMBps,
/// tryCnt, blockSize.
void apply_download_json(const nlohmann::json& j,
                         DownloadConfig& base) {
    if (!j.is_object()) return;
    if (j.contains("enable")) base.enable = j["enable"].get<bool>();
    if (j.contains("delay")) base.delay_sec = j["delay"].get<uint32_t>();
    if (j.contains("delayExtra"))
        base.delay_extra_sec = j["delayExtra"].get<uint32_t>();
    if (j.contains("maxMBps")) base.max_mbps = j["maxMBps"].get<uint32_t>();
    if (j.contains("tryCnt")) base.try_count = j["tryCnt"].get<uint32_t>();
    if (j.contains("blockSize"))
        base.block_size = j["blockSize"].get<uint32_t>();
}

/// prefetch.head_kb / prefetch.tail_kb: one structural warm-up window
/// size in KiB (default 1024 when the key is absent). A negative value
/// would wrap to ~4 TiB through the uint32 conversion and silently warm
/// every layer whole at each bring-up, so out-of-range values are
/// rejected fail-loud — structural config errors fail loud (the same
/// ruling as the malformed lower digest, ADR-0016's boundary).
uint32_t prefetch_window_kb(const nlohmann::json& j, const char* key) {
    if (!j.contains(key)) return 1024;
    const int64_t v = j[key].get<int64_t>();
    if (v < 0 || v > std::numeric_limits<uint32_t>::max()) {
        throw error(EINVAL,
                    std::string("prefetch.") + key + " out of range: " +
                        std::to_string(v) + " (want 0.." +
                        std::to_string(
                            std::numeric_limits<uint32_t>::max()) +
                        ")");
    }
    return static_cast<uint32_t>(v);
}

}  // namespace

GlobalConfig GlobalConfig::from_file(const std::string& path) {
    return from_json_text(read_text_file(path));
}

GlobalConfig GlobalConfig::from_json_text(const std::string& text) {
    GlobalConfig cfg;
    const auto j = parse_json(text, "global config");

    if (const auto it = j.find("credentialConfig");
        it != j.end() && it->is_object()) {
        // overlaybd: mode=file → read from the given file (default
        // /opt/overlaybd/cred.json). Only mode=file is honored.
        const std::string mode = it->value("mode", "file");
        if (mode == "file") {
            cfg.credential_file =
                it->value("path", std::string("/opt/overlaybd/cred.json"));
        }
    }
    if (const auto it = j.find("p2pConfig"); it != j.end() && it->is_object()) {
        cfg.p2p_enable = it->value("enable", false);
        cfg.p2p_address = it->value("address", "");
    }
    if (const auto it = j.find("download"); it != j.end()) {
        apply_download_json(*it, cfg.download);
    }
    if (const auto it = j.find("ublkConfig"); it != j.end() && it->is_object()) {
        cfg.ublk_recovery = it->value("enableRecovery", true);
    }
    if (const auto it = j.find("logConfig"); it != j.end() && it->is_object()) {
        cfg.log_level = it->value("logLevel", 1);
    }
    if (const auto it = j.find("prefetch"); it != j.end() && it->is_object()) {
        // ADR-0012/0013: the honored subset of the upstream prefetch
        // section is the master switch plus the structural head/tail
        // window sizes — the admission funnel's AIMD window is
        // deliberately not operator-configured.
        cfg.prefetch_enable = it->value("enable", true);
        cfg.prefetch_head_kb = prefetch_window_kb(*it, "head_kb");
        cfg.prefetch_tail_kb = prefetch_window_kb(*it, "tail_kb");
    }
    // cacheConfig / ioEngine: intentionally not honored in v0.1
    // (docs/config.md).
    return cfg;
}

std::string ImageConfig::digest_sha256_hex(const std::string& digest) {
    constexpr std::string_view kPrefix = "sha256:";
    if (digest.starts_with(kPrefix)) {
        return digest.substr(kPrefix.size());
    }
    return "";
}

ImageConfig ImageConfig::from_file(const std::string& path,
                                   const DownloadConfig& defaults) {
    return from_json_text(read_text_file(path), defaults);
}

ImageConfig ImageConfig::from_json_text(const std::string& text,
                                        const DownloadConfig& defaults) {
    const auto j = parse_json(text, "image config");
    ImageConfig cfg;
    cfg.download = defaults;

    cfg.repo_blob_url = j.value("repoBlobUrl", "");
    if (const auto it = j.find("lowers"); it != j.end() && it->is_array()) {
        for (const auto& l : *it) {
            LowerConfig lower;
            lower.digest = l.value("digest", "");
            lower.size = l.value("size", 0ULL);
            lower.dir = l.value("dir", "");
            lower.file = l.value("file", "");
            cfg.lowers.push_back(std::move(lower));
        }
    }
    if (const auto it = j.find("upper"); it != j.end() && it->is_object() &&
        !it->empty()) {
        // ADR-0008: a writable upper engages the writable device mode.
        cfg.upper.dir = it->value("dir", "");
        cfg.upper.type = it->value("type", "lsmt");
        if (cfg.upper.type != "lsmt" && cfg.upper.type != "sparse") {
            throw error(EINVAL, "unknown upper.type '" + cfg.upper.type +
                                    "' (want lsmt|sparse, ADR-0008)");
        }
    }
    cfg.result_file = j.value("resultFile", "");
    // ADR-0013: the upstream backstore config field marking the uppermost
    // lower as the acceleration (trace) layer (trace-format.md §6).
    cfg.acceleration_layer = j.value("accelerationLayer", false);
    if (const auto it = j.find("download"); it != j.end()) {
        apply_download_json(*it, cfg.download);
    }
    return cfg;
}

}  // namespace obd::image
