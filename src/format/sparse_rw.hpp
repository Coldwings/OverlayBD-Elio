// Sparse writable layer: a sparse file whose written extents form the
// layer's segment index with identity mapping (moffset == offset).
// Extents are rebuilt from the kernel fiemap (SEEK_DATA/SEEK_HOLE) when an
// existing file is opened, so reopening preserves coverage. Note: fiemap
// is filesystem-block granular — a sub-block punch-hole zeroes but cannot
// deallocate, so a recovered index may be fatter than the pre-reopen one;
// reads are unaffected (punched blocks read back as zeroes).
#pragma once

#include "format/writable.hpp"
#include "source/local_file.hpp"

#include <string>

namespace obd::format {

class SparseRwLayer final : public WritableLayer {
public:
    /// Opens (creating if needed) `path` as a sparse file of `vsize` bytes.
    /// Existing content is kept; written extents are recovered via fiemap.
    static elio::coro::task<std::unique_ptr<SparseRwLayer>> open(
        const std::string& path, uint64_t vsize);

    elio::coro::task<ssize_t> pwrite(const void* buf, size_t count,
                                     uint64_t offset) override;
    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;
    elio::coro::task<int> flush() override;
    elio::coro::task<int> discard(uint64_t offset, uint64_t len) override;
    /// No-op: sparse extents are durable via the fiemap already (sparse
    /// uppers never seal — ADR-0014 upstream parity).
    elio::coro::task<int> checkpoint() override;

    uint64_t virtual_size() const override { return vsize_; }
    const std::vector<bytes::segment_mapping>& segments() const override {
        return segments_;
    }
    source::BlobSource& data_source() override { return *ro_; }

private:
    SparseRwLayer() = default;
    /// Inserts [off, off+len) (sectors) merging overlapping/adjacent
    /// identity segments.
    void insert_extent(uint64_t off, uint64_t len);

    int fd_ = -1;                    // RW fd (writes + flushes)
    source::BlobSourcePtr ro_;       // RO view for data_source()
    uint64_t vsize_ = 0;             // bytes
    std::vector<bytes::segment_mapping> segments_;
};

}  // namespace obd::format
