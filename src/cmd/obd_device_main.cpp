// obd-device: one isolated process serving one OverlayBD image as one ublk
// block device (ADR-0004). Spawned by obd-supervisor; reports lifecycle
// status as JSON-lines on the inherited control fd (protocol.hpp).
#include "image/config.hpp"
#include "image/image_file.hpp"
#include "format/merged_writable.hpp"
#include "common/errors.hpp"
#include "supervisor/device_control.hpp"
#include "supervisor/protocol.hpp"

#if defined(OBD_HAVE_UBLK) && OBD_HAVE_UBLK
#include "ublk/device.hpp"
#else
#error "obd-device requires OBD_ENABLE_UBLK=ON (ublk is the block-device backend)"
#endif

#include <elio/log/macros.hpp>
#include <elio/runtime/async_main.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/runtime/spawn_blocking.hpp>
#include <elio/signal/signalfd.hpp>

#include <sys/socket.h>

#include <atomic>
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
                 "[--recover] [--virtual-size BYTES]\n",
                 argv0);
}

struct Args {
    std::string config;
    std::string global;
    int control_fd = -1;
    int dev_id = -1;
    bool recover = false;  // ADR-0010: attach to an existing device
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

void report(const obd::supervisor::ControlChannelWriterPtr& channel,
            const obd::supervisor::DeviceStatus& st) {
    if (!channel) return;
    // Serialized with the trace control loop's writes (SOCK_STREAM has
    // no PIPE_BUF rule — see ControlChannelWriter).
    channel->write_line(obd::supervisor::make_device_status(st));
}

elio::coro::task<int> device_main(Args args) {
    using obd::supervisor::DeviceStatus;
    // ONE serialized writer for every channel writer (status reports,
    // trace replies, the expiry event) — see ControlChannelWriter.
    const obd::supervisor::ControlChannelWriterPtr channel =
        args.control_fd >= 0
            ? std::make_shared<obd::supervisor::ControlChannelWriter>(
                  args.control_fd)
            : nullptr;
    report(channel, DeviceStatus{"starting", "", ""});
    // Owners survive the protected body, so exceptions use the same awaited
    // cleanup as a normal shutdown instead of destructing on this worker.
    std::shared_ptr<obd::ublk::Device> dev;
    obd::image::OpenedImage opened;
    auto stopping = std::make_shared<std::atomic<bool>>(false);
    auto resize_gate = std::make_shared<std::mutex>();
    std::optional<elio::coro::join_handle<void>> control;
    int result = 0;
    auto finish_trace_recording = [&]() -> elio::coro::task<void> {
        if (!opened.recorder) co_return;
        const auto tres = co_await opened.recorder->stop("shutdown");
        if (!tres.ok && tres.error != "no trace recording in progress") {
            ELIO_LOG_ERROR("trace finalize on shutdown failed: {}",
                           tres.error);
        }
    };
    try {
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
                co_return 1;
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
            co_return 1;
        }
        uint64_t dev_bytes = opened.virtual_size;
        if (args.virtual_size > 0) {
            if (args.virtual_size % 512 != 0) {
                ELIO_LOG_ERROR(
                    "virtual-size override {} is not sector aligned",
                    args.virtual_size);
                report(channel, DeviceStatus{"failed", "",
                                          "virtual_size not sector aligned"});
                co_return 1;
            }
            if (!opened.writable) {
                std::string cap_error;
                dev_bytes = obd::image::device_capacity_bytes(
                    opened.virtual_size, args.virtual_size, &cap_error);
                if (dev_bytes == 0) {
                    ELIO_LOG_ERROR("virtual-size override rejected: {}",
                                   cap_error);
                    report(channel, DeviceStatus{"failed", "", cap_error});
                    co_return 1;
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
                co_return 1;
            }
            dev = co_await obd::ublk::Device::attach(
                static_cast<uint32_t>(args.dev_id), params,
                std::move(opened.root));
        } else {
            dev = co_await obd::ublk::Device::create(params,
                                                     std::move(opened.root));
        }
        report(channel, DeviceStatus{"ready", dev->bdev_path(), ""});

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
            control.emplace(elio::spawn(obd::supervisor::run_device_control,
                                        channel, opened.recorder,
                                        std::move(hooks)));
        }

        // Serve until SIGTERM/SIGINT.
        elio::signal::signal_set term_set;
        term_set.add(SIGTERM).add(SIGINT);
        elio::signal::signal_fd term_fd(term_set);
        for (;;) {
            auto info = co_await term_fd.wait();
            if (info && (info->signo == SIGTERM || info->signo == SIGINT)) {
                break;
            }
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
        co_await obd::image::park_image_fills(opened);
        // Keep our owning reference until the control task's captures have
        // been destroyed; its last reference must not destroy Device on a worker.
        report(channel, DeviceStatus{"stopped", "", ""});
        // Unblock the trace control loop only AFTER the checkpoint and
        // the stopped report are out: it is parked in a control-channel
        // read that only EOF/error can end, and a parked coroutine
        // stalls the scheduler's teardown drain — but shutdown(2) gives
        // the supervisor an immediate EOF, and EOF marks the device
        // gone, so an earlier call would race the checkpoint (a commit
        // seal could observe "no valid shutdown checkpoint").
        if (args.control_fd >= 0) {
            ::shutdown(args.control_fd, SHUT_RDWR);
        }
    } catch (const std::system_error& e) {
        ELIO_LOG_ERROR("device failed: {}", e.what());
        report(channel, DeviceStatus{"failed", "", e.what()});
        result = 1;
    } catch (const std::exception& e) {
        ELIO_LOG_ERROR("device failed: {}", e.what());
        report(channel, DeviceStatus{"failed", "", e.what()});
        result = 1;
    } catch (...) {
        ELIO_LOG_ERROR("device failed: unknown exception");
        report(channel, DeviceStatus{"failed", "", "unknown exception"});
        result = 1;
    }
    // Also covers exceptions after successful create/attach. First exclude
    // resizes, then finish the control body before waiting for frame teardown:
    // the body may need the sole blocking thread to complete a resize.
    stopping->store(true, std::memory_order_release);
    co_await elio::spawn_blocking([&] {
        std::lock_guard<std::mutex> drain(*resize_gate);
    });
    if (control) {
        ::shutdown(args.control_fd, SHUT_RDWR);
        auto& control_task = *control;
        try {
            co_await control_task;
        } catch (const std::exception& e) {
            ELIO_LOG_ERROR("device control failed during shutdown: {}", e.what());
            result = 1;
        } catch (...) {
            ELIO_LOG_ERROR("device control failed during shutdown: unknown exception");
            result = 1;
        }
        co_await elio::spawn_blocking([&] { control->wait_destroyed(); });
        control.reset();
    }
    if (dev) {
        co_await dev->stop_async();
        co_await finish_trace_recording();
        // The normal path already parks fills before reporting stopped.
        // Retain the same owners during exceptional cleanup as well.
        if (result != 0) co_await obd::image::park_image_fills(opened);
        co_await elio::spawn_blocking([&] { dev.reset(); });
    }
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

    elio::signal::signal_set sigs;
    sigs.add(SIGTERM).add(SIGINT);
    if (!sigs.block_all_threads()) {
        std::fprintf(stderr, "cannot block signals\n");
        return 1;
    }
    return elio::run(device_main, std::move(args));
}
