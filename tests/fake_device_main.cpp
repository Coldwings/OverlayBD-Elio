// Test-only fake obd-device (tests/integration/test_commit.cpp). Speaks
// the real fd-3 lifecycle protocol (JSON-lines status reports) and, like
// the real obd-device, checkpoints its LSMT-RW upper on SIGTERM — but
// serves no ublk device. Signals are consumed via signalfd, so the
// inherited blocked signal mask (the tests block SIGTERM/SIGINT/SIGCHLD
// process-wide for the daemon's signalfd model) works exactly as it does
// for the real device.
//
// This is what lets the commit integration test pin stop-then-seal: the
// upper's on-disk checkpoint exists ONLY after the supervisor's SIGTERM,
// so a commit that sealed without stopping the device finds no checkpoint
// and fails.
#include "image/config.hpp"
#include "image/image_file.hpp"
#include "format/lsmt_rw.hpp"
#include "format/sparse_rw.hpp"
#include "supervisor/device_control.hpp"
#include "supervisor/protocol.hpp"

#include "support.hpp"

#include <elio/log/macros.hpp>
#include <elio/runtime/async_main.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/signal/signalfd.hpp>

#include <sys/socket.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {

/// Must match what tests/integration/test_commit.cpp expects to read back
/// from the sealed layer.
constexpr uint64_t kVsize = 512 * 64;
constexpr uint32_t kPayloadSeed = 4242;
constexpr size_t kPayloadBytes = 512 * 16;

struct Args {
    std::string config;
    int control_fd = -1;
    /// D3 create-time headroom override (bytes; 0 = none), validated
    /// against the fake's declared image size with the same single-source
    /// rule (image::device_capacity_bytes) the real device uses.
    uint64_t virtual_size = 0;
};

void report(const obd::supervisor::ControlChannelWriterPtr& channel,
            const obd::supervisor::DeviceStatus& st) {
    if (!channel) return;
    // Serialized with the trace control loop's writes (SOCK_STREAM has
    // no PIPE_BUF rule — see ControlChannelWriter).
    channel->write_line(obd::supervisor::make_device_status(st));
}

elio::coro::task<int> fake_main(Args args) {
    using obd::supervisor::DeviceStatus;
    // ONE serialized writer for every channel writer (status reports,
    // trace replies, the expiry event) — see ControlChannelWriter.
    const obd::supervisor::ControlChannelWriterPtr channel =
        args.control_fd >= 0
            ? std::make_shared<obd::supervisor::ControlChannelWriter>(
                  args.control_fd)
            : nullptr;
    report(channel, DeviceStatus{"starting", "", ""});
    try {
        const obd::image::ImageConfig img = obd::image::ImageConfig::from_file(
            args.config, obd::image::DownloadConfig{});

        // ADR-0013 record-path tests: a config WITH lowers makes the fake
        // a real image server (full assembly: registry -> record tap ->
        // LayerStore -> tar -> zfile -> lsmt -> merge) minus the ublk
        // device. The trace control loop then drives the real recorder,
        // and the scripted workload (run when a recording starts) reads
        // through the merged root so the tap captures genuine remote
        // reads from the test's mock registry.
        std::optional<obd::image::OpenedImage> opened;
        if (!img.lowers.empty()) {
            // Prefetch off: the structural head/tail warm-up (default
            // on) would pre-warm the whole small test layer at bring-up
            // and the scripted workload would hit local extents,
            // recording nothing.
            obd::image::GlobalConfig g;
            g.prefetch_enable = false;
            opened.emplace(co_await obd::image::open_image(img, g));
        }

        // With an lsmt upper: write the payload and leave the file
        // unsealed (checkpoint only on SIGTERM, like the real device).
        std::unique_ptr<obd::format::LsmtRwLayer> lsmt;
        if (img.writable() && img.upper.type == "lsmt") {
            std::filesystem::create_directories(img.upper.dir);
            lsmt = co_await obd::format::LsmtRwLayer::create(
                img.upper.dir + "/overlaybd.rw", kVsize);
            const auto payload =
                obd::test::pattern_bytes(kPayloadBytes, kPayloadSeed);
            const ssize_t w = co_await lsmt->pwrite(payload.data(),
                                                  payload.size(), 0);
            if (w != static_cast<ssize_t>(payload.size())) {
                report(channel, DeviceStatus{"failed", "", "payload write failed"});
                co_return 1;
            }
        } else if (img.writable()) {
            // Sparse: open the file so the device is plausible; sparse
            // state is durable via fiemap and never seals.
            std::filesystem::create_directories(img.upper.dir);
            auto sparse = co_await obd::format::SparseRwLayer::open(
                img.upper.dir + "/overlaybd.sparse", kVsize);
            (void)sparse;
        }
        // D3 create-time headroom: mirror the real device's grow-only
        // validation against the fake's declared image size (the merged
        // lowers when present, else the writable upper's kVsize) using
        // the same single-source rule. A rejected override fails create
        // with the rule's message, exactly like the real device.
        {
            const uint64_t declared = opened.has_value()
                                          ? opened->virtual_size
                                          : (img.writable() ? kVsize : 0);
            if (args.virtual_size > 0 && declared > 0) {
                std::string cap_error;
                const uint64_t cap = obd::image::device_capacity_bytes(
                    declared, args.virtual_size, &cap_error);
                if (cap == 0) {
                    report(channel, DeviceStatus{"failed", "", cap_error});
                    co_return 1;
                }
            }
        }
        report(channel, DeviceStatus{"ready", "/dev/ublkb70", ""});

        // Trace command channel (protocol v3), like the real obd-device.
        // The workload hook runs inside the recording window: a
        // deterministic read pattern through the merged root. The offsets
        // target MIDDLE extents (assembly probes pre-warm the header
        // extent and the trailer/index extents; reads there would be
        // local hits and record nothing).
        if (opened.has_value() && args.control_fd >= 0) {
            auto* root = opened->root.get();
            obd::supervisor::DeviceControlHooks hooks;
            hooks.on_start = [root]() {
                elio::go([root]() -> elio::coro::task<void> {
                    char buf[4096];
                    for (const uint64_t off : {uint64_t{65536},
                                               uint64_t{131072},
                                               uint64_t{196608}}) {
                        const ssize_t r =
                            co_await root->pread(buf, sizeof(buf), off);
                        if (r < 0) {
                            ELIO_LOG_WARNING("fake workload read at {} "
                                             "failed: {}", off, (int)-r);
                        }
                    }
                });
            };
            elio::go([channel, rec = opened->recorder,
                      hooks = std::move(hooks)]() mutable
                     -> elio::coro::task<void> {
                co_await obd::supervisor::run_device_control(
                    channel, rec, std::move(hooks));
            });
        }

        // Serve until SIGTERM/SIGINT (pending under the inherited blocked
        // mask; signalfd consumes it).
        elio::signal::signal_set term_set;
        term_set.add(SIGTERM).add(SIGINT);
        elio::signal::signal_fd term_fd(term_set);
        for (;;) {
            auto info = co_await term_fd.wait();
            if (info && (info->signo == SIGTERM || info->signo == SIGINT)) {
                break;
            }
        }
        // Finalize any active recording, then park fills, before the
        // chain dies (same ordering contract as the real device).
        if (opened.has_value()) {
            if (opened->recorder && opened->recorder->recording()) {
                const auto tres = co_await opened->recorder->stop("shutdown");
                if (!tres.ok) {
                    ELIO_LOG_ERROR("fake: trace finalize failed: {}",
                                   tres.error);
                }
            }
            co_await obd::image::park_image_fills(*opened);
        }
        if (lsmt) {
            const int crc = co_await lsmt->checkpoint();
            if (crc != 0) {
                report(channel, DeviceStatus{"failed", "",
                                          "checkpoint failed"});
                co_return 1;
            }
        }
        report(channel, DeviceStatus{"stopped", "", ""});
        // Unblock the trace control loop (parked in a control-channel
        // read) only AFTER the checkpoint and the stopped report are
        // out: shutdown(2) gives the supervisor an immediate EOF, and
        // the supervisor treats EOF as device-gone — an earlier call
        // would race the checkpoint (a commit seal could observe "no
        // valid shutdown checkpoint"). A parked coroutine would
        // otherwise stall the scheduler's teardown drain.
        if (args.control_fd >= 0) {
            ::shutdown(args.control_fd, SHUT_RDWR);
        }
        co_return 0;
    } catch (const std::exception& e) {
        report(channel, DeviceStatus{"failed", "", e.what()});
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
        else if (a == "--control-fd")
            args.control_fd = std::stoi(next("--control-fd"));
        else if (a == "--global") (void)next("--global");  // accepted, unused
        else if (a == "--dev-id") (void)next("--dev-id");  // accepted, unused
        else if (a == "--virtual-size") {
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
        else if (a == "--recover") { /* accepted, unused */ }
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }
    if (args.config.empty()) {
        std::fprintf(stderr, "usage: %s --config PATH [--control-fd N]\n",
                     argv[0]);
        return 2;
    }
    return elio::run(fake_main, std::move(args));
}
