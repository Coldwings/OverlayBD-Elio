// Merged writable view. See merged_writable.hpp.
#include "format/merged_writable.hpp"

#include "common/errors.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>

namespace obd::format {

namespace {
constexpr uint64_t kSector = lsmt::kAlignment;

struct SyncMutexGuard {
    elio::sync::mutex* mu = nullptr;

    explicit SyncMutexGuard(elio::sync::mutex& m) : mu(&m) {}
    ~SyncMutexGuard() {
        if (mu != nullptr) mu->unlock();
    }

    SyncMutexGuard(const SyncMutexGuard&) = delete;
    SyncMutexGuard& operator=(const SyncMutexGuard&) = delete;
};

struct AdoptActiveOp {};

struct ActiveOpGuard {
    std::atomic<uint32_t>& active;

    explicit ActiveOpGuard(std::atomic<uint32_t>& a, AdoptActiveOp)
        : active(a) {}
    ~ActiveOpGuard() { active.fetch_sub(1, std::memory_order_acq_rel); }

    ActiveOpGuard(const ActiveOpGuard&) = delete;
    ActiveOpGuard& operator=(const ActiveOpGuard&) = delete;
};

struct BoolResetGuard {
    std::atomic<bool>& flag;
    std::mutex& mu;
    bool armed = true;

    BoolResetGuard(std::atomic<bool>& f, std::mutex& m) : flag(f), mu(m) {}
    ~BoolResetGuard() {
        if (armed) {
            std::lock_guard lock(mu);
            flag.store(false, std::memory_order_release);
        }
    }

    BoolResetGuard(const BoolResetGuard&) = delete;
    BoolResetGuard& operator=(const BoolResetGuard&) = delete;
};
}

int MergedWritable::begin_data_op(uint64_t offset, uint64_t len) {
    std::lock_guard lock(state_gate_mu_);
    if (grow_active_.load(std::memory_order_acquire)) return -EBUSY;
    const uint64_t cur_vsize = vsize_.load(std::memory_order_acquire);
    if (offset > cur_vsize || len > cur_vsize - offset) return -EINVAL;
    active_ops_.fetch_add(1, std::memory_order_acq_rel);
    return 0;
}

int MergedWritable::begin_flush_op() {
    std::lock_guard lock(state_gate_mu_);
    if (grow_active_.load(std::memory_order_acquire)) return -EBUSY;
    active_ops_.fetch_add(1, std::memory_order_acq_rel);
    return 0;
}

int MergedWritable::begin_grow_op() {
    std::lock_guard lock(state_gate_mu_);
    if (grow_active_.load(std::memory_order_acquire) ||
        active_ops_.load(std::memory_order_acquire) != 0) {
        return -EBUSY;
    }
    grow_active_.store(true, std::memory_order_release);
    return 0;
}

void MergedWritable::end_data_op() {
    active_ops_.fetch_sub(1, std::memory_order_acq_rel);
}

void MergedWritable::end_grow_op() {
    std::lock_guard lock(state_gate_mu_);
    grow_active_.store(false, std::memory_order_release);
}

elio::coro::task<std::unique_ptr<MergedWritable>> MergedWritable::open(
    std::vector<std::unique_ptr<LsmtLayer>> layers_bottom_up,
    std::unique_ptr<WritableLayer> top) {
    if (!top) throw error(EINVAL, "writable top layer required");
    auto out = std::unique_ptr<MergedWritable>(new MergedWritable);
    // Reverse to topmost-first, mirroring MergedLsmt.
    while (!layers_bottom_up.empty()) {
        out->layers_.push_back(std::move(layers_bottom_up.back()));
        layers_bottom_up.pop_back();
    }
    uint64_t vsize = top->virtual_size();
    for (const auto& l : out->layers_) {
        vsize = std::max(vsize, l->virtual_size());
    }
    out->vsize_.store(vsize, std::memory_order_release);
    out->top_ = std::move(top);
    out->label_ = "merged-writable(" + std::to_string(out->layers_.size() + 1) +
                  " layers)";
    const int rebuild = out->rebuild_index();
    if (rebuild != 0) {
        throw error(-rebuild, "merged writable index rebuild failed");
    }
    co_return out;
}

void MergedWritable::invalidate_index() {
    std::lock_guard lock(index_mu_);
    index_.reset();
}

int MergedWritable::rebuild_index() {
    try {
        auto top_snapshot = top_->segments_snapshot();
        std::vector<const std::vector<bytes::segment_mapping>*> stack;
        stack.reserve(layers_.size() + 1);
        stack.push_back(&top_snapshot);  // tag 0 = writable top
        for (const auto& l : layers_) stack.push_back(&l->segments());
        std::vector<bytes::segment_mapping> next;
        MergedLsmt::merge_indexes(stack, next);
        auto published =
            std::make_shared<const std::vector<bytes::segment_mapping>>(
                std::move(next));
        std::lock_guard lock(index_mu_);
        index_ = std::move(published);
        return 0;
    } catch (const error& e) {
        // A write/discard may have already changed the top layer. Never keep
        // the previous merged snapshot after a rebuild failure: it could expose
        // lower bytes for a range now owned or masked by the top. An invalidated
        // index makes later reads fail closed with -EIO.
        invalidate_index();
        return -e.errno_value();
    } catch (const std::bad_alloc&) {
        invalidate_index();
        return -ENOMEM;
    } catch (...) {
        invalidate_index();
        return -EIO;
    }
}

elio::coro::task<ssize_t> MergedWritable::pread(void* buf, size_t count,
                                                uint64_t offset) {
    if ((offset % kSector) != 0 || (count % kSector) != 0) {
        co_return -EINVAL;
    }
    const uint64_t cur_vsize = vsize_.load(std::memory_order_acquire);
    if (offset >= cur_vsize) co_return 0;
    if (count > cur_vsize - offset) count = static_cast<size_t>(cur_vsize - offset);
    if (count == 0) co_return 0;

    const uint64_t start_sec = offset / kSector;
    const uint64_t end_sec = (offset + count) / kSector;
    uint8_t* out = static_cast<uint8_t*>(buf);

    std::shared_ptr<const std::vector<bytes::segment_mapping>> index_snapshot;
    {
        std::lock_guard lock(index_mu_);
        index_snapshot = index_;
    }
    if (!index_snapshot) co_return -EIO;
    const auto& index = *index_snapshot;
    auto it = std::upper_bound(
        index.begin(), index.end(), start_sec,
        [](uint64_t x, const bytes::segment_mapping& s) {
            return x < s.end();
        });

    uint64_t cur = start_sec;
    while (cur < end_sec) {
        if (it == index.end() || it->offset >= end_sec) {
            std::memset(out + (cur - start_sec) * kSector, 0,
                        static_cast<size_t>(end_sec - cur) * kSector);
            break;
        }
        if (it->offset > cur) {
            const uint64_t h = std::min(it->offset, end_sec);
            std::memset(out + (cur - start_sec) * kSector, 0,
                        static_cast<size_t>(h - cur) * kSector);
            cur = h;
            continue;
        }
        const uint64_t seg_from = std::max(it->offset, cur);
        const uint64_t seg_to = std::min(it->end(), end_sec);
        const size_t piece_bytes =
            static_cast<size_t>(seg_to - seg_from) * kSector;
        uint8_t* piece = out + (seg_from - start_sec) * kSector;
        if (it->zeroed) {
            std::memset(piece, 0, piece_bytes);
        } else {
            // tag 0 is the writable top; tags 1..n index layers_.
            source::BlobSource& src =
                it->tag == 0 ? top_->data_source()
                             : layers_[it->tag - 1]->data_source();
            const uint64_t data_off =
                (it->moffset + (seg_from - it->offset)) * kSector;
            const ssize_t r = co_await src.pread(piece, piece_bytes, data_off);
            if (r < 0) co_return r;
            if (static_cast<size_t>(r) < piece_bytes) {
                const ssize_t r2 = co_await src.pread(
                    piece + r, piece_bytes - static_cast<size_t>(r),
                    data_off + static_cast<uint64_t>(r));
                const size_t filled =
                    static_cast<size_t>(std::max<ssize_t>(r2, 0)) +
                    static_cast<size_t>(r);
                if (filled < piece_bytes) {
                    std::memset(piece + filled, 0, piece_bytes - filled);
                }
            }
        }
        cur = seg_to;
        if (it->end() <= cur) ++it;
    }
    co_return static_cast<ssize_t>(count);
}

elio::coro::task<ssize_t> MergedWritable::pwrite(const void* buf,
                                                 size_t count,
                                                 uint64_t offset) {
    const int gate = begin_data_op(offset, count);
    if (gate != 0) co_return gate;
    ActiveOpGuard active(active_ops_, AdoptActiveOp{});
    if ((offset % kSector) != 0 || (count % kSector) != 0) {
        co_return -EINVAL;
    }
    co_await op_mu_.lock();
    SyncMutexGuard op_guard(op_mu_);
    const ssize_t r = co_await top_->pwrite(buf, count, offset);
    // Rebuild even on error: writable tops can publish complete sector-aligned
    // prefixes before returning a later failure.
    const int rebuild = rebuild_index();
    if (r < 0) co_return r;
    if (rebuild != 0) co_return rebuild;
    co_return r;
}

elio::coro::task<int> MergedWritable::discard(uint64_t offset,
                                              uint64_t len) {
    const int gate = begin_data_op(offset, len);
    if (gate != 0) co_return gate;
    ActiveOpGuard active(active_ops_, AdoptActiveOp{});
    if ((offset % kSector) != 0 || (len % kSector) != 0) co_return -EINVAL;
    co_await op_mu_.lock();
    SyncMutexGuard op_guard(op_mu_);
    const int r = co_await top_->discard(offset, len);
    // Rebuild even on error: sparse discard can publish the protective sidecar
    // mask before a later durability step reports failure.
    const int rebuild = rebuild_index();
    if (r != 0) co_return r;
    if (rebuild != 0) co_return rebuild;
    co_return 0;
}

elio::coro::task<int> MergedWritable::flush() {
    const int gate = begin_flush_op();
    if (gate != 0) co_return gate;
    ActiveOpGuard active(active_ops_, AdoptActiveOp{});
    co_await op_mu_.lock();
    SyncMutexGuard op_guard(op_mu_);
    co_return co_await top_->flush();
}

int MergedWritable::grow(uint64_t vsize) {
    // D3 grow-only vsize extension (see MergedWritable::grow doc in the
    // header). BLOCKING: run off an Elio worker via elio::spawn_blocking
    // — the device resize executor does. The top must accept the new
    // range before the merged view widens, so writes into the headroom
    // land in the writable upper.
    if (vsize == 0 || vsize % kSector != 0) return -EINVAL;
    const int gate = begin_grow_op();
    if (gate != 0) return gate;
    BoolResetGuard grow_guard(grow_active_, state_gate_mu_);
    const uint64_t cur = vsize_.load(std::memory_order_acquire);
    if (vsize < cur) return -EINVAL;  // grow-only (equal = no-op)
    if (vsize == cur) return 0;
    if (vsize > top_->virtual_size()) {
        const int r = top_->grow(vsize);
        if (r != 0) return r;
    }
    vsize_.store(vsize, std::memory_order_release);
    grow_guard.armed = false;
    end_grow_op();
    return 0;
}

}  // namespace obd::format
