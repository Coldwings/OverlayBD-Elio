#include "source/gzip_index_source.hpp"
#include "common/crc32c.hpp"
#include "common/errors.hpp"
#include <climits>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <zlib.h>

namespace obd::source {
namespace {
constexpr size_t header_size = 333, entry_size = 29, window_size = 32768;
constexpr uint64_t metadata_limit = 64 * 1024 * 1024;
uint64_t le(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}
void require(bool ok) {
    if (!ok) throw format_error("invalid ddgzidx v1 index");
}
struct Inflater {
    z_stream stream{};
    bool initialized = false;
    ~Inflater() { if (initialized) inflateEnd(&stream); }
};
bool unpack(const uint8_t* data, size_t len, uint8_t* out, size_t expected) {
    Inflater inflater;
    if (inflateInit(&inflater.stream) != Z_OK) return false;
    inflater.initialized = true;
    auto& z = inflater.stream;
    z.next_in = const_cast<Bytef*>(data);
    z.avail_in = static_cast<uInt>(len);
    z.next_out = out;
    z.avail_out = static_cast<uInt>(expected);
    return inflate(&z, Z_FINISH) == Z_STREAM_END &&
           z.total_out == expected && z.avail_in == 0;
}
}

elio::coro::task<std::unique_ptr<GzipIndexSource>> GzipIndexSource::open(
    BlobSourcePtr compressed, BlobSourcePtr index) {
    require(compressed && index && index->size() >= header_size);
    std::array<uint8_t, header_size> h{};
    auto read_result = co_await index->pread(h.data(), h.size(), 0);
    if (read_result < 0) throw error(static_cast<int>(-read_result), "read ddgzidx header");
    require(read_result == ssize_t(h.size()));
    require(std::memcmp(h.data(), "ddgzidx\0", 8) == 0 && h[8] == 1 &&
            h[9] == 0 && h[10] <= 1 && h[12] == 0);
    require(le(h.data()+17, 4) == window_size && le(h.data()+21, 4) == entry_size);
    require(le(h.data()+13, 4) >= 65536 && le(h.data()+13, 4) <= INT32_MAX);
    require(crc32::crc32c(h.data(), 329) == le(h.data()+329, 4));
    const uint64_t count = le(h.data()+25, 8), start = le(h.data()+313, 8);
    const uint64_t len = le(h.data()+321, 8), size = le(h.data()+49, 8);
    require(le(h.data()+33, 8) == compressed->size() &&
            le(h.data()+41, 8) == index->size() && size <= INT64_MAX);
    require(count > 0 && count <= metadata_limit / entry_size &&
            start >= header_size && start <= index->size() &&
            len == index->size() - start && len > 0 && len <= metadata_limit);
    std::vector<uint8_t> packed(static_cast<size_t>(len));
    read_result = co_await index->pread(packed.data(), packed.size(), start);
    if (read_result < 0) throw error(static_cast<int>(-read_result), "read ddgzidx entries");
    require(read_result == ssize_t(packed.size()));
    std::vector<uint8_t> decoded(static_cast<size_t>(count * entry_size));
    if (h[10]) require(unpack(packed.data(), packed.size(), decoded.data(), decoded.size()));
    else { require(packed.size() == decoded.size()); decoded = std::move(packed); }
    auto result = std::unique_ptr<GzipIndexSource>(new GzipIndexSource);
    result->size_ = size;
    result->algorithm_ = h[10];
    result->entries_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto* p = decoded.data() + i * entry_size;
        Entry e{le(p,8), le(p+8,8), le(p+16,8), static_cast<uint32_t>(le(p+25,4)), p[24]};
        require(e.output <= size && e.input < compressed->size() && e.bits <= 7 &&
                (e.bits == 0 || e.input > 0) && e.dictionary >= header_size &&
                e.dictionary <= start && e.dictionary_size <= start - e.dictionary &&
                e.dictionary_size > 0 && e.dictionary_size <= compressBound(window_size));
        require(h[10] || e.dictionary_size == window_size);
        require(i ? (e.output > result->entries_.back().output &&
                     e.input >= result->entries_.back().input) : e.output == 0);
        result->entries_.push_back(e);
    }
    result->compressed_ = std::move(compressed);
    result->index_ = std::move(index);
    co_return result;
}

elio::coro::task<ssize_t> GzipIndexSource::pread(void* buffer, size_t count, uint64_t offset) {
    if (!count || offset >= size_) co_return 0;
    if (!buffer || count > static_cast<size_t>(SSIZE_MAX)) co_return -EINVAL;
    count = static_cast<size_t>(std::min<uint64_t>(count, size_ - offset));
    try {
        const auto it = std::upper_bound(entries_.begin(), entries_.end(), offset,
            [](uint64_t pos, const Entry& e) { return pos < e.output; });
        const auto& e = *std::prev(it);
        std::array<uint8_t, window_size> dictionary{};
        std::vector<uint8_t> packed(e.dictionary_size);
        auto r = co_await index_->pread(packed.data(), packed.size(), e.dictionary);
        if (r < 0) co_return r;
        if (r != ssize_t(packed.size())) co_return -EIO;
        if (algorithm_) {
            if (!unpack(packed.data(), packed.size(), dictionary.data(), dictionary.size())) co_return -EIO;
        } else std::memcpy(dictionary.data(), packed.data(), dictionary.size());
        Inflater inflater;
        if (inflateInit2(&inflater.stream, -15) != Z_OK) co_return -ENOMEM;
        inflater.initialized = true;
        auto& z = inflater.stream;
        if (e.bits) {
            uint8_t byte;
            r = co_await compressed_->pread(&byte, 1, e.input - 1);
            if (r < 0) co_return r;
            if (r != 1 || inflatePrime(&z, e.bits, byte >> (8-e.bits)) != Z_OK) co_return -EIO;
        }
        if (inflateSetDictionary(&z, dictionary.data(), dictionary.size()) != Z_OK) co_return -EIO;
        std::array<uint8_t, 65536> input{}, discard{};
        uint64_t position = e.input, skip = offset - e.output;
        size_t done = 0;
        while (skip || done < count) {
            const size_t chunk = skip ? std::min<uint64_t>(skip, discard.size()) :
                                        std::min(count-done, discard.size());
            z.next_out = skip ? discard.data() : static_cast<uint8_t*>(buffer)+done;
            z.avail_out = static_cast<uInt>(chunk);
            while (z.avail_out) {
                if (!z.avail_in) {
                    r = co_await compressed_->pread(input.data(), input.size(), position);
                    if (r < 0) co_return r;
                    if (!r || r > ssize_t(input.size())) co_return -EIO;
                    position += r;
                    z.next_in = input.data();
                    z.avail_in = static_cast<uInt>(r);
                }
                const auto before_in = z.avail_in, before_out = z.avail_out;
                const int status = inflate(&z, Z_NO_FLUSH);
                if (status == Z_STREAM_END) {
                    if (z.avail_out || skip || done + chunk != count) co_return -EIO;
                    break;
                }
                if (status != Z_OK && status != Z_BUF_ERROR) co_return -EIO;
                if (before_in == z.avail_in && before_out == z.avail_out) co_return -EIO;
            }
            if (skip) skip -= chunk;
            else done += chunk;
        }
        co_return static_cast<ssize_t>(done);
    } catch (const std::bad_alloc&) { co_return -ENOMEM; }
      catch (...) { co_return -EIO; }
}
} // namespace obd::source
