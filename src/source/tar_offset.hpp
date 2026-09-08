// TarOffsetSource — skips the tar wrapper that OverlayBD puts around layer
// blobs.
//
// Registry blobs produced by overlaybd-commit are a single file stored
// inside a tar container: either a plain 512B ustar header (size taken from
// the header's octal size field) or, for locally committed layers, the
// "new tar" shape written by overlaybd's TarFile::mark_new_tar — a pax
// extended header block, one pax record block, then a real header carrying
// the empty-marker magic "xxtar"/version "xx" (payload size = underlying
// size minus the 1536B prefix).
//
// Detection replicates overlaybd tar/tar_file.cpp: is_tar_file() (ustar
// magic + "00" version + header checksum) decides wrapping; the typeflag
// 'x'/'g' decides base_offset 1536 vs 512; the "xxtar" marker switches the
// size source to (underlying size - base_offset).
#pragma once

#include "source/blob_source.hpp"

#include <elio/coro/task.hpp>

#include <memory>

namespace obd::source {

class TarOffsetSource final : public BlobSource {
public:
    /// Probes the tar wrapper on `src` (ownership taken either way):
    /// returns a TarOffsetSource when `src` is a tar-wrapped overlaybd
    /// blob, or `src` itself when it is not. Throws obd::error on read
    /// failures, obd::format_error on inconsistent tar headers (e.g. a
    /// truncated "new tar" prefix).
    static elio::coro::task<BlobSourcePtr> open(BlobSourcePtr src);

    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;

    /// Warms [offset, offset+len) in the PAYLOAD byte space: translated by
    /// the tar base offset and forwarded to the wrapped source (so a
    /// populate on this view reaches the LayerStore below, ADR-0013 trace
    /// replay). Clamped at the payload size like pread.
    elio::coro::task<ssize_t> populate(uint64_t offset, size_t len) override;

    uint64_t size() const noexcept override { return size_; }
    std::string_view label() const noexcept override { return label_; }

    uint64_t base_offset() const noexcept { return base_; }

private:
    TarOffsetSource() = default;

    BlobSourcePtr src_;
    uint64_t base_ = 0;
    uint64_t size_ = 0;
    std::string label_;
};

}  // namespace obd::source
