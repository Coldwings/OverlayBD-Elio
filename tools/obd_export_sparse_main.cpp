// Offline sparse upper delta export. Uses ordinary files; no daemon/device.
#include "format/sparse_rw.hpp"
#include <elio/runtime/async_main.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <string>

namespace {
struct Options { std::string source, destination, parent, tag; };
void usage() {
    std::cerr << "usage: obd-export-sparse SOURCE OUTPUT [--parent UUID] [--tag TAG]\n"
                 "Stop the source device first. OUTPUT is a separate immutable LSMT delta.\n";
}
elio::coro::task<int> export_main(const Options* options, nlohmann::json* reply) {
    const auto result = co_await obd::format::SparseRwLayer::export_file(
        options->source, options->destination, options->parent, options->tag);
    *reply = {{"path", options->destination}, {"sha256", result.sha256},
              {"size", result.size}, {"uuid", result.uuid}, {"parent_uuid", options->parent}};
    co_return 0;
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") { usage(); return 0; }
    if (argc < 3) { usage(); return 2; }
    Options options{argv[1], argv[2], {}, {}};
    bool parent_seen = false, tag_seen = false;
    for (int i = 3; i < argc; ++i) {
        const std::string option = argv[i];
        if (i + 1 == argc) { usage(); return 2; }
        if (option == "--parent" && !parent_seen) {
            options.parent = argv[++i]; parent_seen = true;
        } else if (option == "--tag" && !tag_seen) {
            options.tag = argv[++i]; tag_seen = true;
        } else { usage(); return 2; }
    }
    try {
        elio::runtime::run_config config;
        config.num_threads = 2;
        config.blocking_threads = 2;
        config.shutdown_timeout = std::chrono::milliseconds::max();
        nlohmann::json reply;
        const int rc = elio::run(config, export_main, &options, &reply);
        if (rc != 0) return rc;
        std::cout << reply.dump() << '\n';
        return std::cout.good() ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "obd-export-sparse: " << e.what() << '\n';
        return 1;
    }
}
