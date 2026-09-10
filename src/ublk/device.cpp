// UblkDevice lifecycle. See device.hpp.
#include "ublk/device.hpp"

#include "common/errors.hpp"
#include "ublk/elio_bridge.hpp"

#include <elio/log/macros.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/runtime/spawn_blocking.hpp>
#include <elio/time/timer.hpp>

#include <cerrno>
#include <chrono>
#include <exception>
#include <thread>

namespace obd::ublk {

elio::coro::task<std::unique_ptr<Device>> Device::create(
    const DeviceParams& params, source::BlobSourcePtr src) {
    if (!src) throw error(EINVAL, "ublk device with null block source");
    if (params.dev_sectors == 0) {
        throw error(EINVAL, "ublk device with zero size");
    }
    auto dev = std::unique_ptr<Device>(new Device());
    dev->params_ = params;
    dev->src_ = std::move(src);

    std::exception_ptr failure;
    try {
        dev->ctrl_ = std::make_unique<Ctrl>();
        // Kernel control calls run off-scheduler (spawn_blocking): they
        // may block in the kernel — START_DEV's add_disk partition scan
        // issues reads serviced by OUR bridges, so a synchronous control
        // call on a worker deadlocks the data plane (worker stalls, the
        // bridge coroutine never runs). See ublk.md control plane.
        dev->dev_id_ = co_await elio::spawn_blocking([&]() -> uint32_t {
            const uint32_t id = dev->ctrl_->add_dev(params);
            dev->ctrl_->set_params(id, params);
            return id;
        });

        for (uint16_t q = 0; q < params.nr_queues; ++q) {
            auto queue = std::make_unique<Queue>(dev->dev_id_, q,
                                                 params.queue_depth,
                                                 params.max_io_buf_bytes);
            queue->open();
            dev->queues_.push_back(std::move(queue));
        }
        ELIO_LOG_INFO("ublk dev {}: add_dev+set_params done, opening queues",
                      dev->dev_id_);
        // Queue threads first: they park the per-tag FETCH commands the
        // driver requires before START_DEV.
        for (auto& queue : dev->queues_) {
            Queue* q = queue.get();
            dev->threads_.emplace_back([q, &stop = dev->stop_] {
                try {
                    q->run(stop);
                } catch (const std::exception& e) {
                    // Never let a queue-thread exception terminate the
                    // process; surface it as a queue failure instead.
                    ELIO_LOG_ERROR("ublk queue {} thread died: {}",
                                   q->q_id(), e.what());
                    q->fail(EIO);
                }
            });
        }
        // Bridge coroutines on the Elio scheduler, one per queue.
        for (auto& queue : dev->queues_) {
            dev->io_tasks_running_.fetch_add(1, std::memory_order_acq_rel);
            try {
                elio::go(run_bridge, queue.get(), dev->src_.get(), &dev->stop_,
                         &dev->io_tasks_running_);
            } catch (...) {
                dev->io_tasks_running_.fetch_sub(1, std::memory_order_acq_rel);
                throw;
            }
        }
        ELIO_LOG_INFO("ublk dev {}: bridges started, START_DEV poll",
                      dev->dev_id_);
        // START_DEV: the driver answers EBUSY until every queue tag has a
        // parked FETCH; poll briefly. (co_await is not allowed inside a
        // catch block, hence the flag-based loop.)
        bool started = false;
        for (int attempt = 0; attempt < 100 && !started; ++attempt) {
            try {
                co_await elio::spawn_blocking(
                    [&] { dev->ctrl_->start_dev(dev->dev_id_); });
                started = true;
            } catch (const std::system_error& e) {
                if (e.code().value() != EBUSY) throw;
            }
            if (!started) {
                for (const auto& q : dev->queues_) {
                    if (q->failed()) {
                        throw_errno(q->failure() ? q->failure() : EIO,
                                    "ublk queue failed before START_DEV");
                    }
                }
                co_await elio::time::sleep_for(
                    std::chrono::milliseconds(50));
            }
        }
        if (!started) {
            throw error(EBUSY, "ublk START_DEV not ready after 5s");
        }
        dev->started_ = true;
        dev->cur_bytes_.store(params.dev_sectors * 512,
                              std::memory_order_release);
        ELIO_LOG_INFO("ublk device {} ready ({})", dev->dev_id_,
                      dev->bdev_path());
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure) {
        co_await cleanup_failed(std::move(dev), failure);
    }
    co_return dev;
}

elio::coro::task<std::unique_ptr<Device>> Device::attach(
    uint32_t dev_id, const DeviceParams& params,
    source::BlobSourcePtr src) {
    if (!src) throw error(EINVAL, "ublk recovery with null block source");
    auto dev = std::unique_ptr<Device>(new Device());
    dev->params_ = params;
    dev->src_ = std::move(src);
    dev->dev_id_ = dev_id;

    std::exception_ptr failure;
    try {
        dev->ctrl_ = std::make_unique<Ctrl>();
        // Announce the replacement server BEFORE parking FETCH commands:
        // the device sits in QUIESCED while there is no server.
        co_await elio::spawn_blocking(
            [&] { dev->ctrl_->start_user_recovery(dev_id); });

        for (uint16_t q = 0; q < params.nr_queues; ++q) {
            auto queue = std::make_unique<Queue>(dev_id, q,
                                                 params.queue_depth,
                                                 params.max_io_buf_bytes);
            queue->open();
            dev->queues_.push_back(std::move(queue));
        }
        for (auto& queue : dev->queues_) {
            Queue* q = queue.get();
            dev->threads_.emplace_back([q, &stop = dev->stop_] {
                try {
                    q->run(stop);
                } catch (const std::exception& e) {
                    // Never let a queue-thread exception terminate the
                    // process; surface it as a queue failure instead.
                    ELIO_LOG_ERROR("ublk queue {} thread died: {}",
                                   q->q_id(), e.what());
                    q->fail(EIO);
                }
            });
        }
        for (auto& queue : dev->queues_) {
            dev->io_tasks_running_.fetch_add(1, std::memory_order_acq_rel);
            try {
                elio::go(run_bridge, queue.get(), dev->src_.get(), &dev->stop_,
                         &dev->io_tasks_running_);
            } catch (...) {
                dev->io_tasks_running_.fetch_sub(1, std::memory_order_acq_rel);
                throw;
            }
        }
        // END_USER_RECOVERY completes the handshake once every tag has a
        // parked FETCH (same EBUSY polling as START_DEV).
        bool recovered = false;
        for (int attempt = 0; attempt < 100 && !recovered; ++attempt) {
            try {
                co_await elio::spawn_blocking(
                    [&] { dev->ctrl_->end_user_recovery(dev_id); });
                recovered = true;
            } catch (const std::system_error& e) {
                if (e.code().value() != EBUSY) throw;
            }
            if (!recovered) {
                co_await elio::time::sleep_for(
                    std::chrono::milliseconds(50));
            }
        }
        if (!recovered) {
            throw error(EBUSY, "ublk END_USER_RECOVERY not ready after 5s");
        }
        dev->ctrl_->adopt_dev(dev_id);
        dev->started_ = true;
        // D3/FIX-2 grow-only baseline: a recovered device's kernel gendisk
        // KEEPS the capacity it had when the previous server died
        // (USER_RECOVERY never resets it, and attach issues no
        // SET_PARAMS/UPDATE_SIZE), which may be LARGER than the
        // create-time params. Seeding the baseline from the params would
        // let a post-recovery "resize" pass the grow-only check and then
        // actually SHRINK the gendisk via UPDATE_SIZE. Read the kernel's
        // REAL current capacity (GET_PARAMS) instead; grow-only then
        // rejects anything at or below it.
        dev->cur_bytes_.store(
            co_await elio::spawn_blocking([&]() -> uint64_t {
                const ublk_params p = dev->ctrl_->get_params(dev_id);
                return p.basic.dev_sectors * 512;
            }),
            std::memory_order_release);
        ELIO_LOG_INFO("ublk device {} recovered ({}, {} bytes)",
                      dev_id, dev->bdev_path(), dev->size_bytes());
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure) {
        co_await cleanup_failed(std::move(dev), failure);
    }
    co_return dev;
}

elio::coro::task<void> Device::cleanup_failed(
    std::unique_ptr<Device> dev, std::exception_ptr failure) {
    co_await dev->stop_async();
    co_await elio::spawn_blocking([&] { dev.reset(); });
    std::rethrow_exception(failure);
}

uint64_t Device::resize_blocking(uint64_t bytes) {
    // D3 grow-only online resize. BLOCKING (kernel control call): every
    // caller must route this off an Elio worker via spawn_blocking —
    // the device command loop does so (see run_device_control).
    if (bytes == 0 || bytes % 512 != 0) {
        throw error(EINVAL, "resize size must be a positive multiple of "
                            "512 bytes");
    }
    const uint64_t cur = cur_bytes_.load(std::memory_order_acquire);
    // Grow-only (ADR-0014 dev_size model): shrinking or no-op'ing a live
    // device is rejected cleanly here — the executor's reply surfaces
    // this message to the CLI.
    if (bytes <= cur) {
        throw error(EINVAL, "resize rejected: grow-only (requested " +
                                std::to_string(bytes) +
                                " <= current " + std::to_string(cur) +
                                " bytes)");
    }
    ctrl_->update_size(dev_id_, bytes / 512);
    cur_bytes_.store(bytes, std::memory_order_release);
    ELIO_LOG_INFO("ublk device {} grew to {} bytes", dev_id_, bytes);
    return bytes;
}

void Device::request_stop() noexcept {
    stop_.store(true, std::memory_order_relaxed);
    for (auto& q : queues_) q->wakeup();
    // Also wake the bridge coroutines: each is parked in an eventfd
    // async_read, and nothing will write that eventfd again once the
    // queue threads exit — a parked detached bridge outlives stop()
    // and hangs scheduler teardown.
    for (auto& q : queues_) q->notify_elio();
}

elio::coro::task<void> Device::stop_async() {
    request_stop();
    // A source may itself await spawn_blocking. Do not occupy the pool
    // while draining: even one worker and one blocking thread must progress.
    while (io_tasks_running_.load(std::memory_order_acquire) > 0) {
        co_await elio::time::sleep_for(std::chrono::milliseconds(1));
    }
    co_await elio::spawn_blocking([this] { stop(); });
}

void Device::stop() noexcept {
    request_stop();
    // Blocking callers must leave the scheduler/source executors running.
    // Elapsed time cannot end the lifetime of an outstanding source IO.
    while (io_tasks_running_.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
}

Device::~Device() {
    stop();
    // STOP_DEV/DEL_DEV happen in ~Ctrl (added_dev_).
}

}  // namespace obd::ublk
