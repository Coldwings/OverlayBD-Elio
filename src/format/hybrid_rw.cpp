#include "format/hybrid_rw.hpp"
#include "common/errors.hpp"
#include "common/sha256.hpp"
#include <elio/io/file_transfer.hpp>
#include <elio/runtime/spawn_blocking.hpp>
#include <algorithm>

namespace obd::format {
HybridRwLayer::HybridRwLayer(std::unique_ptr<hybrid::FilePair> pair)
    : writer_(std::move(pair)), view_(writer_) {}

elio::coro::task<std::unique_ptr<HybridRwLayer>> HybridRwLayer::create(
    std::string directory, uint64_t virtual_size) {
    auto pair = co_await hybrid::FilePair::create(std::move(directory), virtual_size);
    co_return std::unique_ptr<HybridRwLayer>(new HybridRwLayer(std::move(pair)));
}
elio::coro::task<std::unique_ptr<HybridRwLayer>> HybridRwLayer::open_existing(
    std::string data_path, std::string index_path) {
    auto pair = co_await hybrid::FilePair::open_existing(
        std::move(data_path), std::move(index_path), true);
    co_return std::unique_ptr<HybridRwLayer>(new HybridRwLayer(std::move(pair)));
}
elio::coro::task<std::unique_ptr<HybridRwLayer>> HybridRwLayer::open_or_create(
    std::string directory, uint64_t virtual_size) {
    std::unique_ptr<HybridRwLayer> layer;
    try { layer = co_await open_existing(directory + "/data", directory + "/index"); }
    catch (const error& e) { if (e.errno_value() != ENOENT) throw; }
    if (!layer) {
        try { layer = co_await create(directory, virtual_size); }
        catch (const error& e) {
            if (e.errno_value() != EEXIST && e.errno_value() != ENOTEMPTY) throw;
        }
        // A concurrent creator may have won, or an incomplete pair directory
        // already exists. Never truncate it or fill in individual missing files.
        if (!layer) layer = co_await open_existing(directory + "/data", directory + "/index");
    }
    if (virtual_size > layer->virtual_size()) {
        auto* active = layer.get();
        const int rc = co_await elio::spawn_blocking([active, virtual_size] {
            return active->grow(virtual_size);
        });
        if (rc != 0) throw error(-rc, "hybrid: cannot grow reopened upper");
    }
    co_return layer;
}
elio::coro::task<ssize_t> HybridRwLayer::pread(void* buffer, size_t count, uint64_t offset) {
    co_return co_await writer_.pread(buffer, count, offset);
}
elio::coro::task<ssize_t> HybridRwLayer::pwrite(const void* buffer, size_t count, uint64_t offset) {
    co_return co_await writer_.pwrite(buffer, count, offset);
}
elio::coro::task<int> HybridRwLayer::discard(uint64_t offset, uint64_t count) {
    co_return co_await writer_.discard(offset, count);
}
elio::coro::task<int> HybridRwLayer::flush() { co_return co_await writer_.flush(); }
elio::coro::task<int> HybridRwLayer::checkpoint() { co_return co_await writer_.checkpoint(); }
int HybridRwLayer::grow(uint64_t virtual_size) { return writer_.grow(virtual_size); }
const std::vector<bytes::segment_mapping>& HybridRwLayer::segments() const {
    borrowed_segments_ = writer_.snapshot();
    return borrowed_segments_;
}
std::vector<bytes::segment_mapping> HybridRwLayer::segments_snapshot() const {
    return writer_.snapshot();
}
elio::coro::task<int> HybridRwLayer::seal(std::string user_tag) {
    co_return co_await writer_.seal(std::move(user_tag));
}
elio::coro::task<ssize_t> HybridRwLayer::View::pread(void* buffer, size_t count, uint64_t offset) {
    co_return co_await writer_.pread_physical(buffer, count, offset);
}
elio::coro::task<int> HybridRwLayer::seal_file(
    std::string data_path, std::string index_path, std::string user_tag,
    std::string* sha256_hex, uint64_t* size, uint64_t virtual_size,
    std::string* reject) {
    if (reject) reject->clear();
    if (user_tag.size() >= 256) {
        if (reject) *reject = "commit user_tag must contain fewer than 256 bytes";
        co_return -EINVAL;
    }
    if (virtual_size % 512 != 0 || virtual_size / 512 > bytes::segment_mapping::kMaxOffset) {
        if (reject) *reject = "commit virtual_size must be 512-aligned and fit the Hybrid logical range";
        co_return -EINVAL;
    }
    try {
        auto pair = co_await hybrid::FilePair::open_existing(
            std::move(data_path), std::move(index_path), true);
        if (virtual_size != 0 && virtual_size < pair->header().virtual_size) {
            if (reject) *reject = "commit virtual_size is smaller than the layer's declared size (grow-only re-baseline)";
            co_return -EINVAL;
        }
        // Keep this exact locked pair alive through seal and digest. Reopening
        // by pathname after seal could hash a replacement inode instead.
        auto* artifacts = pair.get();
        hybrid::Writer writer(std::move(pair));
        common::Sha256 hash;
        std::vector<std::byte> buffer(1 << 20);
        if (virtual_size != 0 && virtual_size != writer.virtual_size()) {
            auto* active = &writer;
            const int grown = co_await elio::spawn_blocking([active, virtual_size] {
                return active->grow(virtual_size);
            });
            if (grown != 0) co_return grown;
        }
        const int sealed = co_await writer.seal(std::move(user_tag));
        if (sealed != 0) co_return sealed;
        const auto total = artifacts->data_bytes();
        for (uint64_t offset = 0; offset < total;) {
            const auto count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), total - offset));
            const auto read = co_await elio::io::pread_exactly(artifacts->data_fd(),
                std::span(buffer).first(count), offset);
            if (read.error) co_return -read.error.value();
            if (read.end != elio::io::transfer_end::complete) co_return -EIO;
            hash.update(buffer.data(), count);
            offset += count;
        }
        auto digest = hash.final_hex();
        if (sha256_hex) *sha256_hex = std::move(digest);
        if (size) *size = total;
        co_return 0;
    } catch (const error& e) {
        if (reject && e.errno_value() == EINVAL) *reject = e.what();
        co_return -e.errno_value();
    } catch (const std::bad_alloc&) {
        co_return -ENOMEM;
    } catch (const std::length_error&) {
        co_return -EFBIG;
    }
}
}  // namespace obd::format
