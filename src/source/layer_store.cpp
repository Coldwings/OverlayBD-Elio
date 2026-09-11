// LayerStore. See layer_store.hpp.
#include "source/layer_store.hpp"

#include "common/bytes.hpp"
#include "common/errors.hpp"
#include "common/sha256.hpp"

#include <elio/io/io_awaitables.hpp>
#include <elio/log/macros.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/runtime/spawn_blocking.hpp>
#include <elio/time/timer.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>

namespace obd::source {

namespace {

constexpr char kSidecarMagic[8] = {'O', 'B', 'D', 'S', 'I', 'D', 'E', '1'};
constexpr uint32_t kSidecarVersion = 1;
constexpr size_t kHeaderSize = 80;  // fixed, little-endian field layout below

// Blocking write loop — writer-thread / cold-path use only, never on an
// Elio worker. Returns 0 or an errno.
int pwrite_all(int fd, const void* buf, size_t count, uint64_t offset) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t done = 0;
    while (done < count) {
        const ssize_t w =
            ::pwrite(fd, p + done, count - done,
                     static_cast<off_t>(offset + done));
        if (w < 0) {
            if (errno == EINTR) continue;
            return errno;
        }
        if (w == 0) return EIO;
        done += static_cast<size_t>(w);
    }
    return 0;
}

// Cold-path read loop; returns bytes read (short at EOF) or -errno.
ssize_t pread_upto(int fd, void* buf, size_t count, uint64_t offset) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < count) {
        const ssize_t r =
            ::pread(fd, p + done, count - done,
                    static_cast<off_t>(offset + done));
        if (r < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (r == 0) break;
        done += static_cast<size_t>(r);
    }
    return static_cast<ssize_t>(done);
}

uint32_t crc32_of(const void* buf, size_t len) {
    return static_cast<uint32_t>(::crc32(
        0L, reinterpret_cast<const Bytef*>(buf), static_cast<uInt>(len)));
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Normalizes an image-config digest: strips an optional "sha256:" prefix,
// lowercases, and validates 64 hex chars. Empty input stays empty (no
// verification). Fills `raw` with the 32 digest bytes when non-empty.
std::string normalize_digest(std::string hex,
                             std::array<uint8_t, 32>& raw) {
    constexpr std::string_view kPrefix = "sha256:";
    if (hex.compare(0, kPrefix.size(), kPrefix) == 0) {
        hex.erase(0, kPrefix.size());
    }
    if (hex.empty()) return {};
    if (hex.size() != 64) {
        throw error(EINVAL, "malformed sha256 digest: " + hex);
    }
    for (size_t i = 0; i < 32; ++i) {
        const int hi = hex_digit(hex[2 * i]);
        const int lo = hex_digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            throw error(EINVAL, "malformed sha256 digest: " + hex);
        }
        raw[i] = static_cast<uint8_t>((hi << 4) | lo);
        hex[2 * i] = "0123456789abcdef"[hi];
        hex[2 * i + 1] = "0123456789abcdef"[lo];
    }
    return hex;
}

// Parses "<prefix><16 lowercase/any hex chars>" into a nonce; 0 = no match.
uint64_t parse_nonce_name(const std::string& name, std::string_view prefix) {
    if (name.size() != prefix.size() + 16) return 0;
    if (name.compare(0, prefix.size(), prefix) != 0) return 0;
    uint64_t nonce = 0;
    for (size_t i = prefix.size(); i < name.size(); ++i) {
        const int d = hex_digit(name[i]);
        if (d < 0) return 0;
        nonce = (nonce << 4) | static_cast<uint64_t>(d);
    }
    return nonce == 0 ? 0 : nonce;  // 0 is the "no pair" sentinel
}

// Unlinks every stale staging/sidecar pair file in `dir` (the commit-bind
// sweep: once overlaybd.commit exists the probe binds it first, so a
// leftover pair can never be resumed — it is pure disk leak). Returns the
// number of files removed. Blocking; cold paths only.
size_t sweep_stale_pairs(const std::string& dir) noexcept {
    size_t removed = 0;
    try {
        std::error_code ec;
        std::filesystem::directory_iterator it(dir, ec);
        const std::filesystem::directory_iterator end;
        while (!ec && it != end) {
            const std::filesystem::path path = it->path();
            const std::string name = path.filename().string();
            if (name.compare(0, 10, ".download.") == 0 ||
                name.compare(0, 8, ".bitmap.") == 0) {
                if (::unlink(path.c_str()) == 0) ++removed;
            }
            it.increment(ec);
        }
    } catch (...) {
        // Best-effort hygiene must never make image assembly fail.
    }
    return removed;
}

}  // namespace

size_t sweep_stale_layer_store_pairs(const std::string& dir) noexcept {
    return sweep_stale_pairs(dir);
}

std::string LayerStore::hex_nonce(uint64_t nonce) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx",
                  static_cast<unsigned long long>(nonce));
    return buf;
}

elio::coro::task<std::unique_ptr<LayerStore>> LayerStore::open(
    BlobSourcePtr remote, std::string dir, std::string expected_sha256_hex) {
    co_return co_await open(std::move(remote), std::move(dir),
                            std::move(expected_sha256_hex), Config{});
}

elio::coro::task<std::unique_ptr<LayerStore>> LayerStore::open(
    BlobSourcePtr remote, std::string dir, std::string expected_sha256_hex,
    Config cfg) {
    if (!remote) throw error(EINVAL, "layer store with null remote");
    if (cfg.extent_size == 0) {
        throw error(EINVAL, "layer store with zero extent size");
    }
    if (cfg.queue_max_bytes < cfg.extent_size) {
        throw error(EINVAL, "layer store queue smaller than one extent");
    }
    if (cfg.try_count == 0) {
        throw error(EINVAL, "layer store with zero try count");
    }
    auto ls = std::unique_ptr<LayerStore>(new LayerStore());
    ls->cfg_ = cfg;
    ls->dir_ = std::move(dir);
    ls->size_ = remote->size();
    ls->label_ = "layer-store(" + std::string(remote->label()) + ")";
    ls->expected_hex_ =
        normalize_digest(std::move(expected_sha256_hex), ls->expected_raw_);
    ls->extent_count_ =
        (ls->size_ + cfg.extent_size - 1) / cfg.extent_size;
    ls->records_ = std::vector<std::atomic<uint64_t>>(ls->extent_count_);
    ls->remote_ = std::move(remote);

    co_await elio::spawn_blocking([self = ls.get()] {
        struct stat st {};
        if (::stat(self->dir_.c_str(), &st) != 0) {
            throw_errno(errno, "layer store dir " + self->dir_);
        }
        if (!S_ISDIR(st.st_mode)) {
            throw error(ENOTDIR, "layer store dir " + self->dir_);
        }
        if (::access(self->dir_.c_str(), W_OK | X_OK) != 0) {
            throw_errno(errno, "layer store dir not writable " + self->dir_);
        }

        // A committed layer from a previous run binds read-only; no staging.
        const std::string commit = LayerStore::commit_path(self->dir_);
        if (::stat(commit.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            const int fd = ::open(commit.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) throw_errno(errno, "cannot open " + commit);
            self->commit_fd_.store(fd, std::memory_order_release);
            self->state_.store(static_cast<int>(State::Complete),
                               std::memory_order_release);
            // Cheap hygiene: a pair left beside the commit can never win
            // the probe — sweep it.
            const size_t swept = sweep_stale_layer_store_pairs(self->dir_);
            ELIO_LOG_INFO(
                "layer store {}: bound to commit file ({} stale pair "
                "files swept)",
                self->dir_, swept);
            return;
        }

        const uint64_t resumed = self->find_valid_pair();
        if (resumed == 0) {
            LayerStore::FreshPair p;
            const int rc = self->create_fresh_pair(p);
            if (rc != 0) {
                throw_errno(rc, "cannot create staging pair in " + self->dir_);
            }
            self->nonce_ = p.nonce;
            self->staging_fd_.store(p.staging_fd, std::memory_order_release);
            self->sidecar_fd_ = p.sidecar_fd;
            ELIO_LOG_INFO("layer store {}: fresh staging pair (nonce {})",
                          self->dir_, LayerStore::hex_nonce(self->nonce_));
        } else {
            ELIO_LOG_INFO("layer store {}: resuming nonce {} ({}/{} extents)",
                          self->dir_, LayerStore::hex_nonce(resumed),
                          self->present_.load(std::memory_order_relaxed),
                          self->extent_count_);
        }
        self->kick_completion_check_ =
            self->extent_count_ == 0 ||
            self->present_.load(std::memory_order_relaxed) ==
                self->extent_count_;
    });
    if (ls->state() == State::Complete) co_return ls;
    LayerStore* self = ls.get();
    ls->writer_ = std::thread([self] { self->writer_main(); });
    if (ls->cfg_.fill.enable && ls->extent_count_ > 0) {
        elio::go([self]() -> elio::coro::task<void> {
            co_await self->run_fill();
        });
    }
    co_return ls;
}

LayerStore::~LayerStore() {
    if (writer_.joinable()) {
        {
            std::lock_guard lk(qmu_);
            stopping_ = true;
        }
        qcv_.notify_all();
        writer_.join();
    }
    if (sidecar_fd_ >= 0) ::close(sidecar_fd_);
    for (const int fd : retired_fds_) elio::io::close_fd_for_destructor(fd);
    const int sfd = staging_fd_.load(std::memory_order_relaxed);
    if (sfd >= 0) elio::io::close_fd_for_destructor(sfd);
    const int cfd = commit_fd_.load(std::memory_order_acquire);
    if (cfd >= 0 && cfd != sfd) {
        elio::io::close_fd_for_destructor(cfd);
    }
}

void LayerStore::set_test_write_hook(
    std::function<int(uint64_t extent_id)> hook) {
    std::lock_guard lk(qmu_);
    write_hook_ = std::move(hook);
}

void LayerStore::set_test_fetch_done_hook(std::function<void()> hook) {
    // Armed before any coroutine runs (tests only), so no locking.
    fetch_done_hook_ = std::move(hook);
}

// ---------------------------------------------------------------------------
// Setup / recovery (cold paths)
// ---------------------------------------------------------------------------

int LayerStore::create_fresh_pair(FreshPair& out) {
    std::random_device rd;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint64_t nonce =
            (static_cast<uint64_t>(rd()) << 32) | rd();
        if (nonce == 0) continue;
        const std::string sp = dir_ + "/.download." + hex_nonce(nonce);
        const std::string bp = dir_ + "/.bitmap." + hex_nonce(nonce);
        const int fd =
            ::open(sp.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return errno;
        }
        if (::ftruncate(fd, static_cast<off_t>(size_)) != 0) {
            const int e = errno;
            ::close(fd);
            ::unlink(sp.c_str());
            return e;
        }
        const int bfd =
            ::open(bp.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (bfd < 0) {
            const int e = errno;
            ::close(fd);
            ::unlink(sp.c_str());
            if (e == EEXIST) continue;
            return e;
        }
        uint8_t hdr[kHeaderSize] = {};
        std::memcpy(hdr, kSidecarMagic, sizeof kSidecarMagic);
        bytes::store_u32_le(hdr + 8, kSidecarVersion);
        bytes::store_u32_le(hdr + 12, cfg_.extent_size);
        bytes::store_u64_le(hdr + 16, size_);
        bytes::store_u64_le(hdr + 24, extent_count_);
        std::memcpy(hdr + 32, expected_raw_.data(), expected_raw_.size());
        bytes::store_u64_le(hdr + 64, nonce);
        bytes::store_u32_le(hdr + 72, crc32_of(hdr, 72));
        // [76,80) reserved, zero
        int rc = pwrite_all(bfd, hdr, sizeof hdr, 0);
        if (rc == 0 &&
            ::ftruncate(bfd, static_cast<off_t>(kHeaderSize +
                                                extent_count_ * 8)) != 0) {
            rc = errno;
        }
        if (rc != 0) {
            ::close(bfd);
            ::close(fd);
            ::unlink(sp.c_str());
            ::unlink(bp.c_str());
            return rc;
        }
        out.nonce = nonce;
        out.staging_fd = fd;
        out.sidecar_fd = bfd;
        return 0;
    }
    return EEXIST;  // repeated nonce collisions
}

uint64_t LayerStore::find_valid_pair() {
    std::map<uint64_t, std::string> stagings;
    std::map<uint64_t, std::string> sidecars;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(dir_, ec)) {
        const std::string name = entry.path().filename().string();
        if (const uint64_t n = parse_nonce_name(name, ".download.")) {
            stagings.emplace(n, entry.path().string());
        } else if (const uint64_t n = parse_nonce_name(name, ".bitmap.")) {
            sidecars.emplace(n, entry.path().string());
        }
    }
    std::vector<std::string> stale;
    for (const auto& [n, p] : stagings) {
        if (!sidecars.count(n)) stale.push_back(p);
    }
    for (const auto& [n, p] : sidecars) {
        if (!stagings.count(n)) stale.push_back(p);
    }
    uint64_t found = 0;
    for (const auto& [n, p] : stagings) {
        const auto it = sidecars.find(n);
        if (it == sidecars.end()) continue;
        if (found == 0 && try_load_pair(n, p, it->second)) {
            found = n;
        } else {
            stale.push_back(p);
            stale.push_back(it->second);
        }
    }
    for (const std::string& p : stale) {
        ELIO_LOG_WARNING("layer store {}: deleting stale staging file {}",
                         dir_, p);
        ::unlink(p.c_str());
    }
    return found;
}

bool LayerStore::try_load_pair(uint64_t nonce, const std::string& staging,
                               const std::string& sidecar) {
    const int bfd = ::open(sidecar.c_str(), O_RDWR | O_CLOEXEC);
    if (bfd < 0) return false;
    uint8_t hdr[kHeaderSize];
    bool ok = pread_upto(bfd, hdr, sizeof hdr, 0) ==
              static_cast<ssize_t>(sizeof hdr);
    if (ok) {
        ok = std::memcmp(hdr, kSidecarMagic, sizeof kSidecarMagic) == 0 &&
             bytes::load_u32_le(hdr + 8) == kSidecarVersion &&
             bytes::load_u32_le(hdr + 12) == cfg_.extent_size &&
             bytes::load_u64_le(hdr + 16) == size_ &&
             bytes::load_u64_le(hdr + 24) == extent_count_ &&
             std::memcmp(hdr + 32, expected_raw_.data(),
                         expected_raw_.size()) == 0 &&
             bytes::load_u64_le(hdr + 64) == nonce &&
             bytes::load_u32_le(hdr + 72) == crc32_of(hdr, 72);
    }
    int sfd = -1;
    if (ok) {
        sfd = ::open(staging.c_str(), O_RDWR | O_CLOEXEC);
        if (sfd < 0) ok = false;
    }
    if (ok) {
        struct stat st {};
        if (::fstat(sfd, &st) != 0 || !S_ISREG(st.st_mode) ||
            static_cast<uint64_t>(st.st_size) != size_) {
            ok = false;
        }
    }
    uint64_t present = 0;
    std::vector<uint64_t> recs;
    if (ok) {
        recs.resize(extent_count_);
        std::vector<uint8_t> raw(extent_count_ * 8);
        // A short read is fine: records never written read back as zeros.
        const ssize_t got =
            pread_upto(bfd, raw.data(), raw.size(), kHeaderSize);
        if (got < 0) {
            ok = false;
        } else {
            std::memset(raw.data() + got, 0, raw.size() - got);
            for (uint64_t i = 0; i < extent_count_; ++i) {
                const uint32_t crc = bytes::load_u32_le(raw.data() + i * 8);
                const uint32_t flags =
                    bytes::load_u32_le(raw.data() + i * 8 + 4);
                recs[i] = (static_cast<uint64_t>(crc) << 32) |
                          (flags & kFlagPresent);
                if (flags & kFlagPresent) ++present;
            }
        }
    }
    if (!ok) {
        if (sfd >= 0) ::close(sfd);
        ::close(bfd);
        return false;
    }
    for (uint64_t i = 0; i < extent_count_; ++i) {
        records_[i].store(recs[i], std::memory_order_relaxed);
    }
    present_.store(present, std::memory_order_relaxed);
    nonce_ = nonce;
    staging_fd_.store(sfd, std::memory_order_release);
    sidecar_fd_ = bfd;
    return true;
}

// ---------------------------------------------------------------------------
// Hot paths (coroutines; -errno results; never throw)
// ---------------------------------------------------------------------------

elio::coro::task<ssize_t> LayerStore::read_fd_loop(int fd, void* buf,
                                                   size_t count,
                                                   uint64_t offset) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < count) {
        const auto r = co_await elio::io::async_read(
            fd, p + done, count - done,
            static_cast<int64_t>(offset + done));
        if (r.result < 0) {
            co_return done > 0 ? static_cast<ssize_t>(done) : r.result;
        }
        if (r.result == 0) break;  // EOF (file shrank underneath us)
        done += static_cast<size_t>(r.result);
    }
    co_return static_cast<ssize_t>(done);
}

elio::coro::task<LayerStore::FetchResult> LayerStore::join_or_fetch(
    uint64_t extent_id, ReadClass cls) {
    std::shared_ptr<InFlight> f;
    bool starter = false;
    co_await inflight_mu_.lock();
    auto it = inflight_.find(extent_id);
    if (it != inflight_.end()) {
        f = it->second;
    } else if (cls == ReadClass::OnDemand) {
        // OnDemand admission is unconditional and cannot queue. Publish
        // before acquiring its permit so concurrent same-extent readers
        // still coalesce behind one remote fetch.
        f = std::make_shared<InFlight>();
        inflight_.emplace(extent_id, f);
        starter = true;
    }
    inflight_mu_.unlock();

    if (f && !starter) {
        coalesced_joins_.fetch_add(1, std::memory_order_relaxed);
        co_await f->done.wait();
        co_return FetchResult{f->data, f->error};
    }

    AdmissionFunnel::Permit permit;
    if (cls != ReadClass::OnDemand) {
        // A scavenger request that has not yet passed admission is not
        // published in the in-flight map. That keeps same-extent OnDemand
        // misses unconditional: they may issue their own fetch instead of
        // joining a queued warm-up and inheriting its wait or EAGAIN skip.
        if (cfg_.funnel && cls == ReadClass::Prefetch &&
            cfg_.populate_admit_timeout.count() > 0) {
            auto bounded = co_await cfg_.funnel->acquire_scavenger_bounded(
                cls, cfg_.populate_admit_timeout);
            if (!bounded) {
                co_return FetchResult{nullptr, EAGAIN};
            }
            permit = std::move(*bounded);
        } else if (cfg_.funnel) {
            permit = co_await cfg_.funnel->acquire(cls);
        }

        // Another class may have issued and persisted this extent while
        // Prefetch waited. Re-check the local bitmap after admission, before
        // publishing new remote work, so a queued scavenger does not fetch
        // bytes OnDemand already made present.
        if (state() != State::Filling ||
            (records_[extent_id].load(std::memory_order_acquire) &
             kFlagPresent)) {
            co_return FetchResult{nullptr, 0};
        }

        // Another class may have issued this extent while Prefetch waited but
        // not yet made it present. Join that already-issued work and give back
        // the scavenger slot instead of starting a duplicate remote request.
        co_await inflight_mu_.lock();
        it = inflight_.find(extent_id);
        if (it != inflight_.end()) {
            f = it->second;
        } else {
            f = std::make_shared<InFlight>();
            inflight_.emplace(extent_id, f);
            starter = true;
        }
        inflight_mu_.unlock();

        if (!starter) {
            permit.reset();
            coalesced_joins_.fetch_add(1, std::memory_order_relaxed);
            co_await f->done.wait();
            co_return FetchResult{f->data, f->error};
        }
    } else if (cfg_.funnel) {
        permit = co_await cfg_.funnel->acquire(cls);
    }

    const uint64_t ebase = extent_id * cfg_.extent_size;
    const size_t elen = static_cast<size_t>(
        std::min<uint64_t>(cfg_.extent_size, size_ - ebase));
    auto buf = std::make_shared<std::vector<uint8_t>>(elen);
    // ADR-0012: the starter's remote fetch enters the source only
    // through the device's admission funnel (the permit's lifetime is
    // the fetch — its wall-clock latency is the AIMD sample for
    // OnDemand). One extent is 64 KiB, under the scavenger size cap.
    // A populate (Prefetch) fetch additionally honors
    // populate_admit_timeout (issue #35): a gate closed for longer than
    // that skips the extent with EAGAIN instead of waiting. Because
    // Prefetch publishes no in-flight entry until after admission, that
    // local skip is never inherited by a same-extent OnDemand miss.
    ssize_t r;
    remote_fetches_.fetch_add(1, std::memory_order_relaxed);
    r = co_await remote_->pread(buf->data(), elen, ebase);
    // The permit covers the remote fetch alone — its lifetime is the
    // latency sample (the same contract as run_fill's): release the
    // window slot before the completion bookkeeping below, so a queued
    // scavenger never waits behind the in-flight map update.
    permit.reset();
    if (fetch_done_hook_) fetch_done_hook_();  // test-only
    if (r < 0) {
        f->error = static_cast<int>(-r);
    } else if (static_cast<size_t>(r) != elen) {
        f->error = EIO;  // short fill from the remote
    } else {
        f->data = std::move(buf);
    }
    co_await inflight_mu_.lock();
    // Signal completion BEFORE retiring the entry, in the same critical
    // section: a same-extent caller then either finds the entry (done
    // already signaled — its wait returns immediately with the result)
    // or finds nothing (the fetch is complete and its joiners were
    // released). The previous erase-then-set order left a window —
    // hittable once the scheduler runs several workers — in which a
    // caller missed the retired entry and started a duplicate remote
    // fetch for bytes just fetched, the waste extent dedup exists to
    // prevent (ADR-0012). done.set() only collects and schedules
    // waiters (never blocks, leaf mutex), so signaling under
    // inflight_mu_ cannot deadlock. Joiners hold their own
    // shared_ptr<InFlight>, so the event stays alive until every waiter
    // has been scheduled away from it.
    f->done.set();
    inflight_.erase(extent_id);
    inflight_mu_.unlock();
    co_return FetchResult{f->data, f->error};
}

void LayerStore::enqueue_write(
    uint64_t extent_id, std::shared_ptr<const std::vector<uint8_t>> data,
    size_t data_offset) {
    const uint64_t ebase = extent_id * cfg_.extent_size;
    const size_t elen = static_cast<size_t>(
        std::min<uint64_t>(cfg_.extent_size, size_ - ebase));
    const uint32_t crc = crc32_of(data->data() + data_offset, elen);
    std::lock_guard lk(qmu_);
    if (stopping_ || state() != State::Filling) return;
    if (queued_bytes_ + elen > cfg_.queue_max_bytes) {
        dropped_writes_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    queued_bytes_ += elen;
    queue_.push_back(
        WriteJob{extent_id, std::move(data), data_offset, elen, crc, false});
    qcv_.notify_one();
}

void LayerStore::enqueue_clear(uint64_t extent_id) {
    std::lock_guard lk(qmu_);
    if (stopping_ || state() != State::Filling) return;
    // Bookkeeping, not payload: never dropped for queue fullness.
    queue_.push_back(WriteJob{extent_id, nullptr, 0, 0, 0, true});
    qcv_.notify_one();
}

elio::coro::task<ssize_t> LayerStore::pread(void* buf, size_t count,
                                            uint64_t offset) {
    if (offset >= size_) co_return 0;
    if (count > size_ - offset) {
        count = static_cast<size_t>(size_ - offset);
    }
    if (count == 0) co_return 0;

    if (state() == State::Complete) {
        co_return co_await read_fd_loop(
            commit_fd_.load(std::memory_order_acquire), buf, count, offset);
    }

    const uint64_t es = cfg_.extent_size;
    uint8_t* out = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < count) {
        const uint64_t pos = offset + done;
        const uint64_t eid = pos / es;
        const uint64_t ebase = eid * es;
        const uint64_t eend = std::min(ebase + es, size_);
        const size_t within = static_cast<size_t>(pos - ebase);
        const size_t need =
            static_cast<size_t>(std::min(eend, offset + count) - pos);
        const size_t elen = static_cast<size_t>(eend - ebase);

        bool served = false;
        if (state() == State::Filling) {
            const uint64_t rec =
                records_[eid].load(std::memory_order_acquire);
            if (rec & kFlagPresent) {
                const uint32_t expected_crc = static_cast<uint32_t>(rec >> 32);
                // Fast path: a request covering the whole extent CRCs the
                // caller's buffer directly, avoiding a copy and an alloc.
                std::vector<uint8_t> tmp;
                void* dst = out + done;
                if (within != 0 || need != elen) {
                    tmp.resize(elen);
                    dst = tmp.data();
                }
                const ssize_t r = co_await read_fd_loop(
                    staging_fd_.load(std::memory_order_acquire), dst, elen,
                    ebase);
                if (r < 0) {
                    co_return done > 0 ? static_cast<ssize_t>(done) : r;
                }
                if (static_cast<size_t>(r) == elen &&
                    crc32_of(dst, elen) == expected_crc) {
                    if (dst != out + done) {
                        std::memcpy(out + done,
                                    static_cast<const uint8_t*>(dst) + within,
                                    need);
                    }
                    served = true;
                } else {
                    // Corrupt or torn extent: demote to a hole and re-fetch.
                    crc_failures_.fetch_add(1, std::memory_order_relaxed);
                    const uint64_t old = records_[eid].exchange(
                        0, std::memory_order_acq_rel);
                    if (old & kFlagPresent) {
                        present_.fetch_sub(1, std::memory_order_relaxed);
                    }
                    enqueue_clear(eid);
                }
            }
        }
        if (!served) {
            // A guest-blocking miss: the ADR-0012 OnDemand class.
            const FetchResult fr =
                co_await join_or_fetch(eid, ReadClass::OnDemand);
            if (fr.error != 0) {
                co_return done > 0 ? static_cast<ssize_t>(done)
                                   : -fr.error;
            }
            std::memcpy(out + done, fr.data->data() + within, need);
            if (state() == State::Filling &&
                !(records_[eid].load(std::memory_order_acquire) &
                  kFlagPresent)) {
                enqueue_write(eid, fr.data);
            }
        }
        done += need;
    }
    co_return static_cast<ssize_t>(done);
}

elio::coro::task<ssize_t> LayerStore::populate(uint64_t offset, size_t len) {
    if (state() != State::Filling) co_return 0;
    if (offset >= size_) co_return 0;
    const uint64_t end =
        offset + std::min<uint64_t>(len, size_ - offset);
    const uint64_t es = cfg_.extent_size;
    for (uint64_t eid = offset / es; eid * es < end; ++eid) {
        if (records_[eid].load(std::memory_order_acquire) & kFlagPresent) {
            continue;
        }
        // Warm-up (structural prefetch, trace replay): the ADR-0012
        // Prefetch scavenger class.
        const FetchResult fr =
            co_await join_or_fetch(eid, ReadClass::Prefetch);
        if (fr.error != 0) co_return -fr.error;
        if (state() == State::Filling &&
            !(records_[eid].load(std::memory_order_acquire) &
              kFlagPresent)) {
            enqueue_write(eid, fr.data);
        }
    }
    co_return 0;
}

// ---------------------------------------------------------------------------
// Background fill (one scavenger-class coroutine; ADR-0011 bulk path)
// ---------------------------------------------------------------------------

elio::coro::task<bool> LayerStore::wait_queue_room(size_t len) {
    for (;;) {
        {
            std::lock_guard lk(qmu_);
            if (stopping_ || state() != State::Filling ||
                fill_stop_.load(std::memory_order_acquire)) {
                co_return false;
            }
            if (queued_bytes_ + len <= cfg_.queue_max_bytes) co_return true;
        }
        // Scavenger back-pressure: fill may wait on the writer; readers
        // never do. 1 ms polling is cheap at concurrency 1.
        co_await elio::time::sleep_for(std::chrono::milliseconds(1));
    }
}

elio::coro::task<void> LayerStore::run_fill() {
    // Fill is the ADR-0012 `Fill` scavenger class: conservative
    // concurrency 1 (this single walk), admitted at the device's funnel
    // (when configured) behind both OnDemand and Prefetch traffic.
    fill_status_.store(static_cast<int>(FillStatus::kWaiting),
                       std::memory_order_release);
    uint32_t delay = cfg_.fill.delay_sec;
    if (cfg_.fill.delay_extra_sec != 0) {
        // std::random_device jitter (upstream download precedent); not part of the
        // determinism surface.
        std::random_device rd;
        delay += rd() % (cfg_.fill.delay_extra_sec + 1);
    }
    // Slept in 100 ms slices so stop_fill() (park_image_fills, teardown)
    // takes effect promptly even inside a long start delay.
    for (uint64_t slept_ms = 0;
         slept_ms < static_cast<uint64_t>(delay) * 1000 &&
         !fill_stop_.load(std::memory_order_acquire);
         slept_ms += 100) {
        co_await elio::time::sleep_for(std::chrono::milliseconds(100));
    }
    if (fill_stop_.load(std::memory_order_acquire) ||
        state() != State::Filling) {
        fill_status_.store(static_cast<int>(FillStatus::kStopped),
                           std::memory_order_release);
        co_return;
    }
    fill_status_.store(static_cast<int>(FillStatus::kFilling),
                       std::memory_order_release);
    ELIO_LOG_INFO("layer store {}: background fill starting ({} extents)",
                  dir_, extent_count_);

    const uint64_t es = cfg_.extent_size;
    // Coalescing cap: the bulk-path rule — contiguous misses are fetched
    // with larger range reads, capped near 1 MiB. With a funnel the cap
    // is the funnel's scavenger size cap (ADR-0012: the funnel caps, the
    // caller splits — one fill range read is one scavenger admission).
    uint64_t cap = std::min<uint64_t>(
        std::max<uint64_t>(cfg_.fill.block_size, es), 1024 * 1024);
    if (cfg_.funnel) {
        cap = cfg_.funnel->cap_request(ReadClass::Fill,
                                       static_cast<size_t>(cap));
    }
    const uint64_t budget =
        static_cast<uint64_t>(std::max(cfg_.fill.max_mbps, 1u)) << 20;
    uint64_t window_used = 0;
    auto window_start = std::chrono::steady_clock::now();
    unsigned consecutive_errors = 0;
    bool done = false;

    while (!fill_stop_.load(std::memory_order_acquire) &&
           state() == State::Filling) {
        // First missing extent, then the contiguous missing run after it.
        uint64_t e = 0;
        for (; e < extent_count_; ++e) {
            if (!(records_[e].load(std::memory_order_acquire) &
                  kFlagPresent)) {
                break;
            }
        }
        if (e == extent_count_) {
            done = true;  // nothing missing: completion is the writer's job
            break;
        }
        uint64_t run_end = e;
        while (run_end < extent_count_ &&
               (run_end - e) * es < cap &&
               !(records_[run_end].load(std::memory_order_acquire) &
                 kFlagPresent)) {
            ++run_end;
        }
        const uint64_t run_len =
            std::min((run_end - e) * es, size_ - e * es);
        auto buf = std::make_shared<std::vector<uint8_t>>(
            static_cast<size_t>(run_len));
        // ADR-0012: fill's range read is one Fill-class scavenger
        // admission; it may suspend here until no on-demand request is
        // in flight and the AIMD window has room.
        AdmissionFunnel::Permit permit;
        if (cfg_.funnel) {
            permit = co_await cfg_.funnel->acquire(ReadClass::Fill);
        }
        const ssize_t r =
            co_await remote_->pread(buf->data(), buf->size(), e * es);
        // The permit's lifetime is the remote fetch alone (the latency
        // sample): release the window slot BEFORE the write-behind
        // back-pressure, the error backoff, and the max_mbps throttle
        // sleep below — those are fill-local delays, and a queued
        // Prefetch (which outranks Fill) must not wait behind them.
        permit.reset();
        if (r < 0 || static_cast<uint64_t>(r) != run_len) {
            // Transient remote trouble: back off (1s doubling, capped at
            // 60s) and resume the walk — persisted extents survive in the
            // sidecar, so progress is never lost.
            ++consecutive_errors;
            const unsigned shift = std::min(consecutive_errors, 6u);
            ELIO_LOG_WARNING(
                "layer store {}: fill read failed at extent {} ({}); "
                "retrying in {}s",
                dir_, e,
                r < 0 ? strerror(static_cast<int>(-r)) : "short read",
                1u << shift);
            co_await elio::time::sleep_for(std::chrono::seconds(1u << shift));
            continue;
        }
        consecutive_errors = 0;
        for (uint64_t x = e; x < run_end; ++x) {
            if (records_[x].load(std::memory_order_acquire) &
                kFlagPresent) {
                continue;  // a reader beat us to it
            }
            const uint64_t xbase = x * es;
            const size_t elen = static_cast<size_t>(
                std::min<uint64_t>(es, size_ - xbase));
            const bool room =
                co_await wait_queue_room(elen);
            if (!room) {
                fill_status_.store(
                    static_cast<int>(FillStatus::kStopped),
                    std::memory_order_release);
                co_return;
            }
            enqueue_write(x, buf, static_cast<size_t>(xbase - e * es));
        }
        // Throughput throttle (the download contract's per-second budget window).
        window_used += run_len;
        if (window_used >= budget) {
            const auto elapsed =
                std::chrono::steady_clock::now() - window_start;
            if (elapsed < std::chrono::seconds(1)) {
                co_await elio::time::sleep_for(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::seconds(1) - elapsed));
            }
            window_start = std::chrono::steady_clock::now();
            window_used = 0;
        }
    }
    fill_status_.store(static_cast<int>(done ? FillStatus::kDone
                                             : FillStatus::kStopped),
                       std::memory_order_release);
    ELIO_LOG_INFO("layer store {}: background fill {}",
                  dir_, done ? "finished" : "stopped");
}

// ---------------------------------------------------------------------------
// Write-behind writer thread (blocking disk work lives here, never on an
// Elio worker — same precedent as the ublk queue threads)
// ---------------------------------------------------------------------------

void LayerStore::writer_main() {
    if (kick_completion_check_) {
        // A previous run filled every extent but died before the rename.
        kick_completion_check_ = false;
        complete_layer();
    }
    std::unique_lock lk(qmu_);
    for (;;) {
        qcv_.wait(lk, [&] { return stopping_ || !queue_.empty(); });
        if (stopping_) {
            // Bounded best-effort drain by dropping: pending writes are
            // only cache; teardown must never hang.
            queue_.clear();
            queued_bytes_ = 0;
            return;
        }
        WriteJob job = std::move(queue_.front());
        queue_.pop_front();
        if (job.data) queued_bytes_ -= job.len;
        std::function<int(uint64_t)> hook = write_hook_;
        lk.unlock();
        process_job(job, hook);
        lk.lock();
    }
}

void LayerStore::process_job(
    const WriteJob& job, const std::function<int(uint64_t)>& hook) {
    if (state() != State::Filling) return;  // bypass/complete: drop
    if (job.clear) {
        // Best-effort demote of a CRC-failed extent.
        uint8_t rec[8] = {};
        const int rc = pwrite_all(sidecar_fd_, rec, sizeof rec,
                                  kHeaderSize + job.extent_id * 8);
        if (rc != 0) {
            on_write_error(rc);
            return;
        }
        const uint64_t old = records_[job.extent_id].exchange(
            0, std::memory_order_acq_rel);
        if (old & kFlagPresent) {
            present_.fetch_sub(1, std::memory_order_relaxed);
        }
        return;
    }
    if (hook) {
        const int injected = hook(job.extent_id);
        if (injected != 0) {
            on_write_error(injected);
            return;
        }
    }
    // Consistency rule (ADR-0011): the record lands only after the data.
    int rc = pwrite_all(staging_fd_.load(std::memory_order_relaxed),
                        job.data->data() + job.data_offset, job.len,
                        job.extent_id * cfg_.extent_size);
    if (rc == 0) {
        uint8_t rec[8];
        bytes::store_u32_le(rec, job.crc);
        bytes::store_u32_le(rec + 4, static_cast<uint32_t>(kFlagPresent));
        rc = pwrite_all(sidecar_fd_, rec, sizeof rec,
                        kHeaderSize + job.extent_id * 8);
    }
    if (rc != 0) {
        on_write_error(rc);
        return;
    }
    const uint64_t packed =
        (static_cast<uint64_t>(job.crc) << 32) | kFlagPresent;
    const uint64_t old =
        records_[job.extent_id].exchange(packed, std::memory_order_acq_rel);
    if (!(old & kFlagPresent)) {
        const uint64_t p =
            present_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (p == extent_count_) complete_layer();
    }
}

void LayerStore::on_write_error(int err) {
    if (err == ENOSPC || err == EIO) {
        enter_bypass(err, "write failed");
    } else {
        ELIO_LOG_WARNING("layer store {}: entry dropped after write "
                         "failure: {}",
                         dir_, strerror(err));
    }
}

void LayerStore::enter_bypass(int err, const char* what) {
    int expected = static_cast<int>(State::Filling);
    if (state_.compare_exchange_strong(expected,
                                       static_cast<int>(State::Bypass),
                                       std::memory_order_acq_rel)) {
        // Logged once: only the first transition wins the CAS.
        ELIO_LOG_ERROR("layer store {} entering bypass ({}: {}); reads "
                       "continue remotely",
                       dir_, what,
                       err != 0 ? strerror(err) : "attempts exhausted");
    }
}

void LayerStore::complete_layer() {
    // Runs on the writer thread — the mandated blocking context for the
    // whole-file read-back and sha256.
    ++attempts_;
    bool verified = true;
    if (!expected_hex_.empty()) {
        try {
            common::Sha256 hash;
            std::vector<uint8_t> buf(1 << 20);
            uint64_t off = 0;
            while (off < size_) {
                const size_t chunk = static_cast<size_t>(
                    std::min<uint64_t>(buf.size(), size_ - off));
                const ssize_t r =
                    ::pread(staging_fd_.load(std::memory_order_relaxed),
                            buf.data(), chunk, static_cast<off_t>(off));
                if (r <= 0) {
                    verified = false;
                    break;
                }
                hash.update(buf.data(), static_cast<size_t>(r));
                off += static_cast<uint64_t>(r);
            }
            if (verified && hash.final_hex() != expected_hex_) {
                verified = false;
            }
        } catch (const std::exception& e) {
            ELIO_LOG_ERROR("layer store {}: sha256 unavailable: {}",
                           dir_, e.what());
            enter_bypass(0, "verification unavailable");
            return;
        }
    } else {
        ELIO_LOG_WARNING("layer store {}: no sha256 digest; completing "
                         "without verification",
                         dir_);
    }

    if (verified) {
        if (::rename(staging_path().c_str(), commit_path(dir_).c_str()) !=
            0) {
            enter_bypass(errno, "commit rename failed");
            return;
        }
        ::unlink(sidecar_path().c_str());
        if (sidecar_fd_ >= 0) {
            ::close(sidecar_fd_);
            sidecar_fd_ = -1;
        }
        // The staging fd becomes the commit fd: same inode after the
        // rename, so reads in flight on it stay valid.
        commit_fd_.store(staging_fd_.load(std::memory_order_relaxed),
                         std::memory_order_release);
        state_.store(static_cast<int>(State::Complete),
                     std::memory_order_release);
        ELIO_LOG_INFO("layer store {} complete: {} ({} extents)", dir_,
                      commit_path(dir_), extent_count_);
        return;
    }

    ELIO_LOG_WARNING("layer store {}: sha256 mismatch (attempt {}/{})",
                     dir_, attempts_, cfg_.try_count);
    if (attempts_ >= cfg_.try_count) {
        enter_bypass(0, "sha256 mismatch persists");
        return;
    }
    restart_fresh();
}

void LayerStore::restart_fresh() {
    // Ordering contract: demote everything FIRST. Readers either see a
    // cleared record (treated as a miss, re-fetched remotely — any write
    // they enqueue lands in the new pair, because process_job resolves the
    // fds after the publish below) or the old fd with a stale record (old
    // bytes, CRC-verified). Only then build and publish the replacement
    // pair, so a watcher that observes the new pair's files already sees
    // cleared records.
    for (auto& rec : records_) rec.store(0, std::memory_order_relaxed);
    present_.store(0, std::memory_order_relaxed);
    const std::string old_staging = staging_path();
    const std::string old_sidecar = sidecar_path();
    // Build the replacement pair into locals: readers keep using the old
    // staging fd (retired but still open, data intact) until the new one is
    // published, so there is never an instant where readers observe
    // staging_fd_ == -1.
    FreshPair p;
    const int rc = create_fresh_pair(p);
    if (rc != 0) {
        enter_bypass(rc, "fresh staging pair creation failed");
        return;
    }
    // Publish: exchange the fd, retire (don't close) the old one — reader
    // coroutines may still have io_uring reads in flight on it; closed in
    // the destructor.
    const int old_fd =
        staging_fd_.exchange(p.staging_fd, std::memory_order_acq_rel);
    retired_fds_.push_back(old_fd);
    if (sidecar_fd_ >= 0) {
        ::close(sidecar_fd_);
    }
    sidecar_fd_ = p.sidecar_fd;
    nonce_ = p.nonce;
    ::unlink(old_staging.c_str());
    ::unlink(old_sidecar.c_str());
    ELIO_LOG_INFO("layer store {}: restarting fresh (nonce {})", dir_,
                  hex_nonce(nonce_));
}

}  // namespace obd::source
