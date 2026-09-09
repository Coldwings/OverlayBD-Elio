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
#include "format/lsmt_rw.hpp"
#include "format/sparse_rw.hpp"
#include "supervisor/protocol.hpp"

#include "support.hpp"

#include <elio/log/macros.hpp>
#include <elio/runtime/async_main.hpp>
#include <elio/signal/signalfd.hpp>

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
};

void report(const Args& args, const obd::supervisor::DeviceStatus& st) {
    if (args.control_fd < 0) return;
    const std::string line = obd::supervisor::make_device_status(st);
    // Best-effort, single short write; the supervisor tolerates loss as EOF.
    const ssize_t w = ::write(args.control_fd, line.data(), line.size());
    (void)w;
}

elio::coro::task<int> fake_main(Args args) {
    using obd::supervisor::DeviceStatus;
    report(args, DeviceStatus{"starting", "", ""});
    try {
        const obd::image::ImageConfig img = obd::image::ImageConfig::from_file(
            args.config, obd::image::DownloadConfig{});

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
                report(args, DeviceStatus{"failed", "", "payload write failed"});
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
        report(args, DeviceStatus{"ready", "/dev/ublkb70", ""});

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
        if (lsmt) {
            const int crc = co_await lsmt->checkpoint();
            if (crc != 0) {
                report(args, DeviceStatus{"failed", "",
                                          "checkpoint failed"});
                co_return 1;
            }
        }
        report(args, DeviceStatus{"stopped", "", ""});
        co_return 0;
    } catch (const std::exception& e) {
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
        else if (a == "--control-fd")
            args.control_fd = std::stoi(next("--control-fd"));
        else if (a == "--global") (void)next("--global");  // accepted, unused
        else if (a == "--dev-id") (void)next("--dev-id");  // accepted, unused
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
