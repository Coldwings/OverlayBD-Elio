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
#include "format/merged_writable.hpp"
#include "format/sparse_rw.hpp"
#include "supervisor/device_control.hpp"
#include "supervisor/protocol.hpp"

#include "support.hpp"

#include <elio/log/macros.hpp>
#include <elio/runtime/async_main.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/runtime/spawn_blocking.hpp>
#include <elio/signal/signalfd.hpp>

#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
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
    // ADR-0014 modes 2/3: blank raw device (--blank-size/--blank-dir).
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
        // The stack served while the fake is "ready". Blank mode and the
        // record-path image mode assemble through the REAL image module so
        // the tests exercise genuine assembly minus the ublk device.
        std::optional<obd::image::OpenedImage> opened;
        // D3 shutdown guard + drain gate (created here so the SIGTERM
        // path below can set/drain them; see make_resize_apply).
        auto stopping = std::make_shared<std::atomic<bool>>(false);
        auto resize_gate = std::make_shared<std::mutex>();
        // D3 resize/checkpoint handles: `upper` is the image's writable
        // top (lsmt/sparse, sized at the headroom override); `blank_top`
        // is the blank device's merged writable top and `blank_merged`
        // that merged root itself (the resize executor grows the merged
        // DATA PLANE through it, exactly like the real device).
        std::shared_ptr<obd::format::WritableLayer> upper;
        obd::format::WritableLayer* blank_top = nullptr;
        obd::format::MergedWritable* blank_merged = nullptr;

        if (args.blank) {
            // ADR-0014 modes 2/3: assemble the real blank stack (empty
            // LSMT zero base + LSMT-RW upper) and drive a ublk-free
            // round-trip through it: an unwritten region reads zeroes,
            // then the payload write lands in the upper and reads back.
            obd::image::BlankDeviceSpec spec;
            spec.size = args.blank_size;
            spec.dir = args.blank_dir;
            opened.emplace(co_await obd::image::open_blank_device(spec));
            auto* w = dynamic_cast<obd::source::WritableBlobSource*>(
                opened->root.get());
            if (w == nullptr) {
                report(channel, DeviceStatus{"failed", "",
                                          "blank root is not writable"});
                co_return 1;
            }
            std::vector<uint8_t> zero_buf(kPayloadBytes);
            const ssize_t zr = co_await w->pread(zero_buf.data(),
                                                 zero_buf.size(),
                                                 512 * 32);  // unwritten
            if (zr != static_cast<ssize_t>(zero_buf.size()) ||
                !std::all_of(zero_buf.begin(), zero_buf.end(),
                             [](uint8_t b) { return b == 0; })) {
                report(channel, DeviceStatus{"failed", "",
                                          "blank unwritten read not zero"});
                co_return 1;
            }
            const auto payload =
                obd::test::pattern_bytes(kPayloadBytes, kPayloadSeed);
            const ssize_t wrc =
                co_await w->pwrite(payload.data(), payload.size(), 0);
            if (wrc != static_cast<ssize_t>(payload.size())) {
                report(channel, DeviceStatus{"failed", "",
                                          "blank payload write failed"});
                co_return 1;
            }
            std::vector<uint8_t> back(payload.size());
            const ssize_t brc =
                co_await w->pread(back.data(), back.size(), 0);
            if (brc != static_cast<ssize_t>(back.size()) ||
                std::memcmp(back.data(), payload.data(), back.size()) != 0) {
                report(channel, DeviceStatus{"failed", "",
                                          "blank payload readback mismatch"});
                co_return 1;
            }
            auto* mw = dynamic_cast<obd::format::MergedWritable*>(
                opened->root.get());
            if (mw == nullptr) {
                report(channel, DeviceStatus{"failed", "",
                                          "blank root is not merged writable"});
                co_return 1;
            }
            blank_top = &mw->writable_top();
            blank_merged = mw;
        } else {
            const obd::image::ImageConfig img =
                obd::image::ImageConfig::from_file(
                    args.config, obd::image::DownloadConfig{});

            // ADR-0013 record-path tests: a config WITH lowers makes the
            // fake a real image server (full assembly: registry -> record
            // tap -> LayerStore -> tar -> zfile -> lsmt -> merge) minus
            // the ublk device. The trace control loop then drives the real
            // recorder, and the scripted workload (run when a recording
            // starts) reads through the merged root so the tap captures
            // genuine remote reads from the test's mock registry.
            if (!img.lowers.empty()) {
                // Prefetch off: the structural head/tail warm-up (default
                // on) would pre-warm the whole small test layer at
                // bring-up and the scripted workload would hit local
                // extents, recording nothing.
                obd::image::GlobalConfig g;
                g.prefetch_enable = false;
                opened.emplace(co_await obd::image::open_image(img, g));
            }

            // D3 create-time headroom: mirror the real device's grow-only
            // validation against the fake's declared image size (the
            // merged lowers when present, else the writable upper's
            // kVsize) using the same single-source rule. A rejected
            // override fails create with the rule's message, exactly like
            // the real device — and BEFORE the writable upper is created
            // below.
            {
                const uint64_t declared =
                    opened.has_value()
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

            // With an lsmt/sparse upper: like the real device's writable
            // assembly, the writable layer is sized at the D3 headroom
            // override when given (so a commit of a headroom-created
            // device seals that declared size), else kVsize; the standard
            // payload is written at offset 0 (what the commit integration
            // reads back) and the file is left unsealed — checkpoint only
            // on SIGTERM, exactly like the real device.
            const uint64_t layer_vsize =
                args.virtual_size > 0 ? args.virtual_size : kVsize;
            if (img.writable() && img.upper.type == "lsmt") {
                std::filesystem::create_directories(img.upper.dir);
                auto lsmt = co_await obd::format::LsmtRwLayer::create(
                    img.upper.dir + "/overlaybd.rw", layer_vsize);
                const auto payload =
                    obd::test::pattern_bytes(kPayloadBytes, kPayloadSeed);
                const ssize_t w = co_await lsmt->pwrite(
                    payload.data(), payload.size(), 0);
                if (w != static_cast<ssize_t>(payload.size())) {
                    report(channel, DeviceStatus{"failed", "",
                                              "payload write failed"});
                    co_return 1;
                }
                upper = std::move(lsmt);
            } else if (img.writable()) {
                // Sparse: open the file so the device is plausible; sparse
                // state is durable via fiemap and never seals.
                std::filesystem::create_directories(img.upper.dir);
                auto sparse = co_await obd::format::SparseRwLayer::open(
                    img.upper.dir + "/overlaybd.sparse", layer_vsize);
                upper = std::move(sparse);
            }
        }
        report(channel, DeviceStatus{"ready", "/dev/ublkb70", ""});

        // Serve the supervisor's device commands on the control channel
        // (trace record path + D3 resize), exactly like the real
        // obd-device. The resize executor mirrors the real device's
        // grow-only semantics without a kernel: the loop enforces
        // grow-only against the fake's current size; apply records the
        // grow. A shared_ptr keeps `fake_size` alive for the detached
        // loop (which may outlive this coroutine's frame).
        if (args.control_fd >= 0) {
            // The fake's declared device size: the writable layer's
            // (override-size or kVsize) when present, else the assembled
            // image's size, else 0.
            const uint64_t base_size =
                upper != nullptr
                    ? upper->virtual_size()
                    : (opened.has_value() ? opened->virtual_size : 0);
            // Shared state for the detached control loop (which may
            // outlive this coroutine's frame): the tracked current size,
            // and — for the layer-backed fakes — a shared_ptr to the
            // writable layer so an in-flight grow can never outlive it.
            auto fake_size = std::make_shared<uint64_t>(base_size);
            // Shutdown guard + drain gate, exactly like the real device:
            // set/drained before the SIGTERM checkpoint below so a grow
            // can never rewrite the layer header after the trailer.
            obd::supervisor::DeviceControlHooks hooks;
            if (opened.has_value()) {
                auto* root = opened->root.get();
                hooks.on_start = [root]() {
                    elio::go([root]() -> elio::coro::task<void> {
                        char buf[4096];
                        for (const uint64_t off : {uint64_t{65536},
                                                   uint64_t{131072},
                                                   uint64_t{196608}}) {
                            const ssize_t r =
                                co_await root->pread(buf, sizeof(buf), off);
                            if (r < 0) {
                                ELIO_LOG_WARNING(
                                    "fake workload read at {} failed: {}",
                                    off, (int)-r);
                            }
                        }
                    });
                };
            }
            // D3 resize executor seam (no kernel): current_size returns
            // the fake's tracked size; apply is the SAME
            // make_resize_apply policy the real obd-device installs —
            // grow-only ordering (data plane via the REAL format grow
            // path, so a later checkpoint/commit seals the grown
            // declared size), plus the shutdown guard and gate that keep
            // a grow from interleaving with the shutdown checkpoint.
            hooks.resize.current_size = [fake_size]() -> uint64_t {
                return *fake_size;
            };
            // Data-plane grow for whichever stack this fake serves: the
            // image's writable upper (D3), or the blank device's merged
            // root (ADR-0014 modes 2/3) — the same seam the real
            // obd-device wires from merged_root.
            std::function<int(uint64_t)> grow_data_plane;
            if (upper != nullptr) {
                grow_data_plane = [upper](uint64_t bytes) {
                    return upper->grow(bytes);
                };
            } else if (blank_merged != nullptr) {
                grow_data_plane = [blank_merged](uint64_t bytes) {
                    return blank_merged->grow(bytes);
                };
            }
            hooks.resize.apply_resize = obd::supervisor::make_resize_apply(
                stopping, resize_gate, std::move(grow_data_plane),
                [fake_size](uint64_t bytes) {
                    *fake_size = bytes;
                    return bytes;
                });
            elio::go([channel, rec = opened.has_value()
                                     ? opened->recorder
                                     : obd::image::TraceRecorderPtr{},
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
        // Stop accepting resizes and drain any in-flight grow BEFORE the
        // checkpoint writes its trailer (the real device's contract;
        // make_resize_apply re-checks `stopping` under the gate).
        if (channel) {
            stopping->store(true, std::memory_order_release);
            co_await elio::spawn_blocking([&] {
                std::lock_guard<std::mutex> drain(*resize_gate);
            });
        }
        // ADR-0014: persist the writable upper's index on graceful
        // shutdown (LSMT-RW image upper, or the blank device's merged
        // writable top), exactly like the real obd-device, so the
        // supervisor's offline commit can seal it afterwards.
        if (upper != nullptr) {
            const int crc = co_await upper->checkpoint();
            if (crc != 0) {
                report(channel, DeviceStatus{"failed", "",
                                          "checkpoint failed"});
                co_return 1;
            }
        }
        if (blank_top != nullptr) {
            const int crc = co_await blank_top->checkpoint();
            if (crc != 0) {
                report(channel, DeviceStatus{"failed", "",
                                          "blank checkpoint failed"});
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
        else if (a == "--blank-size") {
            args.blank = true;
            args.blank_size = std::stoull(next("--blank-size"));
        } else if (a == "--blank-dir") args.blank_dir = next("--blank-dir");
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
    if (args.blank) {
        if (args.blank_size == 0 || args.blank_dir.empty()) {
            std::fprintf(stderr,
                         "usage: %s --blank-size BYTES --blank-dir PATH "
                         "[--control-fd N]\n",
                         argv[0]);
            return 2;
        }
    } else if (args.config.empty()) {
        std::fprintf(stderr, "usage: %s --config PATH [--control-fd N]\n",
                     argv[0]);
        return 2;
    }
    return elio::run(fake_main, std::move(args));
}
