// SwitchSource — atomic remote→local switch of a blob's read path
// (overlaybd SwitchFile contract, simplified per design-assumptions.md §S-4:
// the switch is whole-file, not per-extent).
//
// Reads go to the remote source until the background downloader completes;
// then the local commit file is opened once and atomically swapped in via a
// shared control block (so the downloader's completion coroutine does not
// depend on the SwitchSource's lifetime). If the commit file already exists
// at open time (downloaded by a previous run), reads bind to it directly
// and no download is started.
#pragma once

#include "source/blob_source.hpp"
#include "source/downloader.hpp"

#include <elio/coro/task.hpp>

#include <atomic>
#include <memory>
#include <string>

namespace obd::source {

class SwitchSource final : public BlobSource {
public:
    /// `remote` — the read path used until the switch (typically the
    /// chunk-cached registry/DART source). `raw_remote` — the un-cached
    /// source the downloader reads from (may be null when downloads are
    /// disabled). `dir` — the per-layer directory from the image config.
    /// When `dl_cfg.enable` and no commit file exists yet, the background
    /// download is started immediately (requires a running scheduler).
    static elio::coro::task<std::unique_ptr<SwitchSource>> open(
        BlobSourcePtr remote, BlobSourcePtr raw_remote, std::string dir,
        std::string expected_sha256, const DownloadConfig& dl_cfg);

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;

    uint64_t size() const noexcept override { return size_; }
    std::string_view label() const noexcept override { return label_; }

    /// True once reads are served from the local file.
    bool switched() const noexcept {
        return ctl_->local.load(std::memory_order_acquire) != nullptr;
    }

    Downloader* downloader() const noexcept { return downloader_.get(); }

    /// Shared with the downloader completion coroutine: lets the switch
    /// happen even if this SwitchSource has been released by then.
    /// (Public for the completion helper; not part of the module API.)
    struct Control {
        std::atomic<std::shared_ptr<BlobSource>> local;
    };

private:
    SwitchSource() = default;

    BlobSourcePtr remote_;
    std::shared_ptr<Control> ctl_;
    uint64_t size_ = 0;
    std::string label_;
    std::shared_ptr<Downloader> downloader_;
};

}  // namespace obd::source
