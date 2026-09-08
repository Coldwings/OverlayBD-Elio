// UblkDevice lifecycle. See device.hpp.
#include "ublk/device.hpp"

#include "common/errors.hpp"
#include "ublk/elio_bridge.hpp"

#include <elio/log/macros.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/time/timer.hpp>

#include <cerrno>

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

    try {
        dev->ctrl_ = std::make_unique<Ctrl>();
        dev->dev_id_ = dev->ctrl_->add_dev(params);
        dev->ctrl_->set_params(dev->dev_id_, params);

        for (uint16_t q = 0; q < params.nr_queues; ++q) {
            auto queue = std::make_unique<Queue>(dev->dev_id_, q,
                                                 params.queue_depth,
                                                 params.max_io_buf_bytes);
            queue->open();
            dev->queues_.push_back(std::move(queue));
        }
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
            elio::go(run_bridge, queue.get(), dev->src_.get(), &dev->stop_);
        }
        // START_DEV: the driver answers EBUSY until every queue tag has a
        // parked FETCH; poll briefly. (co_await is not allowed inside a
        // catch block, hence the flag-based loop.)
        bool started = false;
        for (int attempt = 0; attempt < 100 && !started; ++attempt) {
            try {
                dev->ctrl_->start_dev(dev->dev_id_);
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
        ELIO_LOG_INFO("ublk device {} ready ({})", dev->dev_id_,
                      dev->bdev_path());
    } catch (...) {
        dev->stop();
        throw;
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

    try {
        dev->ctrl_ = std::make_unique<Ctrl>();
        // Announce the replacement server BEFORE parking FETCH commands:
        // the device sits in QUIESCED while there is no server.
        dev->ctrl_->start_user_recovery(dev_id);

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
            elio::go(run_bridge, queue.get(), dev->src_.get(), &dev->stop_);
        }
        // END_USER_RECOVERY completes the handshake once every tag has a
        // parked FETCH (same EBUSY polling as START_DEV).
        bool recovered = false;
        for (int attempt = 0; attempt < 100 && !recovered; ++attempt) {
            try {
                dev->ctrl_->end_user_recovery(dev_id);
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
        ELIO_LOG_INFO("ublk device {} recovered ({})", dev_id,
                      dev->bdev_path());
    } catch (...) {
        dev->stop();
        throw;
    }
    co_return dev;
}

void Device::stop() noexcept {
    stop_.store(true, std::memory_order_relaxed);
    for (auto& q : queues_) q->wakeup();
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
