// SwitchSource. See switch_source.hpp.
#include "source/switch_source.hpp"

#include "common/errors.hpp"
#include "source/local_file.hpp"

#include <elio/log/macros.hpp>

#include <sys/stat.h>

namespace obd::source {

namespace {

elio::coro::task<void> switch_to_local(std::shared_ptr<SwitchSource::Control> ctl,
                                       std::string path) {
    try {
        auto local = co_await LocalFileSource::open(path);
        ctl->local.store(std::move(local), std::memory_order_release);
        ELIO_LOG_INFO("switched reads to local file {}", path);
    } catch (const std::system_error& e) {
        // Switch failure is non-fatal: reads keep working over the remote.
        ELIO_LOG_ERROR("switch to {} failed: {}", path, e.what());
    }
}

}  // namespace

elio::coro::task<std::unique_ptr<SwitchSource>> SwitchSource::open(
    BlobSourcePtr remote, BlobSourcePtr raw_remote, std::string dir,
    std::string expected_sha256, const DownloadConfig& dl_cfg) {
    if (!remote) throw error(EINVAL, "switch source with null remote");
    auto sw = std::unique_ptr<SwitchSource>(new SwitchSource());
    sw->ctl_ = std::make_shared<Control>();
    sw->size_ = remote->size();
    sw->label_ = "switch(" + std::string(remote->label()) + ")";
    sw->remote_ = std::move(remote);

    // Already downloaded by a previous run?
    struct stat st {};
    const std::string target = Downloader::target_path(dir);
    if (::stat(target.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        auto local = co_await LocalFileSource::open(target);
        sw->ctl_->local.store(std::move(local), std::memory_order_release);
        ELIO_LOG_INFO("using existing commit file {}", target);
        co_return sw;
    }

    if (dl_cfg.enable) {
        if (!raw_remote) {
            throw error(EINVAL, "download enabled but no raw remote source");
        }
        auto dl = std::make_shared<Downloader>(std::move(raw_remote),
                                               std::move(dir),
                                               std::move(expected_sha256),
                                               dl_cfg);
        auto ctl = sw->ctl_;
        dl->on_complete = [ctl](const std::string& path)
            -> elio::coro::task<void> {
            co_await switch_to_local(ctl, path);
        };
        sw->downloader_ = std::move(dl);
        sw->downloader_->start();
    }
    co_return sw;
}

elio::coro::task<ssize_t> SwitchSource::pread(void* buf, size_t count,
                                              uint64_t offset) {
    if (auto local = ctl_->local.load(std::memory_order_acquire)) {
        co_return co_await local->pread(buf, count, offset);
    }
    co_return co_await remote_->pread(buf, count, offset);
}

}  // namespace obd::source
