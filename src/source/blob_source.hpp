// BlobSource — the single async byte-source interface every layer of the
// read stack is built on.
//
// Contract (docs/source.md §"Invariants & Guarantees"):
//   * pread is positional and never mutates shared read state; concurrent
//     preads on one source are safe unless the concrete type says otherwise.
//   * A successful pread returns `count` except at end of blob, where it
//     returns the remaining byte count (possibly 0). Implementations loop
//     internally over short backend reads; callers never see a mid-blob
//     short read.
//   * Errors are returned as negative errno values on the hot path;
//     implementations must not throw from pread.
//   * size() is fixed for the lifetime of the source (blobs are immutable).
#pragma once

#include <elio/coro/task.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <sys/types.h>

namespace obd::source {

class BlobSource {
public:
    virtual ~BlobSource() = default;

    /// Reads up to `count` bytes at `offset` into `buf`.
    /// Returns bytes read (== count except at EOF, 0 at/after EOF), or a
    /// negative -errno on failure. Runs as an Elio coroutine.
    virtual elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                            uint64_t offset) = 0;

    /// Total blob size in bytes. Constant for the source's lifetime.
    virtual uint64_t size() const noexcept = 0;

    /// Human-readable identity for logs and diagnostics (e.g. the layer
    /// digest or local path). Stable for the source's lifetime.
    virtual std::string_view label() const noexcept = 0;
};

using BlobSourcePtr = std::unique_ptr<BlobSource>;

/// Writable block source (ADR-0008): the device root of an image with a
/// writable upper layer. Reads follow the BlobSource contract; writes are
/// sector-aligned like reads and follow the same return convention.
/// The ublk bridge dispatches WRITE/FLUSH through this interface and
/// answers -EROFS when the root does not implement it.
class WritableBlobSource : public BlobSource {
public:
    /// Writes count bytes at offset (both 512B multiples).
    /// Returns count, or a negative -errno.
    virtual elio::coro::task<ssize_t> pwrite(const void* buf, size_t count,
                                             uint64_t offset) = 0;

    /// Durability point (ublk FLUSH). Returns 0 or a negative -errno.
    virtual elio::coro::task<int> flush() = 0;
};

}  // namespace obd::source
