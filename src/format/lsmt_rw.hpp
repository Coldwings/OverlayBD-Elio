// LSMT writable data/index pair (ADR-0029), with in-place DATA overwrite.
// Uncovered or discarded (zeroed) subranges append new payload. ZERO mappings
// own no physical blocks. Completed ownership changes publish upstream index
// records before success, allowing recovery after process exit without FLUSH.
// Explicit FLUSH provides storage synchronization; interrupted operations may
// be partial. checkpoint() flushes and terminates the current instance only;
// reopening the unsealed pair permits further writes.
// seal() compacts into a standard sealed LSMT RO file readable by LsmtLayer.
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
#include <memory>
#include <mutex>
#include <string>

namespace obd::format {
namespace lsmt_rw { class FilePair; }

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
/// (-EINVAL when vsize is not a positive multiple of 512). Setup and
/// serialization may throw (including user_tag longer than 255 bytes).
/// After opening the output, every failure closes it and removes the partial
/// file; successful output is retained.
elio::coro::task<int> create_empty_lsmt_layer(const std::string& path,
                                              uint64_t vsize,
                                              const std::string& user_tag = "");

class LsmtRwLayer final : public WritableLayer {
public:
    ~LsmtRwLayer() override;  // out-of-line: View is an incomplete type here

    /// Creates a new upstream pair at path and path + ".index". Existing
    /// artifacts are never truncated or replaced.
    static elio::coro::task<std::unique_ptr<LsmtRwLayer>> create(
        const std::string& path, uint64_t vsize);
    /// Reopen a valid pair, migrate a complete legacy checkpoint, or create
    /// only when neither artifact exists. Recovery callers set existing_only.
    static elio::coro::task<std::unique_ptr<LsmtRwLayer>> open_or_create(
        const std::string& path, uint64_t minimum_size, bool existing_only = false);
#ifdef OBD_TEST_HOOKS
    // Produces the retired memory-only layout for migration fixtures only.
    static elio::coro::task<std::unique_ptr<LsmtRwLayer>> create_legacy_for_test(
        const std::string& path, uint64_t vsize);
#endif

    /// Opens an upstream append-type data/index pair without truncation.
    /// Completed mutations publish index records before returning. The caller
    /// serializes reads/mutations, as for the existing MergedWritable path.
    static elio::coro::task<std::unique_ptr<LsmtRwLayer>> open_existing(
        const std::string& data_path, const std::string& index_path);
    static elio::coro::task<std::unique_ptr<LsmtRwLayer>> create_persistent(
        const std::string& data_path, const std::string& index_path, uint64_t vsize);
    static elio::coro::task<std::unique_ptr<LsmtRwLayer>> migrate_checkpoint(
        const std::string& data_path, const std::string& index_path);

    elio::coro::task<ssize_t> pwrite(const void* buf, size_t count,
                                     uint64_t offset) override;
    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;
    elio::coro::task<int> flush() override;
    elio::coro::task<int> discard(uint64_t offset, uint64_t len) override;

    /// Flushes the persistent pair and terminates this writable instance.
    /// The unsealed pair can subsequently reopen for writes or offline seal.
    /// Called by the device process on graceful shutdown after IO has drained.
    /// Terminal: pwrite/discard after a checkpoint return -EROFS. Returns 0
    /// or a negative -errno; -EROFS when already sealed or checkpointed,
    /// -EBUSY when a direct grow/checkpoint/seal call is already in flight.
    elio::coro::task<int> checkpoint() override;

    uint64_t virtual_size() const override {
        return vsize_.load(std::memory_order_acquire);
    }
    const std::vector<bytes::segment_mapping>& segments() const override {
        return segments_;
    }
    std::optional<uint64_t> segments_revision() const noexcept override {
        const auto value = segments_revision_.load(std::memory_order_acquire);
        if (value == kUnknownRevision) return std::nullopt;
        return value;
    }
    /// The file view segment data is read from; its descriptor handle and
    /// size track appends and a successful live seal (unlike a
    /// LocalFileSource, which pins the size at open).
    source::BlobSource& data_source() override;

    bool sealed() const noexcept {
        return sealed_.load(std::memory_order_acquire);
    }

    /// D3 grow-only vsize extension (see WritableLayer::grow): also
    /// rewrites the on-disk declared-size header (uuid preserved) so a
    /// later checkpoint/offline seal stays consistent with the grown
    /// size. BLOCKING (header rewrite + fsync): run off an Elio worker
    /// via elio::spawn_blocking. Returns 0 or a negative -errno; -EBUSY
    /// when a direct grow/checkpoint/seal call is already in flight.
    int grow(uint64_t vsize) override;

    /// Compacts and seals the file in place (atomic rename); afterwards it
    /// is a standard sealed LSMT RO file. Subsequent pwrite returns -EROFS.
    /// The sealed uuid is content-derived (see the file header comment), so
    /// identical content seals to identical bytes (ADR-0014). Returns -EBUSY
    /// when a direct grow/checkpoint/seal call is already in flight.
    elio::coro::task<int> seal(const std::string& user_tag = "");

    /// Offline seal (ADR-0014/0029): exclusively opens the persistent pair
    /// at `path` and `path + ".index"`, or migrates a valid legacy checkpoint,
    /// then seals it in place and reports its sha256 hex digest and byte size.
    /// A persistent pair needs no prior checkpoint. Returns 0 or -errno;
    /// sealed input returns -EALREADY, malformed input returns -EINVAL, and
    /// another cooperating owner prevents opening the pair.
    ///
    /// D3 commit re-baseline: `virtual_size` (bytes, 0 = keep the
    /// recovered size) overrides the virtual size written into the
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
    static std::unique_ptr<LsmtRwLayer> from_pair(
        std::unique_ptr<lsmt_rw::FilePair>, const std::string& data_path);
    friend struct LsmtRwLayerTestAccess;

    int begin_grow_op();
    void end_grow_op();
    int begin_terminal_op(bool allow_checkpointed);
    void end_terminal_op();

    class FdHandle;                 // shared fd lifetime across in-flight IO
    std::atomic<std::shared_ptr<FdHandle>> fd_;
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
    std::unique_ptr<lsmt_rw::FilePair> pair_;
    bool journal_failed_ = false;
    elio::coro::task<int> prepare_mutation();
    /// Terminal-state flags. ATOMIC on purpose: grow() runs on a
    /// spawn_blocking pool thread (the device resize executor) while
    /// checkpoint()/seal() — which set them — run on Elio workers, the
    /// same cross-thread class the vsize_ atomic covers. Invariant:
    /// once either is set, no pwrite/discard/grow may mutate the layer
    /// (they return -EROFS), so the on-disk header can never be
    /// rewritten after the shutdown checkpoint. (No TSAN in this build.)
    std::atomic<bool> sealed_{false};
    std::atomic<bool> checkpointed_{false};  // terminal: no more pwrite/discard
    std::mutex state_gate_mu_;
    bool grow_op_in_progress_ = false;
    bool terminal_op_in_progress_ = false;
    std::vector<bytes::segment_mapping> segments_;
    static constexpr uint64_t kUnknownRevision = ~uint64_t{0};
    std::atomic<uint64_t> segments_revision_{0};
    // Mutations are caller-serialized, like access to segments_. Publish a
    // changed revision before any potentially partial live-vector update.
    void mappings_changed() noexcept {
        const auto value = segments_revision_.load(std::memory_order_relaxed);
        if (value != kUnknownRevision)
            segments_revision_.store(value + 1, std::memory_order_release);
    }
};

}  // namespace obd::format
