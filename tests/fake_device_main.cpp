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
#include "format/hybrid_rw.hpp"
#include "format/merged_writable.hpp"
#include "format/sparse_rw.hpp"
#include "supervisor/device_control.hpp"
#include "supervisor/protocol.hpp"

#include "support.hpp"

#include <elio/log/macros.hpp>
#include <elio/net/uds.hpp>
#include <elio/runtime/async_main.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/runtime/spawn_blocking.hpp>
#include <elio/signal/signalfd.hpp>
#include <elio/sync/mutex.hpp>
#include <elio/time/timer.hpp>

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
#include <vector>
#include <unistd.h>

namespace {

/// Must match what tests/integration/test_commit.cpp expects to read back
/// from the sealed layer.
constexpr uint64_t kVsize = 512 * 64;
constexpr uint32_t kPayloadSeed = 4242;
constexpr size_t kPayloadBytes = 512 * 16;

class TraceWorkloads {
public:
    void spawn(obd::source::BlobSource* root) {
        std::lock_guard<std::mutex> lock(mu_);
        tasks_.reserve(tasks_.size() + 1);
        tasks_.push_back(elio::spawn(
            [root]() -> elio::coro::task<void> {
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
            }));
    }

    elio::coro::task<void> join() {
        std::vector<elio::coro::join_handle<void>> tasks;
        {
            std::lock_guard<std::mutex> lock(mu_);
            tasks.swap(tasks_);
        }
        for (auto& task : tasks) {
            while (!task.is_destroyed()) {
                co_await elio::time::sleep_for(std::chrono::milliseconds(1));
            }
            try {
                task.await_resume();
            } catch (const std::exception& e) {
                ELIO_LOG_WARNING("fake workload failed: {}", e.what());
            } catch (...) {
                ELIO_LOG_WARNING("fake workload failed: unknown exception");
            }
        }
        co_return;
    }

private:
    std::mutex mu_;
    std::vector<elio::coro::join_handle<void>> tasks_;
};

struct Args {
    std::string config;
    int control_fd = -1;
    uint64_t startup_generation = 0;
    /// D3 create-time headroom override (bytes; 0 = none), validated
    /// against the fake's declared image size with the same single-source
    /// rule (image::device_capacity_bytes) the real device uses.
    uint64_t virtual_size = 0;
    // ADR-0014 modes 2/3: blank raw device (--blank-size/--blank-dir).
    bool blank = false;
    bool recover = false;
    uint64_t blank_size = 0;
    std::string blank_dir;
    std::string blank_upper_type = "lsmt";
};

void report(const obd::supervisor::ControlChannelWriterPtr& channel,
            const obd::supervisor::DeviceStatus& st) {
    if (!channel) return;
    // Serialized with the trace control loop's writes (SOCK_STREAM has
    // no PIPE_BUF rule — see ControlChannelWriter).
    channel->write_line(obd::supervisor::make_device_status(st));
}

elio::coro::task<int> fake_body(
    Args args, obd::supervisor::StartupOwnerStatus* owner = nullptr) {
    using obd::supervisor::DeviceStatus;
    // ONE serialized writer for every channel writer (status reports,
    // trace replies, the expiry event) — see ControlChannelWriter.
    const obd::supervisor::ControlChannelWriterPtr channel =
        args.control_fd >= 0
            ? std::make_shared<obd::supervisor::ControlChannelWriter>(
                  args.control_fd)
            : nullptr;
    auto publish = [&](const obd::supervisor::ControlChannelWriterPtr& writer,
                       DeviceStatus status) {
        if (owner && !status.startup_owner) status.startup_owner = *owner;
        report(writer, status);
    };
    publish(channel, DeviceStatus{"starting", "", ""});
    try {
        // The stack served while the fake is "ready". Blank mode and the
        // record-path image mode assemble through the REAL image module so
        // the tests exercise genuine assembly minus the ublk device.
        std::optional<obd::image::OpenedImage> opened;
        // D3 shutdown guard + drain gate (created here so the SIGTERM
        // path below can set/drain them; see make_resize_apply).
        auto stopping = std::make_shared<std::atomic<bool>>(false);
        auto resize_gate = std::make_shared<std::mutex>();
        auto trace_start_stopping =
            std::make_shared<std::atomic<bool>>(false);
        auto trace_start_gate = std::make_shared<elio::sync::mutex>();
        auto trace_workloads = std::make_shared<TraceWorkloads>();
        auto close_trace_start_admission = [&]() -> elio::coro::task<void> {
            trace_start_stopping->store(true, std::memory_order_release);
            co_await trace_start_gate->lock();
            trace_start_gate->unlock();
        };
        // D3 resize/checkpoint handles: `upper` is the image's writable
        // top (lsmt/sparse, sized at the headroom override); `blank_top`
        // is the assembled blank/Sparse image's writable top and `blank_merged`
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
            spec.upper_type = args.blank_upper_type;
            spec.recover = args.recover;
            opened.emplace(co_await obd::image::open_blank_device(spec));
            auto* w = dynamic_cast<obd::source::WritableBlobSource*>(
                opened->root.get());
            if (w == nullptr) {
                publish(channel, DeviceStatus{"failed", "",
                                          "blank root is not writable"});
                co_return 1;
            }
            // 0xFF-prefilled: a no-op pread must fail the all-zero check.
            std::vector<uint8_t> zero_buf(kPayloadBytes, 0xFF);
            const ssize_t zr = co_await w->pread(zero_buf.data(),
                                                 zero_buf.size(),
                                                 512 * 32);  // unwritten
            if (zr != static_cast<ssize_t>(zero_buf.size()) ||
                !std::all_of(zero_buf.begin(), zero_buf.end(),
                             [](uint8_t b) { return b == 0; })) {
                publish(channel, DeviceStatus{"failed", "",
                                          "blank unwritten read not zero"});
                co_return 1;
            }
            const auto payload =
                obd::test::pattern_bytes(kPayloadBytes, kPayloadSeed);
            const ssize_t wrc =
                co_await w->pwrite(payload.data(), payload.size(), 0);
            if (wrc != static_cast<ssize_t>(payload.size())) {
                publish(channel, DeviceStatus{"failed", "",
                                          "blank payload write failed"});
                co_return 1;
            }
            std::vector<uint8_t> back(payload.size());
            const ssize_t brc =
                co_await w->pread(back.data(), back.size(), 0);
            if (brc != static_cast<ssize_t>(back.size()) ||
                std::memcmp(back.data(), payload.data(), back.size()) != 0) {
                publish(channel, DeviceStatus{"failed", "",
                                          "blank payload readback mismatch"});
                co_return 1;
            }
            auto* mw = dynamic_cast<obd::format::MergedWritable*>(
                opened->root.get());
            if (mw == nullptr) {
                publish(channel, DeviceStatus{"failed", "",
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
                opened.emplace(co_await obd::image::open_image(img, g, args.virtual_size, args.recover));
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
                if (args.virtual_size > 0 && declared > 0 &&
                    !(args.recover && img.upper.type == "hybrid")) {
                    std::string cap_error;
                    const uint64_t cap = obd::image::device_capacity_bytes(
                        declared, args.virtual_size, &cap_error);
                    if (cap == 0) {
                        publish(channel, DeviceStatus{"failed", "", cap_error});
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
                    publish(channel, DeviceStatus{"failed", "",
                                              "payload write failed"});
                    co_return 1;
                }
                upper = std::move(lsmt);
            } else if (img.writable() && img.upper.type == "hybrid") {
                const auto payload = obd::test::pattern_bytes(kPayloadBytes, kPayloadSeed);
                ssize_t written = 0;
                if (opened) {
                    auto* merged = dynamic_cast<obd::format::MergedWritable*>(opened->root.get());
                    if (!merged) co_return 1;
                    blank_top = &merged->writable_top();
                    blank_merged = merged;
                    written = co_await merged->pwrite(payload.data(), payload.size(), 0);
                } else {
                    std::filesystem::create_directories(img.upper.dir);
                    const auto directory = img.upper.dir + "/overlaybd.hybrid";
                    if (args.recover) {
                        upper = co_await obd::format::HybridRwLayer::open_existing(
                            directory + "/data", directory + "/index");
                    } else {
                        upper = co_await obd::format::HybridRwLayer::open_or_create(directory, layer_vsize);
                    }
                    written = co_await upper->pwrite(payload.data(), payload.size(), 0);
                }
                if (written != static_cast<ssize_t>(payload.size())) co_return 1;
            } else if (img.writable() && opened) {
                // Real image assembly already owns the Sparse inode. Reuse
                // its merged root for IO, resize and shutdown checkpoint.
                auto* merged = dynamic_cast<obd::format::MergedWritable*>(
                    opened->root.get());
                if (!merged) throw std::runtime_error("missing writable image root");
                blank_top = &merged->writable_top();
                blank_merged = merged;
                const auto payload =
                    obd::test::pattern_bytes(kPayloadBytes, kPayloadSeed);
                const auto written = co_await merged->pwrite(
                    payload.data(), payload.size(), 0);
                const auto zeroed = co_await merged->discard(kPayloadBytes, 512);
                if (written != static_cast<ssize_t>(payload.size()) || zeroed != 0)
                    throw std::runtime_error("sparse fixture payload write failed");
            } else if (img.writable()) {
                // A no-lower fixture owns its Sparse instance directly.
                std::filesystem::create_directories(img.upper.dir);
                auto sparse = co_await obd::format::SparseRwLayer::open(
                    img.upper.dir + "/overlaybd.sparse", layer_vsize);
                upper = std::move(sparse);
            }
        }
        obd::supervisor::StartupOwnerStatus ready_owner;
        ready_owner.generation = args.startup_generation;
        ready_owner.revision = 2;
        ready_owner.phase = "ready";
        ready_owner.device_id = 70;
        if (owner) *owner = ready_owner;
        // Test-only marker models a child predating parent-identity reports.
        const bool omit_parent = !args.config.empty() &&
            std::filesystem::exists(args.config + ".omit-parent-uuid");
        publish(channel, DeviceStatus{"ready", "/dev/ublkb70", "",
            args.startup_generation
                ? std::optional{ready_owner} : std::nullopt,
            omit_parent ? std::nullopt :
                (opened ? std::optional{opened->upper_parent_uuid}
                        : std::optional<std::string>{""})});

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
                hooks.on_start = [root, trace_workloads]() {
                    trace_workloads->spawn(root);
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
            hooks.trace_start_stopping = trace_start_stopping;
            hooks.trace_start_gate = trace_start_gate;
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
        // Finalize and drain the recorder, then park fills, before the
        // chain dies (same ordering contract as the real device). Do not
        // gate on recording(): expiry finalization has already lowered that
        // flag while the timer coroutine may still be alive.
        if (opened.has_value()) {
            co_await close_trace_start_admission();
            // `opened` owns the raw root captured by the synthetic trace
            // workload. Keep the image chain alive until every retained
            // workload coroutine has left its read path.
            co_await trace_workloads->join();
            if (opened->recorder) {
                const auto tres = co_await opened->recorder->stop("shutdown");
                if (!tres.ok &&
                    tres.error != "no trace recording in progress") {
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
                publish(channel, DeviceStatus{"failed", "",
                                          "checkpoint failed"});
                co_return 1;
            }
        }
        if (blank_top != nullptr) {
            const int crc = co_await blank_top->checkpoint();
            if (crc != 0) {
                publish(channel, DeviceStatus{"failed", "",
                                          "blank checkpoint failed"});
                co_return 1;
            }
        }
        publish(channel, DeviceStatus{"stopped", "", ""});
        // Test-only EOF-before-cleanup fixture. The checkpoint is already
        // valid, exactly like the real device, but channel EOF precedes
        // final Device/ublk control teardown. Hold that interval explicitly.
        const char* cleanup_env = std::getenv("OBD_TEST_CLEANUP_GATE");
        const std::string cleanup_gate = cleanup_env ? cleanup_env : "";
        if (!cleanup_gate.empty()) {
            if (args.control_fd >= 0) ::shutdown(args.control_fd, SHUT_RDWR);
            obd::test::write_file(cleanup_gate + "/closed", {});
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(10);
            while (!std::filesystem::exists(cleanup_gate + "/release")) {
                if (std::chrono::steady_clock::now() >= deadline) co_return 3;
                co_await elio::time::sleep_for(std::chrono::milliseconds(1));
            }
            obd::test::write_file(cleanup_gate + "/cleaned", {});
        }
        // Unblock the trace control loop (parked in a control-channel
        // read) only AFTER the checkpoint and the stopped report are
        // out. The supervisor must wait for actual process exit after
        // EOF, including any remaining device/kernel teardown. A parked
        // coroutine would otherwise stall the scheduler's teardown drain.
        if (args.control_fd >= 0) {
            ::shutdown(args.control_fd, SHUT_RDWR);
        }
        co_return 0;
    } catch (const std::exception& e) {
        publish(channel, DeviceStatus{"failed", "", e.what()});
        co_return 1;
    }
}

// The test child implements the same mandatory pre-publication grant. Read
// one byte at a time during the handoff so no bytes for the later sole
// command reader are consumed and discarded.
elio::coro::task<std::optional<nlohmann::json>> startup_command(
    elio::net::uds_stream& stream) {
    std::string line;
    for (;;) {
        char byte;
        auto read = co_await stream.read(&byte, 1);
        if (read.result != 1) co_return std::nullopt;
        if (byte == '\n') break;
        if (line.size() >= obd::supervisor::kMaxMessageBytes)
            co_return std::nullopt;
        line.push_back(byte);
    }
    auto command = nlohmann::json::parse(line, nullptr, false);
    if (!command.is_object()) co_return std::nullopt;
    co_return command;
}

elio::coro::task<int> fake_main(Args args) {
    using namespace obd::supervisor;
    if (args.startup_generation == 0) co_return co_await fake_body(std::move(args));
    auto channel = std::make_shared<ControlChannelWriter>(args.control_fd);
    elio::net::uds_stream reader(::dup(args.control_fd));
    StartupOwnerStatus owner;
    owner.generation = args.startup_generation;
    report(channel, DeviceStatus{"starting", "", "", owner});
    bool granted = false;
    bool retained = std::getenv("OBD_TEST_STARTUP_RETAIN") != nullptr;
    bool refuse_once = std::getenv("OBD_TEST_STARTUP_REFUSE_ONCE") != nullptr;
    for (;;) {
        auto command = co_await startup_command(reader);
        if (!command) co_return 2;
        const auto name = command->value("cmd", "");
        nlohmann::json reply{{"reply", name}, {"ok", true},
                             {"generation", owner.generation}};
        if (command->value("generation", uint64_t{0}) != owner.generation) {
            reply["ok"] = false;
        } else if (name == "startup_begin" &&
                   command->value("version", 0) == kStartupOwnerVersion) {
            granted = true;
        } else if (name == "startup_release") {
            const auto sequence = command->value("seq", uint64_t{0});
            if (sequence == 0 || sequence <= owner.release_sequence) {
                reply["ok"] = false;
            } else {
                owner.release_sequence = sequence;
                owner.revision += 2;
                if (refuse_once) {
                    refuse_once = false;
                    owner.phase = "retained";
                    owner.release = "refused";
                    owner.release_errno = EIO;
                } else {
                    owner.phase = "released";
                    owner.release = "released";
                    owner.release_errno = 0;
                    owner.raw_retired = owner.raw_published;
                }
            }
        } else if (name != "startup_status") {
            reply["ok"] = false;
        }
        reply["startup_owner"] = startup_owner_json(owner);
        channel->write_line(reply.dump() + "\n");
        if (owner.phase == "released") {
            report(channel, DeviceStatus{"stopped", "", "", owner});
            co_return 0;
        }
        if (!granted) continue;
        if (!retained) break;
        if (owner.phase != "retained") {
            owner.phase = "retained";
            owner.reason_errno = EIO;
            owner.raw_published = 2; // simulated debt; no kernel device exists
            owner.revision += 2;
        }
        report(channel, DeviceStatus{"failed", "",
            "injected retained startup failure", owner});
    }
    reader.close();
    const int result = co_await fake_body(args, &owner);
    // Early body errors before ready have no ublk loans in this fake.
    owner.phase = "released";
    owner.revision += 2;
    report(channel, DeviceStatus{result ? "failed" : "stopped", "", "", owner});
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
            // Parity with the real obd-device: strict decimal validation
            // (std::stoull would turn "-512" into 1.8e19 and sail past the
            // later zero/alignment checks), so a malformed size is the same
            // usage error here as it is in production.
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
                             "multiple of 512 bytes)\n",
                             v.c_str());
                return 2;
            }
            args.blank_size = parsed;
        } else if (a == "--blank-upper-type") {
            args.blank_upper_type = next("--blank-upper-type");
            if (args.blank_upper_type != "lsmt" && args.blank_upper_type != "hybrid") {
                std::fprintf(stderr, "--blank-upper-type must be lsmt or hybrid\n");
                return 2;
            }
        } else if (a == "--blank-dir") args.blank_dir = next("--blank-dir");
        else if (a == "--control-fd")
            args.control_fd = std::stoi(next("--control-fd"));
        else if (a == "--startup-generation")
            args.startup_generation = std::stoull(next("--startup-generation"));
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
        else if (a == "--recover") args.recover = true;
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
        // Parity with the real obd-device: the two creation modes are
        // mutually exclusive, so a test cannot accidentally exercise a
        // combination the production binary rejects.
        if (!args.config.empty()) {
            std::fprintf(stderr,
                         "--config and --blank-size are mutually exclusive\n");
            return 2;
        }
    } else if (args.config.empty()) {
        std::fprintf(stderr, "usage: %s --config PATH [--control-fd N]\n",
                     argv[0]);
        return 2;
    }
    return elio::run(fake_main, std::move(args));
}
