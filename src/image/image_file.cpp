// Image assembly. See image_file.hpp for the stack diagram.
#include "image/image_file.hpp"

#include "common/errors.hpp"
#include "image/trace_record.hpp"
#include "format/lsmt.hpp"
#include "format/lsmt_rw.hpp"
#include "format/merged_writable.hpp"
#include "format/sparse_rw.hpp"
#include "format/zfile.hpp"
#include "source/admission.hpp"
#include "source/dart.hpp"
#include "source/layer_store.hpp"
#include "source/local_file.hpp"
#include "source/registry.hpp"
#include "source/tar_offset.hpp"

#include <elio/log/macros.hpp>
#include <elio/time/timer.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
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

/// Read cap for the trace blob itself: a conforming trace is small
/// (24 + 24 × records; the replay record cap of 65536 needs ~1.5 MiB), so
/// 64 MiB is generous while bounding a hostile layer's memory/IO cost.
constexpr uint64_t kMaxTraceBlobBytes = uint64_t{64} << 20;

/// Reads a whole (small) source into memory; empty vector on failure or
/// when the source exceeds kMaxTraceBlobBytes (callers treat empty as
/// "no usable trace").
elio::coro::task<std::vector<uint8_t>> read_trace_blob(
    source::BlobSource& src) {
    if (src.size() > kMaxTraceBlobBytes) {
        ELIO_LOG_WARNING("trace layer blob too large ({} bytes, cap {}); "
                         "prefetch disabled",
                         src.size(), kMaxTraceBlobBytes);
        co_return std::vector<uint8_t>{};
    }
    std::vector<uint8_t> out(static_cast<size_t>(src.size()));
    if (out.empty()) co_return out;
    const ssize_t r = co_await src.pread(out.data(), out.size(), 0);
    if (r < 0 || static_cast<uint64_t>(r) != src.size()) {
        ELIO_LOG_WARNING("trace layer blob unreadable; prefetch disabled");
        co_return std::vector<uint8_t>{};
    }
    co_return out;
}

/// Best-effort load of the acceleration layer's trace blob
/// (trace-format.md §6): locally the extracted member `<dir>/trace` (the
/// upstream lookup name) or an explicit `file`; remotely the layer blob
/// through the tar wrapper (a plain RegistrySource — the blob is small
/// and needs no LayerStore persistence — behind an AdmissionSource so
/// the fetch passes the device's funnel, ADR-0012; it rides the
/// bring-up critical path, hence OnDemand). NEVER fails assembly: every
/// error path logs and returns an empty vector.
elio::coro::task<std::vector<uint8_t>> load_trace_blob(
    const LowerConfig& accel, const ImageConfig& cfg,
    const std::shared_ptr<source::RegistryClient>& client,
    const source::AdmissionFunnelPtr& funnel) {
    try {
        std::string local;
        if (is_regular_file(accel.file)) local = accel.file;
        if (local.empty() && !accel.dir.empty()) {
            const std::string p = accel.dir + "/trace";
            if (is_regular_file(p)) local = p;
        }
        if (!local.empty()) {
            auto src = co_await source::LocalFileSource::open(local);
            co_return co_await read_trace_blob(*src);
        }
        if (cfg.repo_blob_url.empty()) {
            ELIO_LOG_WARNING("acceleration layer has no local trace and no "
                             "repoBlobUrl; prefetch disabled");
            co_return std::vector<uint8_t>{};
        }
        const std::string url = cfg.repo_blob_url + "/" + accel.digest;
        auto reg = co_await source::RegistrySource::open(client, url);
        auto gated = std::make_unique<source::AdmissionSource>(
            std::move(reg), funnel);
        auto untarred =
            co_await source::TarOffsetSource::open(std::move(gated));
        co_return co_await read_trace_blob(*untarred);
    } catch (const std::exception& e) {
        ELIO_LOG_WARNING("trace layer load failed ({}); prefetch disabled",
                         e.what());
        co_return std::vector<uint8_t>{};
    }
}

/// How long one warm-up or replay (Prefetch-class populate) extent fetch
/// may wait at the admission funnel's gate before the extent — and with
/// it the warm-up window / replay record — is skipped (issue #35). Long
/// enough to ride out a transient on-demand burst or one fill fetch
/// cycle; short enough that a storm-closed gate degrades warm-up to
/// skips well inside the 30 s wall budget (the budget check between
/// populate slices stops the pass regardless). Not operator-configured
/// (ADR-0012's small prefetch surface); the background fill keeps its
/// unbounded scavenger wait.
constexpr std::chrono::milliseconds kWarmupAdmitTimeout{2000};

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

    // The read admission funnel (ADR-0012): ONE instance per device
    // open, shared by every lower's LayerStore and by the remote-only
    // source wrappers below, so all remote reads of the device compete
    // at a single gate. Its AIMD window is internal (not
    // operator-configured).
    auto funnel = std::make_shared<source::AdmissionFunnel>();

    // Trace layer recognition (ADR-0013; trace-format.md §6):
    // `accelerationLayer: true` marks the UPPERMOST lower as the
    // acceleration layer. It is NOT a data layer: set it aside from the
    // merge. Recognition is structural and always applies — but the
    // trace blob itself is loaded only AFTER the structural warm-up
    // below (ADR-0012's floor runs first: a slow or unhealthy trace
    // layer must not delay it; the load's only time bound is the
    // registry client's connect/read timeouts and retries, which sits
    // outside both warm-up budgets).
    std::span<const LowerConfig> data_lowers(cfg.lowers);
    if (cfg.acceleration_layer) {
        if (cfg.lowers.size() < 2) {
            throw error(EINVAL, "accelerationLayer set but the image has no "
                                "data lower beneath it");
        }
        data_lowers = data_lowers.first(cfg.lowers.size() - 1);
    }

    std::vector<std::unique_ptr<format::LsmtLayer>> layers;
    layers.reserve(data_lowers.size());
    // Stored-blob-level source of each data lower (the TarOffsetSource
    // view — the byte space trace record offsets address, matching
    // upstream's PrefetchFile position below decompression). Non-owning;
    // the objects are owned by the layer chain built below.
    std::vector<source::BlobSource*> warm_targets;
    warm_targets.reserve(data_lowers.size());
    std::vector<source::LayerStore*> stores;
    // The trace recorder (ADR-0013, record path): created idle; the
    // supervisor's trace_start arms it. Every REMOTE lower's registry
    // source is wrapped in a record tap (trace_record.hpp — a pread on
    // that source IS a remote read by construction, so local LayerStore
    // hits record nothing). Local lowers get no tap but still occupy
    // their layer_index slot.
    auto recorder = std::make_shared<TraceRecorder>();
    for (size_t layer_index = 0; layer_index < data_lowers.size();
         ++layer_index) {
        const auto& lower = data_lowers[layer_index];
        source::BlobSourcePtr raw;
        TraceRecordSource* tap = nullptr;
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
            // ADR-0016 boundary: the degrade path below is for environment
            // failures (an unusable layer dir) only — structural config
            // errors fail loud. A malformed layer digest is a config
            // error, so it is validated here, before any registry I/O and
            // outside the degrade handler. (A digest without the
            // "sha256:" prefix normalizes to empty = no verification —
            // that form is allowed.)
            const std::string sha =
                ImageConfig::digest_sha256_hex(lower.digest);
            if (!sha.empty() &&
                (sha.size() != 64 ||
                 !std::all_of(sha.begin(), sha.end(), [](char c) {
                     return std::isxdigit(static_cast<unsigned char>(c)) !=
                            0;
                 }))) {
                throw error(EINVAL, "malformed sha256 digest: " +
                                        lower.digest);
            }
            const std::string url = cfg.repo_blob_url + "/" + lower.digest;
            auto reg = co_await source::RegistrySource::open(client, url);
            auto tapped = std::make_unique<TraceRecordSource>(
                std::move(reg), recorder,
                static_cast<uint32_t>(layer_index));
            tap = tapped.get();
            if (lower.dir.empty()) {
                // No persistence directory configured: a LayerStore needs
                // a writable per-layer dir to stage into, so the layer is
                // served remote-only (no local caching at all; the kernel
                // page cache has nothing to work on). The snapshotter
                // always sets dir, so this is the compatibility path.
                // The AdmissionSource routes its reads through the
                // device's funnel (ADR-0012) like every other remote
                // path.
                ELIO_LOG_WARNING("layer {} has no dir; serving remotely "
                                 "without persistence",
                                 lower.digest);
                raw = std::make_unique<source::AdmissionSource>(
                    std::move(tapped), funnel);
            } else {
                // ADR-0011: one remote source per layer; the LayerStore
                // reads through it and persists every served extent into
                // the per-layer dir (staging pair, renamed to
                // overlaybd.commit on completion). The download section
                // drives the background fill (enable/delay/throttle) and
                // the completion-verify retry bound (tryCnt).
                std::error_code ec;
                std::filesystem::create_directories(lower.dir, ec);
                source::LayerStore::Config lsc;
                lsc.try_count = cfg.download.try_count;
                lsc.fill.enable = cfg.download.enable;
                lsc.fill.delay_sec = cfg.download.delay_sec;
                lsc.fill.delay_extra_sec = cfg.download.delay_extra_sec;
                lsc.fill.max_mbps = cfg.download.max_mbps;
                lsc.fill.block_size = cfg.download.block_size;
                lsc.funnel = funnel;  // ADR-0012: shared across all lowers
                // Issue #35: warm-up's populate fetches may not wait at
                // a storm-closed funnel gate longer than this — the
                // window is skipped instead of awaited (bring-up can
                // never stall behind scavenger admission).
                lsc.populate_admit_timeout = kWarmupAdmitTimeout;
                bool store_opened = false;
                try {
                    raw = co_await source::LayerStore::open(
                        std::move(tapped), lower.dir, sha, std::move(lsc));
                    stores.push_back(
                        static_cast<source::LayerStore*>(raw.get()));
                    store_opened = true;
                } catch (const error& e) {
                    // ADR-0016: persistence is best-effort. An unwritable,
                    // full, or otherwise unusable layer dir must not stop
                    // the image from booting — degrade to remote-only.
                    ELIO_LOG_WARNING(
                        "layer {}: persistence unavailable ({}); serving "
                        "remotely without caching",
                        lower.digest, e.what());
                }
                if (!store_opened) {
                    // The moved-from `reg` died with the failed open;
                    // re-open the registry source (cold path) behind a
                    // fresh tap and the same funnel wrapper as the
                    // no-dir path.
                    reg = co_await source::RegistrySource::open(client, url);
                    auto retap = std::make_unique<TraceRecordSource>(
                        std::move(reg), recorder,
                        static_cast<uint32_t>(layer_index));
                    tap = retap.get();
                    raw = std::make_unique<source::AdmissionSource>(
                        std::move(retap), funnel);
                }
            }
        }

        auto untarred = co_await source::TarOffsetSource::open(std::move(raw));
        if (tap != nullptr) {
            // Recorded offsets address the payload space (the replay
            // contract); the tar base is known only after this probe and
            // no read traffic exists yet (assembly is sequential).
            if (const auto* tos =
                    dynamic_cast<const source::TarOffsetSource*>(
                        untarred.get())) {
                tap->set_base(tos->base_offset());
            }
        }
        warm_targets.push_back(untarred.get());
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

    // Structural warm-up (ADR-0012's cold-start floor): populate the
    // head/tail windows of every data lower's stored-blob view before
    // the merge takes ownership of the layer chain — the floor first;
    // trace replay below refines it. Awaited inline during bring-up
    // under a wall-time budget, exactly like replay (detaching both off
    // the bring-up path is the same documented follow-up); every
    // populate rides the funnel as the Prefetch scavenger class, and
    // dedup against the open-time probes, replay, and fill is automatic
    // via the LayerStore in-flight map. Opportunistic — failures are
    // logged, never propagated, and a bypassed/degraded store turns
    // populate into a no-op. `prefetch.enable` is the master gate for
    // both warm-up kinds.
    StructuralWarmupStats warmup_stats;
    if (global.prefetch_enable) {
        StructuralWarmupOptions wopts;
        wopts.head_bytes =
            static_cast<uint64_t>(global.prefetch_head_kb) << 10;
        wopts.tail_bytes =
            static_cast<uint64_t>(global.prefetch_tail_kb) << 10;
        warmup_stats = co_await warmup_structural(warm_targets, wopts);
    }

    // Trace replay (ADR-0013): with the floor warmed, load the
    // acceleration layer's trace blob best-effort (a missing, unreadable,
    // or slow blob only disables prefetch — the device is already fully
    // functional, C3's contract; `prefetch.enable` gates the load/replay)
    // and warm the data lowers from the recorded access pattern before
    // the merge takes ownership of the layer chain.
    TraceReplayStats trace_stats;
    if (cfg.acceleration_layer && global.prefetch_enable) {
        const std::vector<uint8_t> trace_blob = co_await load_trace_blob(
            cfg.lowers.back(), cfg, client, funnel);
        if (!trace_blob.empty()) {
            trace_stats = co_await replay_trace(trace_blob, warm_targets);
        }
    }

    const size_t n = layers.size();
    if (!cfg.writable()) {
        auto merged = co_await format::MergedLsmt::open(std::move(layers));
        OpenedImage out;
        out.virtual_size = merged->size();
        out.layer_count = n;
        out.trace = trace_stats;
        out.warmup = warmup_stats;
        out.root = std::move(merged);
        out.layer_stores = std::move(stores);
        out.funnel = std::move(funnel);
        out.recorder = std::move(recorder);
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
    out.trace = trace_stats;
    out.warmup = warmup_stats;
    out.root = std::move(merged);
    out.layer_stores = std::move(stores);
    out.funnel = std::move(funnel);
    out.recorder = std::move(recorder);
    ELIO_LOG_INFO("image assembled writable: {} lowers + {} upper, virtual "
                  "size {} bytes",
                  n, cfg.upper.type, out.virtual_size);
    co_return out;
}

elio::coro::task<void> park_image_fills(const OpenedImage& opened) {
    for (auto* store : opened.layer_stores) store->stop_fill();
    for (auto* store : opened.layer_stores) {
        bool parked = false;
        for (int i = 0; i < 5000 && !parked; ++i) {
            using FillStatus = source::LayerStore::FillStatus;
            const FillStatus s = store->fill_status();
            parked = s == FillStatus::kDisabled || s == FillStatus::kDone ||
                     s == FillStatus::kStopped;
            if (!parked) {
                co_await elio::time::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (!parked) {
            // Still in its start delay or stuck on the remote: the caller
            // is on a teardown path that ends in process exit, which
            // reaps the coroutine before it can resume (see the
            // LayerStore lifetime contract).
            ELIO_LOG_WARNING("layer store fill did not park within the "
                             "bounded wait; relying on process exit");
        }
    }
    co_return;
}

uint64_t device_capacity_bytes(uint64_t image_bytes, uint64_t override_bytes,
                               std::string* error) {
    // Grow-only (ADR-0014 dev_size model): the override is sanctioned
    // headroom — an override below the image's declared size would
    // shrink the device below its content and is rejected cleanly.
    // override == image size is a no-op override and allowed.
    if (override_bytes > 0 && override_bytes < image_bytes) {
        if (error != nullptr) {
            *error = "create virtual_size " + std::to_string(override_bytes) +
                     " is smaller than the image's virtual size " +
                     std::to_string(image_bytes) +
                     " (grow-only: a smaller device would shrink below the "
                     "image content)";
        }
        return 0;
    }
    return override_bytes > 0 ? override_bytes : image_bytes;
}

}  // namespace obd::image
