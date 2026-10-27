// Hybrid WritableLayer adapter. See ADR-0026.
#pragma once
#include "format/hybrid_writer.hpp"
#include "format/writable.hpp"

namespace obd::format {
class HybridRwLayer final : public WritableLayer {
public:
    static elio::coro::task<std::unique_ptr<HybridRwLayer>> create(
        std::string directory, uint64_t virtual_size);
    static elio::coro::task<std::unique_ptr<HybridRwLayer>> open_existing(
        std::string data_path, std::string index_path);

    /// Open an existing pair or atomically create an absent pair directory.
    /// Existing incomplete directories fail closed; a larger persisted size
    /// is retained, while a larger requested size is durably grown.
    static elio::coro::task<std::unique_ptr<HybridRwLayer>> open_or_create(
        std::string directory, uint64_t virtual_size);

    elio::coro::task<ssize_t> pread(void* buffer, size_t count, uint64_t offset) override;
    elio::coro::task<ssize_t> pwrite(const void* buffer, size_t count, uint64_t offset) override;
    elio::coro::task<int> discard(uint64_t offset, uint64_t count) override;
    elio::coro::task<int> flush() override;
    elio::coro::task<int> checkpoint() override;
    int grow(uint64_t virtual_size) override;
    uint64_t virtual_size() const override { return writer_.virtual_size(); }
    /// Borrowed legacy view: caller must exclude all other segments() calls.
    /// Concurrent consumers use segments_snapshot(), which returns an owned copy.
    const std::vector<bytes::segment_mapping>& segments() const override;
    std::vector<bytes::segment_mapping> segments_snapshot() const override;
    source::BlobSource& data_source() override { return view_; }
    bool serialize_merged_reads() const noexcept override { return true; }
    elio::coro::task<int> seal(std::string user_tag = "");
    /// Exclusive offline seal; retains both locks through streaming digest IO.
    /// Outputs change only on success. Zero virtual_size keeps the current
    /// size; a nonzero override is grow-only. Already sealed returns EALREADY.
    static elio::coro::task<int> seal_file(
        std::string data_path, std::string index_path, std::string user_tag,
        std::string* sha256_hex, uint64_t* size, uint64_t virtual_size = 0,
        std::string* reject = nullptr);

private:
    explicit HybridRwLayer(std::unique_ptr<hybrid::FilePair> pair);
    class View final : public source::BlobSource {
    public:
        explicit View(hybrid::Writer& writer) : writer_(writer) {}
        elio::coro::task<ssize_t> pread(void* buffer, size_t count, uint64_t offset) override;
        uint64_t size() const noexcept override { return writer_.physical_size(); }
        std::string_view label() const noexcept override { return "hybrid-upper-payload"; }
    private:
        hybrid::Writer& writer_;
    };
    hybrid::Writer writer_;
    View view_;
    mutable std::vector<bytes::segment_mapping> borrowed_segments_;
};
}  // namespace obd::format
