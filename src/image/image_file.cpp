// Image assembly. See image_file.hpp for the stack diagram.
#include "image/image_file.hpp"

#include "common/errors.hpp"
#include "format/lsmt.hpp"
#include "format/lsmt_rw.hpp"
#include "format/merged_writable.hpp"
#include "format/sparse_rw.hpp"
#include "format/zfile.hpp"
#include "source/chunk_cache.hpp"
#include "source/dart.hpp"
#include "source/layer_store.hpp"
#include "source/local_file.hpp"
#include "source/registry.hpp"
#include "source/tar_offset.hpp"

#include <elio/log/macros.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <filesystem>

namespace obd::image {

namespace {

bool is_regular_file(const std::string& path) {
    struct stat st {};
    return !path.empty() && ::stat(path.c_str(), &st) == 0 &&
           S_ISREG(st.st_mode);
}

/// overlaybd's per-lower local probe: an explicit layer file first, then
/// the commit markers inside the layer directory.
std::string probe_local_blob(const LowerConfig& lower) {
    if (is_regular_file(lower.file)) return lower.file;
    if (!lower.dir.empty()) {
        for (const char* name :
             {"overlaybd.commit", ".commit", "overlaybd.sealed"}) {
            const std::string p = lower.dir + "/" + name;
            if (is_regular_file(p)) return p;
        }
    }
    return "";
}

}  // namespace

elio::coro::task<OpenedImage> open_image(const ImageConfig& cfg,
                                         const GlobalConfig& global) {
    if (cfg.lowers.empty()) {
        throw error(EINVAL, "image config has no lowers");
    }

    // Credentials: a missing credential file is not fatal (anonymous pull).
    source::CredentialStorePtr creds =
        std::make_shared<source::CredentialStore>();
    if (!global.credential_file.empty()) {
        try {
            creds = std::make_shared<source::CredentialStore>(
                source::CredentialStore::from_file(global.credential_file));
        } catch (const std::system_error& e) {
            ELIO_LOG_WARNING("credential file {} unavailable ({}); pulling "
                          "anonymously",
                          global.credential_file, e.code().message());
        }
    }

    // DART accelerate prefix (ADR-0005): enabled only when the proxy is
    // reachable; otherwise we fall back to direct registry reads.
    source::RegistryClientConfig rcc;
    if (global.p2p_enable && !global.p2p_address.empty()) {
        if (const auto addr = source::parse_dart_address(global.p2p_address)) {
            if (co_await source::dart_proxy_reachable(*addr)) {
                rcc.accelerate_base = addr->base;
                ELIO_LOG_INFO("DART acceleration via {}", addr->base);
            } else {
                ELIO_LOG_WARNING("DART proxy {} unreachable; using direct "
                              "registry access",
                              global.p2p_address);
            }
        } else {
            ELIO_LOG_WARNING("malformed p2pConfig.address '{}'",
                          global.p2p_address);
        }
    }
    auto client = std::make_shared<source::RegistryClient>(creds, rcc);

    std::vector<std::unique_ptr<format::LsmtLayer>> layers;
    layers.reserve(cfg.lowers.size());
    for (const auto& lower : cfg.lowers) {
        source::BlobSourcePtr raw;
        const std::string local_path = probe_local_blob(lower);
        if (!local_path.empty()) {
            ELIO_LOG_INFO("layer {} from local file {}", lower.digest,
                          local_path);
            raw = co_await source::LocalFileSource::open(local_path);
        } else {
            if (cfg.repo_blob_url.empty()) {
                throw error(EINVAL, "no local blob and no repoBlobUrl for " +
                                        lower.digest);
            }
            const std::string url = cfg.repo_blob_url + "/" + lower.digest;
            auto reg = co_await source::RegistrySource::open(client, url);
            if (lower.dir.empty()) {
                // No persistence directory configured: a LayerStore needs
                // a writable per-layer dir to stage into, so the layer
                // keeps the legacy in-memory ChunkCache in front of the
                // registry (restart-cold; retired with part 3).
                ELIO_LOG_WARNING("layer {} has no dir; serving remotely "
                                 "with in-memory caching only",
                                 lower.digest);
                raw = co_await source::ChunkCache::open(std::move(reg));
            } else {
                // ADR-0011: one remote source per layer; the LayerStore
                // reads through it and persists every served extent into
                // the per-layer dir (staging pair, renamed to
                // overlaybd.commit on completion).
                std::error_code ec;
                std::filesystem::create_directories(lower.dir, ec);
                raw = co_await source::LayerStore::open(
                    std::move(reg), lower.dir,
                    ImageConfig::digest_sha256_hex(lower.digest));
            }
        }

        auto untarred = co_await source::TarOffsetSource::open(std::move(raw));
        source::BlobSourcePtr view;
        if (co_await format::is_zfile(*untarred)) {
            view = co_await format::ZFileSource::open(std::move(untarred),
                                                      /*caller_verify=*/true);
        } else {
            view = std::move(untarred);
        }
        layers.push_back(
            co_await format::LsmtLayer::open(std::move(view)));
    }

    const size_t n = layers.size();
    if (!cfg.writable()) {
        auto merged = co_await format::MergedLsmt::open(std::move(layers));
        OpenedImage out;
        out.virtual_size = merged->size();
        out.layer_count = n;
        out.root = std::move(merged);
        ELIO_LOG_INFO("image assembled: {} layers, virtual size {} bytes", n,
                      out.virtual_size);
        co_return out;
    }

    // Writable upper (ADR-0008): the device view is the lowers merged with
    // a writable top layer sized to cover the whole image.
    uint64_t vsize = 0;
    for (const auto& l : layers) vsize = std::max(vsize, l->virtual_size());
    std::filesystem::create_directories(cfg.upper.dir);
    std::unique_ptr<format::WritableLayer> top;
    std::string upper_path;
    if (cfg.upper.type == "sparse") {
        upper_path = cfg.upper.dir + "/overlaybd.sparse";
        top = co_await format::SparseRwLayer::open(upper_path, vsize);
    } else {
        upper_path = cfg.upper.dir + "/overlaybd.rw";
        top = co_await format::LsmtRwLayer::create(upper_path, vsize);
    }
    auto merged = co_await format::MergedWritable::open(std::move(layers),
                                                        std::move(top));
    OpenedImage out;
    out.virtual_size = merged->size();
    out.layer_count = n + 1;
    out.writable = true;
    out.upper_path = upper_path;
    out.root = std::move(merged);
    ELIO_LOG_INFO("image assembled writable: {} lowers + {} upper, virtual "
                  "size {} bytes",
                  n, cfg.upper.type, out.virtual_size);
    co_return out;
}

}  // namespace obd::image
