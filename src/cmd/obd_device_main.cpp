// obd-device: one isolated process serving one OverlayBD image as one ublk
// block device (ADR-0004). Spawned by obd-supervisor; reports lifecycle
// status as JSON-lines on the inherited control fd (protocol.hpp).
#include "image/config.hpp"
#include "image/image_file.hpp"
#include "format/merged_writable.hpp"
#include "common/errors.hpp"
#include "supervisor/device_control.hpp"
#include "supervisor/protocol.hpp"
#include "cmd/startup_socket.hpp"

#if defined(OBD_HAVE_UBLK) && OBD_HAVE_UBLK
#include "ublk/device.hpp"
#else
#error "obd-device requires OBD_ENABLE_UBLK=ON (ublk is the block-device backend)"
#endif

#include <elio/log/macros.hpp>
#include <elio/runtime/async_main.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/runtime/spawn_blocking.hpp>
#include <elio/time/timer.hpp>
#include <elio/signal/signalfd.hpp>
#include <elio/sync/mutex.hpp>

#include <sys/socket.h>
#include <sys/random.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unistd.h>

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s (--config PATH | --blank-size BYTES --blank-dir "
                 "PATH)\n"
                 "          [--global PATH] [--control-fd N] [--dev-id N] "
                 "[--recover] [--virtual-size BYTES]\n"
                 "          [--startup-socket @NAME]\n",
                 argv0);
}

struct Args {
    std::string config;
    std::string global;
    int control_fd = -1;
    int dev_id = -1;
    bool recover = false;  // ADR-0010: attach to an existing device
    uint64_t startup_generation = 0;
    std::string startup_socket;
    /// D3 create-time headroom: dev_size override in bytes (0 = derive
    /// from the image's declared virtual size). Only meaningful for an
    /// image device; a blank device sizes itself to blank_size.
    uint64_t virtual_size = 0;
    // ADR-0014 modes 2/3 (blank raw device): no image config; the device
    // assembles the empty LSMT zero base + a writable LSMT-RW upper of
    // blank_size bytes inside blank_dir.
    bool blank = false;
    uint64_t blank_size = 0;
    std::string blank_dir;
};

void write_status(const obd::supervisor::ControlChannelWriterPtr& channel,
            const obd::supervisor::DeviceStatus& st) noexcept {
    if (!channel) return;
    // Serialized with the trace control loop's writes (SOCK_STREAM has
    // no PIPE_BUF rule — see ControlChannelWriter).
    try {
        channel->write_line(obd::supervisor::make_device_status(st));
    } catch (...) {
        // Reporting is best-effort; it never transfers the resource owner.
    }
}

using obd::cmd::startup_status;

void report_failure(
    const obd::supervisor::ControlChannelWriterPtr& channel,
    const char* message, const obd::ublk::StartupSnapshot& snapshot) noexcept {
    // Include construction of the status strings in the exception boundary.
    // An allocation failure in a catch handler must not skip awaited cleanup.
    try {
        write_status(channel, obd::supervisor::DeviceStatus{
            "failed", "", message, startup_status(snapshot)});
    } catch (...) {
    }
}

struct ReportedDeviceFailure {};


elio::coro::task<int> device_main(Args args) {
    using obd::supervisor::DeviceStatus;
    // ONE serialized writer for every channel writer (status reports,
    // trace replies, the expiry event) — see ControlChannelWriter.
    const obd::supervisor::ControlChannelWriterPtr channel =
        args.control_fd >= 0
            ? std::make_shared<obd::supervisor::ControlChannelWriter>(
                  args.control_fd)
            : nullptr;
    obd::ublk::StartupControl startup(args.startup_generation == 0
                                        ? 1 : args.startup_generation);
    auto report = [&](const obd::supervisor::ControlChannelWriterPtr& writer,
                      DeviceStatus status) noexcept {
        try {
            if (!status.startup_owner)
                status.startup_owner = startup_status(startup.snapshot());
            write_status(writer, status);
        } catch (...) {}
    };
    std::atomic<bool> granted{!channel};
    std::atomic<bool> serving{false};
    std::atomic<bool> observer_done{false};
    std::atomic<bool> shutdown_requested{false};
    elio::signal::signal_set term_set;
    term_set.add(SIGTERM).add(SIGINT);
    elio::signal::signal_fd term_fd(term_set);
    // Owners survive the protected body, so exceptions use the same awaited
    // cleanup as a normal shutdown instead of destructing on this worker.
    std::shared_ptr<obd::ublk::Device> dev;
    std::unique_ptr<obd::ublk::Device> unshared_dev;
    std::optional<obd::ublk::StartupSnapshot> retired_device;
    obd::image::OpenedImage opened;
    auto stopping = std::make_shared<std::atomic<bool>>(false);
    auto resize_gate = std::make_shared<std::mutex>();
    auto trace_start_stopping = std::make_shared<std::atomic<bool>>(false);
    auto trace_start_gate = std::make_shared<elio::sync::mutex>();
    std::optional<elio::coro::join_handle<void>> control;
    std::optional<elio::coro::join_handle<void>> observer;
    std::optional<obd::cmd::StartupSocket> standalone_socket;
    std::optional<elio::coro::join_handle<void>> standalone_control;
    std::shared_ptr<obd::supervisor::DeviceControlSession> session;
    bool explicitly_released = false;
    int result = 0;
    auto close_trace_start_admission = [&]() -> elio::coro::task<void> {
        trace_start_stopping->store(true, std::memory_order_release);
        co_await trace_start_gate->lock();
        trace_start_gate->unlock();
    };
    auto finish_trace_recording = [&]() -> elio::coro::task<void> {
        if (!opened.recorder) co_return;
        const auto tres = co_await opened.recorder->stop("shutdown");
        if (!tres.ok && tres.error != "no trace recording in progress") {
            ELIO_LOG_ERROR("trace finalize on shutdown failed: {}",
                           tres.error);
        }
    };
    bool fills_parked = false;
    auto park_opened_fills = [&]() -> elio::coro::task<void> {
        if (fills_parked) co_return;
        co_await obd::image::park_image_fills(opened);
        fills_parked = true;
    };
    try {
        if (!channel) {
            standalone_socket.emplace(args.startup_socket, startup);
            standalone_control.emplace(elio::spawn([&]() -> elio::coro::task<void> {
                co_await standalone_socket->run();
            }));
            // Binding alone is insufficient: the acceptor must have installed
            // its client-frame owner before any kernel publication is allowed.
            while (!standalone_socket->started()) {
                if (standalone_control->is_ready())
                    throw std::runtime_error("startup socket service failed before admission");
                co_await elio::time::sleep_for(std::chrono::milliseconds(1));
            }
            std::fprintf(stderr, "startup control: %s generation=%llu\n",
                         args.startup_socket.c_str(),
                         static_cast<unsigned long long>(startup.snapshot().generation));
        }
        if (channel) {
            session = std::make_shared<obd::supervisor::DeviceControlSession>(
                [&](const nlohmann::json& command)
                    -> std::optional<nlohmann::json> {
                    const auto name = command.at("cmd").get<std::string>();
                    if (name != "startup_begin" && name != "startup_release" &&
                        name != "startup_status") return std::nullopt;
                    const auto generation = command.find("generation");
                    if (generation == command.end() ||
                        !generation->is_number_unsigned() ||
                        generation->get<uint64_t>() != args.startup_generation) {
                        return nlohmann::json{{"ok", false},
                            {"error", "startup owner generation mismatch"}};
                    }
                    nlohmann::json reply = {
                        {"ok", true}, {"generation", args.startup_generation}};
                    if (name == "startup_begin") {
                        const auto version = command.find("version");
                        if (version == command.end() ||
                            !version->is_number_integer() ||
                            *version != obd::supervisor::kStartupOwnerVersion) {
                            reply["ok"] = false;
                            reply["error"] = "unsupported startup owner version";
                        } else {
                            // Publication is allowed only after the parent has
                            // installed its durable, protected Child owner.
                            granted.store(true);
                        }
                    } else if (name == "startup_release") {
                        const auto seq = command.find("seq");
                        const auto result = seq != command.end() &&
                                seq->is_number_unsigned()
                            ? startup.request_release(args.startup_generation,
                                                      seq->get<uint64_t>())
                            : obd::ublk::ReleaseRequestResult::stale;
                        reply["ok"] = result == obd::ublk::ReleaseRequestResult::accepted;
                        switch (result) {
                        case obd::ublk::ReleaseRequestResult::accepted:
                            reply["release"] = "pending"; break;
                        case obd::ublk::ReleaseRequestResult::stale:
                            reply["release"] = "stale"; break;
                        case obd::ublk::ReleaseRequestResult::busy:
                            reply["release"] = "busy"; break;
                        case obd::ublk::ReleaseRequestResult::not_startup:
                            reply["release"] = "not_startup"; break;
                        }
                    }
                    reply["startup_owner"] = obd::supervisor::startup_owner_json(
                        startup_status(startup.snapshot()));
                    return reply;
                });
            control.emplace(elio::spawn(
                obd::supervisor::run_device_control_with_session, channel,
                std::shared_ptr<obd::image::TraceRecorder>{},
                obd::supervisor::DeviceControlHooks{}, session));
        }
        // Cold observer only: no per-IO work or extra handoff. Reporting and
        // signal handling remain available while attach retains its frame.
        observer.emplace(elio::spawn([&]() -> elio::coro::task<void> {
            while (!observer_done.load()) {
                try {
                    while (auto signal = term_fd.try_read()) {
                        if (serving.load()) shutdown_requested.store(true);
                        else ELIO_LOG_WARNING(
                            "startup owner retained: automatic shutdown refused");
                    }
                    if (!stopping->load()) {
                        auto owner = startup_status(startup.snapshot());
                        const bool failed = owner.phase == "retained" ||
                                            owner.phase == "releasing";
                        DeviceStatus status;
                        status.state = serving.load() ? "ready" :
                            failed ? "failed" : "starting";
                        if (serving.load() && owner.device_id >= 0)
                            status.device = "/dev/ublkb" + std::to_string(owner.device_id);
                        if (failed) status.error =
                            "startup failed; owner retained pending explicit release";
                        status.startup_owner = std::move(owner);
                        report(channel, status);
                    }
                } catch (...) {
                    // Observation failure cannot discard the owning startup
                    // coroutine or authorize its parent's force cleanup.
                }
                try {
                    co_await elio::time::sleep_for(std::chrono::milliseconds(100));
                } catch (...) {
                }
            }
        }));
        while (!granted.load()) {
            if (startup.release_before_start()) {
                throw obd::ublk::StartupReleased{};
            }
            if (control && control->is_ready()) {
                throw std::runtime_error("parent channel closed before startup grant");
            }
            co_await elio::time::sleep_for(std::chrono::milliseconds(10));
        }
        obd::image::GlobalConfig global =
            args.global.empty()
                ? obd::image::GlobalConfig{}
                : obd::image::GlobalConfig::from_file(args.global);
        if (args.blank) {
            if (args.blank_size == 0 || args.blank_size % 512 != 0) {
                ELIO_LOG_ERROR("blank size {} is not sector aligned",
                               args.blank_size);
                report(channel, DeviceStatus{"failed", "",
                                          "blank size not sector aligned"});
                throw ReportedDeviceFailure{};
            }
            obd::image::BlankDeviceSpec spec;
            spec.size = args.blank_size;
            spec.dir = args.blank_dir;
            opened = co_await obd::image::open_blank_device(spec);
        } else {
            const obd::image::ImageConfig img =
                obd::image::ImageConfig::from_file(args.config,
                                                   global.download);
            // D3 create-time headroom (--virtual-size, bytes): for a
            // WRITABLE image the override is threaded into open_image,
            // which sizes the writable top — and hence the merged DATA
            // PLANE — to the override (grow-only vs the image's declared
            // size, rejected inside open_image with the single-source
            // rule). A read-only image ignores the override in assembly:
            // its headroom is dev-size-only, validated below with
            // device_capacity_bytes (reads past the image's end are
            // zero-filled by the bridge).
            opened = co_await obd::image::open_image(img, global,
                                                     args.virtual_size);
        }

        if (opened.virtual_size == 0 || opened.virtual_size % 512 != 0) {
            ELIO_LOG_ERROR("image virtual size {} is not sector aligned",
                           opened.virtual_size);
            report(channel, DeviceStatus{"failed", "",
                                      "virtual size not sector aligned"});
            throw ReportedDeviceFailure{};
        }
        uint64_t dev_bytes = opened.virtual_size;
        if (args.virtual_size > 0) {
            if (args.virtual_size % 512 != 0) {
                ELIO_LOG_ERROR(
                    "virtual-size override {} is not sector aligned",
                    args.virtual_size);
                report(channel, DeviceStatus{"failed", "",
                                          "virtual_size not sector aligned"});
                throw ReportedDeviceFailure{};
            }
            if (!opened.writable) {
                std::string cap_error;
                dev_bytes = obd::image::device_capacity_bytes(
                    opened.virtual_size, args.virtual_size, &cap_error);
                if (dev_bytes == 0) {
                    ELIO_LOG_ERROR("virtual-size override rejected: {}",
                                   cap_error);
                    report(channel, DeviceStatus{"failed", "", cap_error});
                    throw ReportedDeviceFailure{};
                }
            }
            // Writable: opened.virtual_size already reflects the override
            // (open_image grew the data plane), so dev_bytes stays as-is.
        }

        obd::ublk::DeviceParams params;
        params.dev_sectors = dev_bytes / 512;
        params.read_only = !opened.writable;  // ADR-0008
        params.enable_recovery = global.ublk_recovery;  // ADR-0010
        if (args.dev_id >= 0) {
            params.dev_id = static_cast<uint32_t>(args.dev_id);
        }
        // ADR-0014: keep a handle on the writable top (checkpointed on
        // graceful shutdown, enabling the supervisor's offline commit)
        // and on the merged root itself — the D3 resize executor grows
        // the merged data plane through it. Both pointers are owned by
        // `dev` below (it takes the source chain); the executor holds a
        // shared_ptr to `dev`, so they cannot dangle (FIX: refcounted
        // lifetime, no TOCTOU seam).
        obd::format::WritableLayer* writable_top = nullptr;
        obd::format::MergedWritable* merged_root = nullptr;
        if (opened.writable) {
            auto* mw = dynamic_cast<obd::format::MergedWritable*>(
                opened.root.get());
            if (mw != nullptr) {
                merged_root = mw;
                writable_top = &mw->writable_top();
            }
        }
        // Refcounted: the control coroutine below captures a copy, so a
        // resize in flight can never outlive the Device (destroyed only
        // when the LAST reference drops, after the control loop exits).
        if (args.recover) {
            // ADR-0010: replace a crashed server for an existing device.
            if (args.dev_id < 0) {
                report(channel, DeviceStatus{"failed", "",
                                          "--recover requires --dev-id"});
                throw ReportedDeviceFailure{};
            }
            unshared_dev = co_await obd::ublk::Device::attach(
                static_cast<uint32_t>(args.dev_id), params,
                std::move(opened.root), startup, park_opened_fills);
        } else {
            unshared_dev = co_await obd::ublk::Device::create(params,
                                                     std::move(opened.root),
                                                     startup, park_opened_fills);
        }
        // Conversion can allocate a shared control block. Its failure leaves
        // this outer unique owner intact for awaited cleanup off the worker.
        dev = std::shared_ptr<obd::ublk::Device>(std::move(unshared_dev));

        // Serve the supervisor's device commands (ADR-0013 trace record
        // path, D3 resize) on the control channel (EOF = supervisor
        // gone; the loop exits and the device keeps serving). The loop
        // may outlive this coroutine's frame, so everything it touches
        // is refcounted: channel, recorder, and `dev` (a shared_ptr copy
        // keeps the Device — and the source chain it owns, including
        // merged_root/writable_top — alive until the loop exits; a
        // resize in flight inside spawn_blocking therefore can never
        // dereference a destroyed Device). recorder may be null
        // (recorder-less images) — trace commands are answered
        // "unavailable" while resize still works.
        // D3 shutdown guard, shared with the resize executor seam: set
        // BEFORE dev->stop() below. Once the graceful shutdown has
        // begun, the shutdown checkpoint writes (or has written) the
        // upper's trailer at the CURRENT size — a resize landing in the
        // window up to the channel EOF would rewrite the on-disk
        // declared-size header to a different size and leave the
        // header/trailer pair inconsistent, i.e. the upper
        // UNCOMMITTABLE. The guard makes every ordering safe: a grow
        // that completes before this point is followed by a checkpoint
        // at the grown size (consistent), and everything after it is
        // rejected cleanly.
        // Serializes an in-flight grow against the shutdown checkpoint:
        // the shutdown path sets `stopping` and then drains this gate, so
        // a grow can never rewrite the layer's declared-size header after
        // the checkpoint wrote its trailer at the old size.
        if (channel) {
            obd::supervisor::DeviceControlHooks hooks;
            // D3 resize executor seam (built by make_resize_apply so the
            // ordering + shutdown contract are unit-tested without a
            // device): grow-only is enforced by the command loop against
            // current_size BEFORE any IO; apply (a) rejects when the
            // device is shutting down, (b) grows the writable DATA PLANE
            // — so writes into the headroom land in the upper and commit
            // can seal them — then (c) issues the blocking ublk
            // UPDATE_SIZE. The loop runs apply via spawn_blocking, per
            // the ublk control-plane rule; the layer grows are BLOCKING
            // by design and run on that same pool thread.
            hooks.resize.current_size =
                [dev]() -> uint64_t { return dev->size_bytes(); };
            hooks.resize.apply_resize = obd::supervisor::make_resize_apply(
                stopping, resize_gate,
                merged_root != nullptr
                    ? std::function<int(uint64_t)>(
                          [merged_root](uint64_t bytes) {
                              return merged_root->grow(bytes);
                          })
                    : std::function<int(uint64_t)>(),
                [dev](uint64_t bytes) { return dev->resize_blocking(bytes); });
            hooks.trace_start_stopping = trace_start_stopping;
            hooks.trace_start_gate = trace_start_gate;
            session->publish_ready(opened.recorder, std::move(hooks));
        }

        serving.store(true);
        report(channel, DeviceStatus{"ready", dev->bdev_path(), "",
                                     startup_status(startup.snapshot())});
        while (!shutdown_requested.load()) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(50));
        }
        ELIO_LOG_INFO("device {} shutting down", dev->bdev_path());
        // Refuse further resizes from here on: the checkpoint below
        // writes its trailer at the current size, so a grow after this
        // point would desynchronize the on-disk header (see the guard's
        // comment where it is created). Set BEFORE stop().
        stopping->store(true, std::memory_order_release);
        // Drain the resize gate: taking and releasing it waits for a grow
        // already IN FLIGHT (which holds it for its whole duration) and,
        // because every apply re-checks `stopping` while holding it,
        // guarantees that no grow is running — or can start — from here
        // through the checkpoint below. Off-worker: the wait is bounded
        // by one grow's 4K header write + fsync.
        co_await elio::spawn_blocking([&] {
            std::lock_guard<std::mutex> drain(*resize_gate);
        });
        co_await close_trace_start_admission();
        co_await dev->stop_async();
        // ADR-0013: finalize and drain any trace recording BEFORE the source
        // chain can go away. Do not gate this on recording(): an expiry-owned
        // finalize has already lowered the hot-path flag while its timer
        // coroutine may still be writing the file or sending the callback.
        co_await finish_trace_recording();
        // ADR-0014: with the queues drained, persist the writable top's
        // index so the supervisor can seal the upper offline (commit). A
        // checkpoint failure is logged, not fatal: shutdown continues and
        // a later commit will report the missing checkpoint.
        if (writable_top != nullptr) {
            const int crc = co_await writable_top->checkpoint();
            if (crc != 0) {
                ELIO_LOG_ERROR("writable upper checkpoint failed: {}",
                               std::strerror(-crc));
            }
        }
        // Park background fills before the source chain is destroyed
        // (the LayerStore lifetime contract; no-op when fill is off).
        co_await park_opened_fills();
        // Keep our owning reference until the control task's captures have
        // been destroyed; its last reference must not destroy Device on a worker.
        report(channel, DeviceStatus{"stopped", "", ""});
        // End the reader while retaining the write side for the final
        // ownership report after all control captures have been destroyed.
        if (args.control_fd >= 0) {
            ::shutdown(args.control_fd, SHUT_RD);
        }
    } catch (const obd::ublk::StartupReleased&) {
        explicitly_released = true;
    } catch (const ReportedDeviceFailure&) {
        result = 1;
    } catch (const std::system_error& e) {
        try { ELIO_LOG_ERROR("device failed: {}", e.what()); } catch (...) {}
        report_failure(channel, e.what(), startup.snapshot());
        result = 1;
    } catch (const std::exception& e) {
        try { ELIO_LOG_ERROR("device failed: {}", e.what()); } catch (...) {}
        report_failure(channel, e.what(), startup.snapshot());
        result = 1;
    } catch (...) {
        try { ELIO_LOG_ERROR("device failed: unknown exception"); } catch (...) {}
        report_failure(channel, "unknown exception", startup.snapshot());
        result = 1;
    }
    // Every cleanup await can fail before it installs its observer/work item.
    // Retain this frame (including the locals borrowed by control/observer)
    // and retry that stage. Completed stages are never replayed. In particular,
    // an allocation failure cannot invoke Device's blocking destructor here.
    stopping->store(true, std::memory_order_release);
    enum class Cleanup {
        resize, trace_admission, control, session, device, trace, fills,
        destroy_device, observer, done,
    };
    auto cleanup = Cleanup::resize;
    while (cleanup != Cleanup::done) {
        try {
            switch (cleanup) {
            case Cleanup::resize:
                co_await elio::spawn_blocking([&] {
                    std::lock_guard<std::mutex> drain(*resize_gate);
                });
                cleanup = Cleanup::trace_admission;
                break;
            case Cleanup::trace_admission:
                co_await close_trace_start_admission();
                cleanup = Cleanup::control;
                break;
            case Cleanup::control:
                if (control) {
                    ::shutdown(args.control_fd, SHUT_RD);
                    // Native destruction includes body completion and capture
                    // destruction. This async observer leaves the sole blocking
                    // worker available to a resize still finishing its body.
                    co_await control->wait_destroyed_async();
                    try { control->await_resume(); }
                    catch (...) { result = 1; }
                    control.reset();
                }
                cleanup = Cleanup::session;
                break;
            case Cleanup::session:
                // Reader hooks are gone before Device destruction.
                session.reset();
                if (startup.finish_before_start())
                    explicitly_released = startup.snapshot().release ==
                        obd::ublk::StartupRelease::released;
                cleanup = Cleanup::device;
                break;
            case Cleanup::device:
                if (dev) co_await dev->stop_async();
                else if (unshared_dev) co_await unshared_dev->stop_async();
                cleanup = Cleanup::trace;
                break;
            case Cleanup::trace:
                co_await finish_trace_recording();
                cleanup = Cleanup::fills;
                break;
            case Cleanup::fills:
                co_await park_opened_fills();
                cleanup = Cleanup::destroy_device;
                break;
            case Cleanup::destroy_device:
                if (dev) retired_device = dev->released_snapshot();
                else if (unshared_dev)
                    retired_device = unshared_dev->released_snapshot();
                co_await elio::spawn_blocking([&] {
                    dev.reset();
                    unshared_dev.reset();
                });
                cleanup = Cleanup::observer;
                break;
            case Cleanup::observer:
                observer_done.store(true);
                if (observer) {
                    co_await observer->wait_destroyed_async();
                    try { observer->await_resume(); }
                    catch (...) { result = 1; }
                    observer.reset();
                }
                cleanup = Cleanup::done;
                break;
            case Cleanup::done:
                break;
            }
        } catch (...) {
            result = 1;
            try { ELIO_LOG_ERROR("device cleanup deferred; resource owners retained"); }
            catch (...) {}
        }
        // Failure of even the retry delay must not unwind the owning frame.
        if (cleanup != Cleanup::done) {
            try { co_await elio::time::sleep_for(std::chrono::milliseconds(1)); }
            catch (...) {}
        }
    }
    // No startup owner can reach here with outstanding obligations. For
    // pre-publication errors this also releases the parent's conservative
    // protection; absent this report the parent must remain conservative.
    auto facts = startup.snapshot();
    {
        if (retired_device) {
            const auto generation = facts.generation;
            const auto version = facts.version;
            facts = *retired_device;
            facts.generation = generation;
            facts.version = version;
        } else if (!startup.owner_entered()) {
            // No Device invocation acquired resource ownership.
            facts.phase = obd::ublk::StartupPhase::released;
        }
        facts.version += 2;
        if (!explicitly_released) facts.release = obd::ublk::StartupRelease::none;
    }
    try {
        const DeviceStatus terminal{result ? "failed" : "stopped", "", "",
                                    startup_status(facts)};
        report(channel, terminal);
        if (!channel)
            std::fprintf(stderr, "%s", obd::supervisor::make_device_status(terminal).c_str());
    } catch (...) {}
    if (args.control_fd >= 0) ::shutdown(args.control_fd, SHUT_WR);
    if (standalone_socket) standalone_socket->finish(facts);
    while (standalone_control) {
        try {
            co_await standalone_control->wait_destroyed_async();
            try { standalone_control->await_resume(); } catch (...) { result = 1; }
            standalone_control.reset();
        } catch (...) {
            // An observer allocation failure cannot destroy a serving socket's
            // frame or the StartupControl borrowed by its client handlers.
        }
        if (standalone_control) {
            try { co_await elio::time::sleep_for(std::chrono::milliseconds(1)); }
            catch (...) {}
        }
    }
    standalone_socket.reset();
    co_return result;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (++i >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(2);
            }
            return argv[i];
        };
        if (a == "--config") args.config = next("--config");
        else if (a == "--blank-size") {
            args.blank = true;
            // Strict decimal validation: std::stoull would happily turn
            // "-512" into 1.8e19 and pass the alignment check, so nothing
            // negative/overflowing/junk reaches the assembler. Range and
            // alignment are enforced HERE as well as in the supervisor's
            // parse_blank_spec — obd-device is also a standalone entry
            // point, and a malformed size must be a usage error, not a
            // device that comes up mis-sized.
            const std::string v = next("--blank-size");
            bool bad = v.empty() || v[0] == '-' ||
                       v.find_first_not_of("0123456789") != std::string::npos;
            uint64_t parsed = 0;
            if (!bad) {
                try {
                    parsed = std::stoull(v);
                } catch (const std::exception&) {
                    bad = true;
                }
            }
            if (!bad &&
                (parsed == 0 || parsed % 512 != 0 ||
                 parsed > obd::supervisor::kMaxBlankSizeBytes)) {
                bad = true;
            }
            if (bad) {
                std::fprintf(stderr,
                             "invalid --blank-size '%s' (want a positive "
                             "multiple of 512 bytes, at most %llu)\n",
                             v.c_str(),
                             static_cast<unsigned long long>(
                                 obd::supervisor::kMaxBlankSizeBytes));
                return 2;
            }
            args.blank_size = parsed;
        } else if (a == "--blank-dir") args.blank_dir = next("--blank-dir");
        else if (a == "--global") args.global = next("--global");
        else if (a == "--control-fd")
            args.control_fd = std::stoi(next("--control-fd"));
        else if (a == "--dev-id") args.dev_id = std::stoi(next("--dev-id"));
        else if (a == "--virtual-size") {
            // Byte count via strtoull (std::stoull would throw on bad
            // input): negative wraps through unsigned — reject it
            // explicitly; the positive/alignment/grow-only semantic
            // checks live in device_main where sizes are comparable.
            const std::string vs = next("--virtual-size");
            const char* v = vs.c_str();
            if (v[0] == '-') {
                std::fprintf(stderr, "invalid --virtual-size '%s'\n", v);
                return 2;
            }
            char* end = nullptr;
            errno = 0;
            const unsigned long long b = std::strtoull(v, &end, 10);
            if (errno != 0 || end == v || *end != '\0') {
                std::fprintf(stderr, "invalid --virtual-size '%s'\n", v);
                return 2;
            }
            args.virtual_size = b;
        }
        else if (a == "--startup-generation") {
            const auto value = next("--startup-generation");
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                return 2;
            try { args.startup_generation = std::stoull(value); }
            catch (const std::exception&) { return 2; }
            if (args.startup_generation == 0) return 2;
        }
        else if (a == "--startup-socket") args.startup_socket = next("--startup-socket");
        else if (a == "--recover") args.recover = true;
        else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }
    if (!args.blank && args.config.empty()) {
        usage(argv[0]);
        return 2;
    }
    if (args.blank && (args.blank_size == 0 || args.blank_dir.empty())) {
        std::fprintf(stderr,
                     "--blank-size and --blank-dir are required for a blank "
                     "device\n");
        return 2;
    }
    if (args.blank && !args.config.empty()) {
        std::fprintf(stderr, "--config and --blank-size are mutually "
                             "exclusive\n");
        return 2;
    }

    if (args.control_fd >= 0 && args.startup_generation == 0) {
        std::fprintf(stderr, "supervised startup requires --startup-generation\n");
        return 2;
    }
    if (args.control_fd >= 0 && !args.startup_socket.empty()) {
        std::fprintf(stderr, "--startup-socket and --control-fd are mutually exclusive\n");
        return 2;
    }
    if (args.control_fd < 0) {
        if (args.startup_socket.empty())
            args.startup_socket = "@obd-device-" + std::to_string(::geteuid()) +
                                  "-" + std::to_string(::getpid());
        if (!obd::cmd::StartupSocket::valid_name(args.startup_socket)) {
            std::fprintf(stderr, "--startup-socket requires @NAME (at most 107 name bytes)\n");
            return 2;
        }
        if (args.startup_generation == 0) {
            // Fresh invocation identity prevents a delayed command aimed at a
            // reused PID/socket name from releasing a later owner.
            do {
                const auto n = ::getrandom(&args.startup_generation,
                                          sizeof(args.startup_generation), GRND_NONBLOCK);
                if (n < 0 && errno == EINTR) continue;
                if (n != static_cast<ssize_t>(sizeof(args.startup_generation))) {
                    std::fprintf(stderr, "cannot generate startup owner identity\n");
                    return 1;
                }
            } while (args.startup_generation == 0);
        }
    }
    elio::signal::signal_set sigs;
    sigs.add(SIGTERM).add(SIGINT);
    if (!sigs.block_all_threads()) {
        std::fprintf(stderr, "cannot block signals\n");
        return 1;
    }
    return elio::run(device_main, std::move(args));
}
