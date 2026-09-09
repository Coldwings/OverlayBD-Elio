// obd-device: one isolated process serving one OverlayBD image as one ublk
// block device (ADR-0004). Spawned by obd-supervisor; reports lifecycle
// status as JSON-lines on the inherited control fd (protocol.hpp).
#include "image/config.hpp"
#include "image/image_file.hpp"
#include "format/merged_writable.hpp"
#include "supervisor/protocol.hpp"

#if defined(OBD_HAVE_UBLK) && OBD_HAVE_UBLK
#include "ublk/device.hpp"
#else
#error "obd-device requires OBD_ENABLE_UBLK=ON (ublk is the block-device backend)"
#endif

#include <elio/log/macros.hpp>
#include <elio/runtime/async_main.hpp>
#include <elio/signal/signalfd.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s --config PATH [--global PATH] [--control-fd N] "
                 "[--dev-id N] [--recover]\n",
                 argv0);
}

struct Args {
    std::string config;
    std::string global;
    int control_fd = -1;
    int dev_id = -1;
    bool recover = false;  // ADR-0010: attach to an existing device
};

void report(const Args& args, const obd::supervisor::DeviceStatus& st) {
    if (args.control_fd < 0) return;
    const std::string line = obd::supervisor::make_device_status(st);
    // Best-effort, single short write; the supervisor tolerates loss as EOF.
    const ssize_t w = ::write(args.control_fd, line.data(), line.size());
    (void)w;
}

elio::coro::task<int> device_main(Args args) {
    using obd::supervisor::DeviceStatus;
    report(args, DeviceStatus{"starting", "", ""});
    try {
        obd::image::GlobalConfig global =
            args.global.empty()
                ? obd::image::GlobalConfig{}
                : obd::image::GlobalConfig::from_file(args.global);
        const obd::image::ImageConfig img =
            obd::image::ImageConfig::from_file(args.config, global.download);

        auto opened = co_await obd::image::open_image(img, global);
        if (opened.virtual_size == 0 || opened.virtual_size % 512 != 0) {
            ELIO_LOG_ERROR("image virtual size {} is not sector aligned",
                           opened.virtual_size);
            report(args, DeviceStatus{"failed", "",
                                      "virtual size not sector aligned"});
            co_return 1;
        }

        obd::ublk::DeviceParams params;
        params.dev_sectors = opened.virtual_size / 512;
        params.read_only = !opened.writable;  // ADR-0008
        params.enable_recovery = global.ublk_recovery;  // ADR-0010
        if (args.dev_id >= 0) {
            params.dev_id = static_cast<uint32_t>(args.dev_id);
        }
        // ADR-0014: keep a handle on the writable top so the graceful-
        // shutdown path can checkpoint it (persist its index) after the
        // queues drain, enabling the supervisor's offline commit.
        obd::format::WritableLayer* writable_top = nullptr;
        if (opened.writable) {
            auto* mw = dynamic_cast<obd::format::MergedWritable*>(
                opened.root.get());
            if (mw != nullptr) writable_top = &mw->writable_top();
        }
        std::unique_ptr<obd::ublk::Device> dev;
        if (args.recover) {
            // ADR-0010: replace a crashed server for an existing device.
            if (args.dev_id < 0) {
                report(args, DeviceStatus{"failed", "",
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
        report(args, DeviceStatus{"ready", dev->bdev_path(), ""});

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
        dev->stop();
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
        dev.reset();
        report(args, DeviceStatus{"stopped", "", ""});
        co_return 0;
    } catch (const std::system_error& e) {
        ELIO_LOG_ERROR("device failed: {}", e.what());
        report(args, DeviceStatus{"failed", "", e.what()});
        co_return 1;
    } catch (const std::exception& e) {
        ELIO_LOG_ERROR("device failed: {}", e.what());
        report(args, DeviceStatus{"failed", "", e.what()});
        co_return 1;
    }
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
        else if (a == "--global") args.global = next("--global");
        else if (a == "--control-fd")
            args.control_fd = std::stoi(next("--control-fd"));
        else if (a == "--dev-id") args.dev_id = std::stoi(next("--dev-id"));
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
    if (args.config.empty()) {
        usage(argv[0]);
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
