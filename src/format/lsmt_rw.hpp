// LSMT writable layer (ADR-0008): an unsealed single-file LSMT supporting
// in-place edit. Write ranges covered by live segments overwrite their
// data blocks in place; uncovered or discarded (zeroed) subranges append
// new data at the data end. Zeroed segments own no physical blocks. seal()
// compacts the file into a standard sealed LSMT RO file (garbage from
// superseded in-place regions is dropped), readable by LsmtLayer.
//
// v0.2 limitation: the segment index is memory-only until checkpoint() or
// seal(); an unsealed RW file is NOT recoverable across process restarts
// (create() truncates). checkpoint() (called by the device process on
// graceful shutdown, ADR-0014) persists the index as an unsealed trailer so
// seal_file() can seal the file offline, from another process, after the
// device is gone.
//
// Seal determinism (ADR-0014): the sealed file's uuid is derived from the
// content digest (sha256 of virtual_size || packed data || packed index,
// formatted as a 8-4-4-4-12 uuid string), so identical upper content seals
// to identical bytes; see docs/format.md.
#pragma once

#include "format/lsmt_format.hpp"
#include "format/writable.hpp"
#include "source/local_file.hpp"

#include <atomic>
#include <string>

namespace obd::format {

/// ADR-0014 blank (raw) device zero base: writes a sealed EMPTY LSMT RO
/// layer at `path` — virtual_size = `vsize`, index_size = 0 (no segments,
/// no data). Reads through the ordinary merge path therefore serve the
/// whole [0, vsize) range as zeroes (LsmtLayer/MergedLsmt zero-fill holes),
/// which is what makes a blank disk read as zeroed from birth.
///
/// Byte-deterministic with the same rule as seal(): the sealed uuid is
/// derived from sha256(vsize as LE u64 || packed data (none) || packed
/// index entries (none)), formatted as a 8-4-4-4-12 uuid string — a pure
/// function of `vsize`, no randomness, no clock. Identical vsize writes
/// identical bytes (docs/format.md); an untouched blank upper that seals
/// with an empty user_tag reproduces this base byte-for-byte.
///
/// Layout mirrors seal()'s compaction output for an empty index: header
/// region at byte 0, trailer region as the file's last 4096 bytes,
/// index_offset = 4096 (the data-region start / first allowed index
/// position), total file size 8192. Returns 0 or a negative -errno
/// (-EINVAL when vsize is not a positive multiple of 512).
elio::coro::task<int> create_empty_lsmt_layer(const std::string& path,
                                              uint64_t vsize,
                                              const std::string& user_tag = "");

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
    elio::coro::task<int> discard(uint64_t offset, uint64_t len) override;

    /// Persists the in-memory segment index into the file as an unsealed
    /// trailer (index region + trailer appended at the data end), so the
    /// file can later be sealed offline by seal_file() (ADR-0014). Called
    /// by the device process on graceful shutdown after IO has drained.
    /// Terminal: pwrite/discard after a checkpoint return -EROFS. Returns 0
    /// or a negative -errno; -EROFS when already sealed or checkpointed.
    elio::coro::task<int> checkpoint() override;

    uint64_t virtual_size() const override {
        return vsize_.load(std::memory_order_acquire);
    }
    const std::vector<bytes::segment_mapping>& segments() const override {
        return segments_;
    }
    /// The file view segment data is read from; its size tracks appends
    /// (unlike a LocalFileSource, which pins the size at open).
    source::BlobSource& data_source() override;

    bool sealed() const noexcept {
        return sealed_.load(std::memory_order_acquire);
    }

    /// D3 grow-only vsize extension (see WritableLayer::grow): also
    /// rewrites the on-disk declared-size header (uuid preserved) so a
    /// later checkpoint/offline seal stays consistent with the grown
    /// size. BLOCKING (header rewrite + fsync): run off an Elio worker
    /// via elio::spawn_blocking. Returns 0 or a negative -errno.
    int grow(uint64_t vsize) override;

    /// Compacts and seals the file in place (atomic rename); afterwards it
    /// is a standard sealed LSMT RO file. Subsequent pwrite returns -EROFS.
    /// The sealed uuid is content-derived (see the file header comment), so
    /// identical content seals to identical bytes (ADR-0014).
    elio::coro::task<int> seal(const std::string& user_tag = "");

    /// Offline seal (ADR-0014): opens a checkpointed unsealed RW file at
    /// `path` (one written by checkpoint(), e.g. by a device process that
    /// has since exited), seals it in place, and reports the sealed file's
    /// sha256 hex digest and byte size. Returns 0 or a negative -errno:
    /// -ENOENT when the file is missing, -EALREADY when it is already
    /// sealed, -EINVAL when it is not a valid checkpointed LSMT-RW file
    /// (e.g. the device crashed before checkpointing).
    ///
    /// D3 commit re-baseline: `virtual_size` (bytes, 0 = keep the
    /// checkpointed size) overrides the virtual size written into the
    /// sealed header/trailer (and hashed into the content digest), so a
    /// commit can declare a larger device. Grow-only, validated here
    /// before any compaction: the override must be 512-aligned and at
    /// least both the layer's declared virtual size and its content
    /// extent (the highest covered sector). A rejected override returns
    /// -EINVAL with a human-readable reason in `reject` (when non-null).
    static elio::coro::task<int> seal_file(const std::string& path,
                                           const std::string& user_tag,
                                           std::string* sha256_hex,
                                           uint64_t* size,
                                           uint64_t virtual_size = 0,
                                           std::string* reject = nullptr);

private:
    LsmtRwLayer() = default;

    /// Opens an existing checkpointed RW file (no truncation) with its
    /// index loaded from the on-disk unsealed trailer. Returns a negative
    /// -errno via `error` (-EINVAL/-EALREADY/-ENOENT/...) on failure.
    static elio::coro::task<std::unique_ptr<LsmtRwLayer>> open_checkpointed(
        const std::string& path, int* error);

    int fd_ = -1;                   // RW fd (also used for data_source reads)
    class View;                     // fd-backed BlobSource with dynamic size
    std::unique_ptr<View> view_;
    std::atomic<uint64_t> data_bytes_{0};  // upper bound for view reads
    /// Declared size in bytes. Atomic: the device resize executor (a
    /// spawn_blocking pool thread) grows the layer while bridge
    /// coroutines on Elio workers read/write through it.
    std::atomic<uint64_t> vsize_{0};
    uint64_t data_end_sector_ = 0;  // append position, sectors
    std::string uuid_;
    std::string path_;
    /// Terminal-state flags. ATOMIC on purpose: grow() runs on a
    /// spawn_blocking pool thread (the device resize executor) while
    /// checkpoint()/seal() — which set them — run on Elio workers, the
    /// same cross-thread class the vsize_ atomic covers. Invariant:
    /// once either is set, no pwrite/discard/grow may mutate the layer
    /// (they return -EROFS), so the on-disk header can never be
    /// rewritten after the shutdown checkpoint. (No TSAN in this build.)
    std::atomic<bool> sealed_{false};
    std::atomic<bool> checkpointed_{false};  // terminal: no more pwrite/discard
    std::vector<bytes::segment_mapping> segments_;
};

}  // namespace obd::format
