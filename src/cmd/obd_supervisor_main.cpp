// obd-supervisor: the per-node daemon supervising isolated obd-device
// processes (docs/supervisor.md, ADR-0004).
#include "supervisor/daemon.hpp"

#include <elio/runtime/async_main.hpp>
#include <elio/signal/signalfd.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [--socket PATH] [--global PATH] [--device-bin PATH]\n"
                 "          [--ready-timeout SEC] [--stop-timeout SEC]\n"
                 "          [--blank-dir PATH] [--mkfs-timeout SEC]\n",
                 argv0);
}

}  // namespace

int main(int argc, char** argv) {
    obd::supervisor::DaemonConfig cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (++i >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(2);
            }
            return argv[i];
        };
        if (a == "--socket") cfg.socket_path = next("--socket");
        else if (a == "--global") cfg.global_config = next("--global");
        else if (a == "--device-bin") cfg.device_bin = next("--device-bin");
        else if (a == "--ready-timeout")
            cfg.ready_timeout_sec = std::stoi(next("--ready-timeout"));
        else if (a == "--stop-timeout")
            cfg.stop_timeout_sec = std::stoi(next("--stop-timeout"));
        else if (a == "--blank-dir") cfg.blank_dir = next("--blank-dir");
        else if (a == "--mkfs-timeout")
            cfg.mkfs_timeout_sec = std::stoi(next("--mkfs-timeout"));
        else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    // signalfd model: block the handled signals in every thread before the
    // scheduler starts (the daemon re-installs per-set signalfds).
    elio::signal::signal_set sigs;
    sigs.add(SIGTERM).add(SIGINT).add(SIGCHLD);
    if (!sigs.block_all_threads()) {
        std::fprintf(stderr, "cannot block signals\n");
        return 1;
    }
    return elio::run(
        [&cfg]() -> elio::coro::task<int> {
            co_return co_await obd::supervisor::run_daemon(cfg);
        });
}
