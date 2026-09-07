// LSMT writable layer (ADR-0008): an unsealed single-file LSMT supporting
// in-place edit. Write ranges already covered (fully or partially) by this
// layer's segment index overwrite their data blocks in place; only
// previously-uncovered subranges append new data at the data end. seal()
// compacts the file into a standard sealed LSMT RO file (garbage from
// superseded in-place regions is dropped), readable by LsmtLayer.
//
// v0.2 limitation: the segment index is memory-only until seal(); an
// unsealed RW file is NOT recoverable across process restarts (create()
// truncates). Seal explicitly to persist.
#pragma once

#include "format/lsmt_format.hpp"
#include "format/writable.hpp"
#include "source/local_file.hpp"

#include <atomic>
#include <string>

namespace obd::format {

class LsmtRwLayer final : public WritableLayer {
public:
    ~LsmtRwLayer() override;  // out-of-line: View is an incomplete type here

    /// Creates a fresh unsealed LSMT RW file at `path` (any existing file
    /// is truncated) with the given virtual size in bytes (sector aligned).
    static elio::coro::task<std::unique_ptr<LsmtRwLayer>> create(
        const std::string& path, uint64_t vsize);

    elio::coro::task<ssize_t> pwrite(const void* buf, size_t count,
                                     uint64_t offset) override;
    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;
    elio::coro::task<int> flush() override;

    uint64_t virtual_size() const override { return vsize_; }
    const std::vector<bytes::segment_mapping>& segments() const override {
        return segments_;
    }
    /// The file view segment data is read from; its size tracks appends
    /// (unlike a LocalFileSource, which pins the size at open).
    source::BlobSource& data_source() override;

    bool sealed() const noexcept { return sealed_; }

    /// Compacts and seals the file in place (atomic rename); afterwards it
    /// is a standard sealed LSMT RO file. Subsequent pwrite returns -EROFS.
    elio::coro::task<int> seal(const std::string& user_tag = "");

private:
    LsmtRwLayer() = default;

    int fd_ = -1;                   // RW fd (also used for data_source reads)
    class View;                     // fd-backed BlobSource with dynamic size
    std::unique_ptr<View> view_;
    std::atomic<uint64_t> data_bytes_{0};  // upper bound for view reads
    uint64_t vsize_ = 0;            // bytes
    uint64_t data_end_sector_ = 0;  // append position, sectors
    std::string uuid_;
    std::string path_;
    bool sealed_ = false;
    std::vector<bytes::segment_mapping> segments_;
};

}  // namespace obd::format
