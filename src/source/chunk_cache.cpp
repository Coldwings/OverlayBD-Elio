// ChunkCache. See chunk_cache.hpp.
#include "source/chunk_cache.hpp"

#include "common/errors.hpp"

#include <algorithm>
#include <cstring>

namespace obd::source {

elio::coro::task<std::unique_ptr<ChunkCache>> ChunkCache::open(
    BlobSourcePtr inner) {
    co_return co_await open(std::move(inner), Config{});
}

elio::coro::task<std::unique_ptr<ChunkCache>> ChunkCache::open(
    BlobSourcePtr inner, Config cfg) {
    if (!inner) throw error(EINVAL, "chunk cache with null source");
    if (cfg.chunk_size == 0 || cfg.max_bytes < cfg.chunk_size) {
        throw error(EINVAL, "invalid chunk cache configuration");
    }
    auto c = std::unique_ptr<ChunkCache>(new ChunkCache());
    c->label_ = "chunk-cache(" + std::string(inner->label()) + ")";
    c->size_ = inner->size();
    c->cfg_ = cfg;
    c->inner_ = std::move(inner);
    co_return c;
}

elio::coro::task<ssize_t> ChunkCache::pread(void* buf, size_t count,
                                            uint64_t offset) {
    if (offset >= size_) co_return 0;
    if (count > size_ - offset) count = static_cast<size_t>(size_ - offset);
    if (count == 0) co_return 0;

    const size_t cs = cfg_.chunk_size;
    uint8_t* out = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < count) {
        const uint64_t pos = offset + done;
        const uint64_t cid = pos / cs;
        const uint64_t cbase = cid * cs;
        const uint64_t cend = std::min(cbase + cs, size_);
        const size_t within = static_cast<size_t>(pos - cbase);
        const size_t need =
            static_cast<size_t>(std::min(cend, offset + count) - pos);

        std::shared_ptr<const std::vector<uint8_t>> chunk;
        co_await mu_.lock();
        auto it = chunks_.find(cid);
        if (it != chunks_.end()) {
            lru_.erase(it->second.lru_it);
            lru_.push_front(cid);
            it->second.lru_it = lru_.begin();
            chunk = it->second.data;
            mu_.unlock();
            hits_.fetch_add(1, std::memory_order_relaxed);
        } else {
            mu_.unlock();
            misses_.fetch_add(1, std::memory_order_relaxed);
            // Fill the chunk outside the lock; duplicate concurrent fills of
            // the same chunk are harmless (idempotent data, last one wins).
            auto fresh = std::make_shared<std::vector<uint8_t>>(
                static_cast<size_t>(cend - cbase));
            const ssize_t r =
                co_await inner_->pread(fresh->data(), fresh->size(), cbase);
            if (r < 0) co_return done > 0 ? static_cast<ssize_t>(done) : r;
            if (static_cast<size_t>(r) != fresh->size()) {
                co_return done > 0 ? static_cast<ssize_t>(done) : -EIO;
            }
            co_await mu_.lock();
            auto again = chunks_.find(cid);
            if (again != chunks_.end()) {
                lru_.erase(again->second.lru_it);
                lru_.push_front(cid);
                again->second.lru_it = lru_.begin();
                chunk = again->second.data;
            } else {
                lru_.push_front(cid);
                chunk = fresh;
                chunks_.emplace(cid,
                                CachedChunk{std::move(fresh), lru_.begin()});
                bytes_ += cs;
                // Evict from the back until within budget.
                while (bytes_ > cfg_.max_bytes && lru_.size() > 1) {
                    const uint64_t victim = lru_.back();
                    if (victim == cid) break;  // keep the chunk we just read
                    lru_.pop_back();
                    chunks_.erase(victim);
                    bytes_ -= cs;
                }
            }
            mu_.unlock();
        }
        std::memcpy(out + done, chunk->data() + within, need);
        done += need;
    }
    co_return static_cast<ssize_t>(done);
}

}  // namespace obd::source
