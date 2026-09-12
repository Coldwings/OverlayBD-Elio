#pragma once
#include "source/blob_source.hpp"
#include <vector>

namespace obd::source {

// Read-only view of an upstream ddgzidx v1 indexed gzip stream.
class GzipIndexSource final : public BlobSource {
public:
    static elio::coro::task<std::unique_ptr<GzipIndexSource>> open(
        BlobSourcePtr compressed, BlobSourcePtr index);
    elio::coro::task<ssize_t> pread(void*, size_t, uint64_t) override;
    uint64_t size() const noexcept override { return size_; }
    std::string_view label() const noexcept override { return compressed_->label(); }
private:
    struct Entry {
        uint64_t output, input, dictionary;
        uint32_t dictionary_size;
        uint8_t bits;
    };
    BlobSourcePtr compressed_, index_;
    std::vector<Entry> entries_;
    uint64_t size_ = 0;
    uint8_t algorithm_ = 0;
};
} // namespace obd::source
