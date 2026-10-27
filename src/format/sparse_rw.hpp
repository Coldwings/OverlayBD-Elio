// Sparse writable layer: exact logical coverage forms the segment index,
// with identity mapping for DATA (moffset == offset).
// New uppers persist exact sector-granular DATA/ZERO ownership in a
// checksummed coverage sidecar. SEEK_DATA/SEEK_HOLE is used only once when
// importing an old upper that has no coverage sidecar; it is never used to
// expand ownership after the v2 sidecar exists.
#pragma once

#include "format/writable.hpp"

#include <elio/sync/mutex.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace obd::format {

struct ZeroMaskPersistResult {
    int rc = 0;
    bool namespace_committed = false;
};

struct SparseExportResult {
    std::string uuid;
    std::string sha256;
    uint64_t size = 0;
};

class SparseRwLayer final : public WritableLayer {
public:
    ~SparseRwLayer() override;

    /// Opens (creating if needed) `path` as a sparse file of `vsize` bytes.
    /// Existing content is kept. V2 content is recovered from exact coverage
    /// metadata; legacy content is imported once from SEEK_DATA/SEEK_HOLE and
    /// the existing `.zeroes` sidecar.
    static elio::coro::task<std::unique_ptr<SparseRwLayer>> open(
        const std::string& path, uint64_t vsize);

    /// Recover an existing regular file at its persisted size without creating
    /// or growing it. All opens hold a nonblocking exclusive inode lock until
    /// destruction; another cooperating writer is rejected with EBUSY.
    static elio::coro::task<std::unique_ptr<SparseRwLayer>> open_existing(
        std::string path);

    /// Offline delta export to a separate immutable LSMT artifact. Source must
    /// have no other cooperating owner; it remains reusable after this returns.
    /// Existing output is accepted only if byte-identical; never overwritten.
    /// Throws on failure, including directory sync after visible publication.
    static elio::coro::task<SparseExportResult> export_file(
        std::string source, std::string destination,
        std::string parent_uuid = {}, std::string user_tag = {},
        uint64_t virtual_size = 0);

    elio::coro::task<ssize_t> pwrite(const void* buf, size_t count,
                                     uint64_t offset) override;
    elio::coro::task<ssize_t> pread(void* buf, size_t count,
                                    uint64_t offset) override;
    elio::coro::task<int> flush() override;
    elio::coro::task<int> discard(uint64_t offset, uint64_t len) override;
    /// Persists dirty zero-mask metadata through the ordered data-file and
    /// sidecar durability path shared with flush(). Ends writes on this instance
    /// but retains its exclusive lock until destruction. Offline export_file()
    /// creates a separate sealed LSMT artifact after the current owner closes.
    elio::coro::task<int> checkpoint() override;

    /// D3 grow-only vsize extension (see WritableLayer::grow): extends
    /// the sparse file to `vsize` bytes so pwrite/discard accept the new
    /// range. BLOCKING (ftruncate): run off an Elio worker via
    /// elio::spawn_blocking. Returns 0 or a negative -errno.
    int grow(uint64_t vsize) override;

    uint64_t virtual_size() const override {
        return vsize_.load(std::memory_order_acquire);
    }
    const std::vector<bytes::segment_mapping>& segments() const override {
        return segments_;
    }
    std::vector<bytes::segment_mapping> segments_snapshot() const override;
    uint64_t segments_generation() const noexcept override {
        return segments_generation_.load(std::memory_order_acquire);
    }
    source::BlobSource& data_source() override;

private:
    class View;
#ifdef OBD_TEST_HOOKS
    friend struct SparseRwLayerTestAccess;
#endif

    enum class Lifecycle {
        kOpen,
        kGrowing,
        kCheckpointing,
        kCheckpointed,
    };

    struct CoveragePageVersion {
        uint64_t generation = 0;
        uint8_t slot = 0;
    };
    enum class CoverageLoadMode { kAuthoritative, kInstalling };

    SparseRwLayer() = default;
    bool has_zero_masks_locked() const;
    bool range_intersects_zero_mask_locked(uint64_t lo, uint64_t hi) const;
    std::vector<bytes::segment_mapping> zero_mask_segments_locked() const;
    void insert_live_extent(uint64_t off, uint64_t len);
    void erase_range(uint64_t lo, uint64_t hi);
    int begin_data_op(uint64_t offset, uint64_t len);
    int begin_flush_op(bool* already_checkpointed);
    int begin_grow_op();
    int begin_checkpoint_op();
    void end_data_op();
    void end_grow_op();
    void end_checkpoint_op(Lifecycle next);
    bool dirty_zero_masks_need_data_first() const;
    ZeroMaskPersistResult persist_zero_masks(
        const std::vector<bytes::segment_mapping>& zeroes,
        uint64_t vsize) const;
    int sync_data_file() const;
    void load_zero_masks();
    static std::unique_ptr<SparseRwLayer> open_sync(std::string path,
                                                    uint64_t vsize,
                                                    bool existing_only = false);
    void create_coverage_snapshot(bool legacy_import);
    void load_coverage(const std::string& artifact_path,
                       uint64_t data_size,
                       CoverageLoadMode mode = CoverageLoadMode::kAuthoritative);
    elio::coro::task<int> publish_coverage_locked(
        const std::vector<bytes::segment_mapping>& exact_next,
        const std::vector<bytes::segment_mapping>& runtime_next,
        uint64_t lo, uint64_t hi, uint64_t* committed_hi);
    elio::coro::task<int> publish_data_first_zero_masks_locked(
        std::vector<bytes::segment_mapping> exact_next,
        std::vector<bytes::segment_mapping> next_zeroes);
    elio::coro::task<int> flush_locked();

    int fd_ = -1;                    // RW fd (writes + flushes)
    int coverage_fd_ = -1;           // exact sector ownership metadata
    std::unique_ptr<View> ro_;       // RO view for data_source()
    /// Declared size in bytes. Atomic: the device resize executor (a
    /// spawn_blocking pool thread) grows the layer while bridge
    /// coroutines on Elio workers read/write through it.
    std::atomic<uint64_t> vsize_{0}; // bytes
    std::string zero_mask_path_;
    std::string coverage_path_;
    std::vector<bytes::segment_mapping> segments_;
    std::atomic<uint64_t> segments_generation_{0};
    std::unordered_map<uint64_t, CoveragePageVersion> coverage_pages_;
    /// Serializes pwrite/discard/flush durability ordering without
    /// blocking an Elio worker while disk work is offloaded.
    elio::sync::mutex op_mu_;
    /// Synchronous admission gate shared by grow/checkpoint and coroutine
    /// operations. Operations register active_ops_ while holding this mutex,
    /// so grow/checkpoint can atomically close admission and observe whether
    /// any already-admitted operation exists.
    std::mutex state_gate_mu_;
    std::atomic<uint32_t> active_ops_{0};
    std::atomic<Lifecycle> lifecycle_{Lifecycle::kOpen};
    mutable std::mutex meta_mu_;
    uint64_t zero_masks_generation_ = 0;
    bool zero_masks_dirty_ = false;
    bool zero_masks_data_first_ = false;
    bool zero_masks_pending_view_ = false;
    std::vector<bytes::segment_mapping> pending_zero_masks_;
    std::vector<bytes::segment_mapping> pending_data_segments_;
    uint64_t coverage_generation_ = 0;
    bool coverage_dirty_ = false;
#ifdef OBD_TEST_HOOKS
    std::optional<uint64_t> fail_coverage_page_for_test_;
    std::optional<uint64_t> fail_coverage_map_prepare_page_for_test_;
    std::optional<uint64_t> fail_coverage_runtime_prepare_page_for_test_;
    std::optional<uint64_t> fail_pwrite_prepare_after_bytes_for_test_;
    bool fail_zero_mask_dirsync_after_commit_for_test_ = false;
    static std::shared_ptr<std::atomic<int>> open_park_for_test_;
    static std::atomic<int> coverage_install_fail_stage_for_test_;
#endif
};

}  // namespace obd::format
